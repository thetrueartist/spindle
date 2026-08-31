// Spindle - Windows filesystem scanner.
//
// Walks a volume with FindFirstFileExW directly rather than via any shell or
// .NET layer. Sizes come out of WIN32_FIND_DATA, so no handle is ever opened
// on a scanned file: nothing is read, nothing is locked, and files held under
// an exclusive kernel lock (pagefile.sys, hiberfil.sys) still report a size.

#include "spindle.h"
#include "sync.h"
#include "workqueue.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <process.h>
#include <shlobj.h>    // SHGetFolderPathW, for the cache directory
#include <aclapi.h>    // SetNamedSecurityInfoW, for taking ownership
#include <sddl.h>
#include <restartmanager.h>   // RmStartSession: who has this file open

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <vector>

namespace spindle {
namespace {

// Guard against a pathological or hostile directory structure driving the
// worker queue without bound. 512 is far past anything a real volume reaches.
constexpr int kMaxDepth = 512;

// Prefix a path so the Win32 API skips MAX_PATH normalisation entirely.
// Paths handed here are always canonical: they are either a caller-supplied
// root or built from FindFirstFile output, never user-typed relative fragments.
std::wstring ExtendedPath(const std::wstring& path) {
    if (path.size() >= 4 && path.compare(0, 4, L"\\\\?\\") == 0) {
        return path;
    }
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    return L"\\\\?\\" + path;
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& leaf) {
    if (dir.empty()) return leaf;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + leaf;
    return dir + L'\\' + leaf;
}

struct Task {
    Node*        node = nullptr;   // stable: parent reserved its children first
    std::wstring path;
    int          depth = 0;
};

using Queue = WorkQueue<Task>;

struct Shared {
    Queue                     queue;
    Progress*                 progress = nullptr;
    Lock                      deniedLock;
    std::vector<std::wstring> denied;
    std::atomic<uint64_t>     deniedCount{0};
    std::atomic<uint64_t>     dirCount{0};
    std::atomic<uint64_t>     fileCount{0};
    std::atomic<uint64_t>     byteCount{0};
    std::atomic<uint64_t>     cloudFiles{0};
    std::atomic<uint64_t>     cloudBytes{0};
    // Set if a worker aborted on an exception, so the caller can say so
    // rather than silently returning a truncated tree.
    std::atomic<bool>         faulted{false};
};

void RecordDenied(Shared& sh, const std::wstring& path) {
    sh.deniedCount.fetch_add(1, std::memory_order_relaxed);
    Held h(sh.deniedLock);
    if (sh.denied.size() < kMaxDeniedRecorded) sh.denied.push_back(path);
}

// Closes the search handle on every path out of ScanOne, including the one
// where a container allocation throws part-way through the enumeration.
// Sixteen threads leaking a find handle per failure exhausts them quickly.
class FindHandle {
public:
    explicit FindHandle(HANDLE h) : h_(h) {}
    ~FindHandle() { if (h_ != INVALID_HANDLE_VALUE) FindClose(h_); }
    FindHandle(const FindHandle&) = delete;
    FindHandle& operator=(const FindHandle&) = delete;
    HANDLE get() const { return h_; }

private:
    HANDLE h_;
};

// Scan one directory. Children are sized and reserved before any pointer into
// the vector escapes, so the tasks queued below hold pointers that stay valid
// for the rest of the scan.
void ScanOne(const Task& task, Shared& sh) {
    const std::wstring pattern = JoinPath(ExtendedPath(task.path), L"*");

    WIN32_FIND_DATAW fd{};
    const FindHandle find(FindFirstFileExW(pattern.c_str(), FindExInfoBasic,
                                           &fd, FindExSearchNameMatch, nullptr,
                                           FIND_FIRST_EX_LARGE_FETCH));
    const HANDLE h = find.get();
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION) {
            RecordDenied(sh, task.path);
        }
        return;
    }

    struct Entry {
        std::wstring name;
        uint64_t     size = 0;
        bool         dir  = false;
        bool         cloud = false;
    };
    std::vector<Entry> entries;
    entries.reserve(64);

    uint64_t localFiles = 0;
    uint64_t localBytes = 0;
    uint64_t localCloudFiles = 0;
    uint64_t localCloudBytes = 0;

    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) {
            continue;
        }

        const bool isDir =
            (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const bool isLink =
            (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

        // Never traverse a reparse point. Junctions and symlinks would
        // otherwise let the walk loop indefinitely or count a subtree twice,
        // and a crafted junction is a trivial way to make a scanner hang.
        if (isDir && isLink) continue;

        Entry e;
        e.name = n;
        e.dir  = isDir;
        // A cloud placeholder reports a full size but is not stored here.
        // Recognised from the attributes alone - reading one would make
        // Windows fetch the whole file over the network, which is the
        // rudest thing a disk usage tool could do.
        e.cloud = (fd.dwFileAttributes &
                   (FILE_ATTRIBUTE_RECALL_ON_OPEN |
                    FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS |
                    FILE_ATTRIBUTE_OFFLINE)) != 0;
        if (!isDir) {
            e.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) |
                     static_cast<uint64_t>(fd.nFileSizeLow);
            localBytes = SatAdd(localBytes, e.size);
            ++localFiles;
            if (e.cloud) {
                localCloudBytes = SatAdd(localCloudBytes, e.size);
                ++localCloudFiles;
            }
        }
        entries.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));

    if (entries.empty()) return;

    // Single allocation, then no further reallocation: the addresses handed to
    // the queue below remain valid because nothing else ever touches this
    // vector's capacity.
    Node& parent = *task.node;
    parent.children.reserve(entries.size());

    std::vector<Task> subdirs;
    subdirs.reserve(8);

    for (Entry& e : entries) {
        Node child(std::move(e.name), e.dir);
        child.cloudOnly = e.cloud;
        if (e.dir) {
            child.cat = Cat::Directory;
        } else {
            child.size  = e.size;
            child.files = 1;
            child.cat   = CategoryForFile(child.name);
        }
        parent.children.push_back(std::move(child));
    }

    if (task.depth < kMaxDepth) {
        for (Node& c : parent.children) {
            if (!c.dir) continue;
            subdirs.push_back(
                Task{&c, JoinPath(task.path, c.name), task.depth + 1});
        }
    }

    sh.fileCount.fetch_add(localFiles, std::memory_order_relaxed);
    sh.byteCount.fetch_add(localBytes, std::memory_order_relaxed);
    if (localCloudFiles > 0) {
        sh.cloudFiles.fetch_add(localCloudFiles, std::memory_order_relaxed);
        sh.cloudBytes.fetch_add(localCloudBytes, std::memory_order_relaxed);
    }
    sh.dirCount.fetch_add(1, std::memory_order_relaxed);

    if (sh.progress) {
        sh.progress->files.store(sh.fileCount.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
        sh.progress->dirs.store(sh.dirCount.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
        sh.progress->bytes.store(sh.byteCount.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
    }

    sh.queue.PushBatch(subdirs);
}

void WorkerLoop(Shared* sh) {
    Task t;
    while (sh->queue.Pop(t)) {
        if (sh->progress &&
            sh->progress->cancel.load(std::memory_order_relaxed)) {
            sh->queue.Done();
            sh->queue.Stop();
            return;
        }
        ScanOne(t, *sh);
        sh->queue.Done();
    }
}

// Thread entry point. The catch-all is not defensive padding: an exception
// escaping a thread function calls std::terminate, which kills the process
// outright with no dialog and no chance to save anything. A scan of a large
// volume performs millions of allocations, so a bad_alloc here has to unwind
// into a reported failure rather than take the application down.
unsigned __stdcall WorkerThunk(void* param) {
    Shared* sh = static_cast<Shared*>(param);
    try {
        WorkerLoop(sh);
    } catch (...) {
        sh->faulted.store(true, std::memory_order_relaxed);
        // Wake anyone waiting: this worker will not be calling Done() again,
        // and without a nudge the remaining threads block until the busy count
        // happens to reach zero on its own.
        sh->queue.Done();
        sh->queue.Stop();
    }
    return 0;
}

// Post-order size rollup, done iteratively. A recursive version would risk the
// stack on a deeply nested tree, and extended-length paths permit far more
// nesting than MAX_PATH ever did.
void RollUp(Node& root) {
    struct Frame { Node* node; size_t next; };

    std::vector<Frame> stack;
    stack.push_back(Frame{&root, 0});

    while (!stack.empty()) {
        Frame& f = stack.back();
        if (f.next < f.node->children.size()) {
            Node* child = &f.node->children[f.next];
            ++f.next;
            if (child->dir) stack.push_back(Frame{child, 0});
            continue;
        }

        Node* n = f.node;
        if (n->dir) {
            uint64_t total = 0;
            uint32_t files = 0;
            for (const Node& c : n->children) {
                total = SatAdd(total, c.size);
                files += c.files;
            }
            n->size  = total;
            n->files = files;
        }
        stack.pop_back();
    }
}

void SortTree(Node& root) {
    struct Frame { Node* node; size_t next; };

    std::vector<Frame> stack;
    stack.push_back(Frame{&root, 0});

    while (!stack.empty()) {
        Frame& f = stack.back();
        if (f.next == 0) {
            std::sort(f.node->children.begin(), f.node->children.end(),
                      [](const Node& a, const Node& b) {
                          return a.size > b.size;
                      });
        }
        if (f.next < f.node->children.size()) {
            Node* child = &f.node->children[f.next];
            ++f.next;
            if (child->dir && !child->children.empty()) {
                stack.push_back(Frame{child, 0});
            }
            continue;
        }
        stack.pop_back();
    }
}

}  // namespace

ScanResult Scan(const std::wstring& root, unsigned threads,
                Progress* progress) {
    const auto t0 = std::chrono::steady_clock::now();

    ScanResult result;
    result.root.name = root;
    result.root.dir  = true;
    result.root.cat  = Cat::Directory;

    if (root.empty()) return result;

    if (threads == 0) {
        SYSTEM_INFO si{};
        GetNativeSystemInfo(&si);
        threads = si.dwNumberOfProcessors;
    }
    if (threads == 0) threads = 4;
    threads = std::min(threads, 64u);

    // Fast path first. It needs a local NTFS volume and elevation; when any
    // of that is missing it returns false having done nothing, and the
    // directory walk below runs instead. The user is not told which ran,
    // because the answer is the same either way.
    if (ScanMft(root, progress, result)) {
        RollUp(result.root);
        SortTree(result.root);
        result.stats.bytes   = result.root.size;
        result.stats.usedMft = true;

        const auto tMft = std::chrono::steady_clock::now();
        result.stats.seconds =
            std::chrono::duration<double>(tMft - t0).count();
        if (progress) progress->done.store(true, std::memory_order_release);
        return result;
    }

    // A failed MFT attempt may have left a partial tree behind.
    result.root.children.clear();
    result.root.size = 0;
    result.root.files = 0;

    Shared sh;
    sh.progress = progress;
    sh.queue.Push(Task{&result.root, root, 0});

    // _beginthreadex rather than CreateThread: the CRT needs its per-thread
    // state set up, and rather than std::thread because the win32 MinGW
    // threading model is the thing this scanner was tripping over.
    std::vector<HANDLE> pool;
    pool.reserve(threads);

    for (unsigned i = 0; i < threads; ++i) {
        const uintptr_t h =
            _beginthreadex(nullptr, 0, WorkerThunk, &sh, 0, nullptr);
        if (h == 0) break;   // out of threads; those already started continue
        pool.push_back(reinterpret_cast<HANDLE>(h));
    }

    if (pool.empty()) {
        // No worker could start, so run the walk inline rather than returning
        // an empty tree and reporting a successful scan of nothing.
        WorkerThunk(&sh);
    }

    // WaitForMultipleObjects caps at MAXIMUM_WAIT_OBJECTS (64) and the thread
    // count is clamped to that, but waiting one at a time is just as correct
    // and does not depend on the cap.
    for (HANDLE h : pool) {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }

    RollUp(result.root);
    SortTree(result.root);

    result.stats.bytes       = result.root.size;
    result.stats.fileCount   = sh.fileCount.load();
    result.stats.dirCount    = sh.dirCount.load();
    result.stats.deniedCount = sh.deniedCount.load();
    result.stats.faulted     = sh.faulted.load();
    result.stats.cloudFiles  = sh.cloudFiles.load();
    result.stats.cloudBytes  = sh.cloudBytes.load();

    {
        Held h(sh.deniedLock);
        result.denied = std::move(sh.denied);
    }

    const auto t1 = std::chrono::steady_clock::now();
    result.stats.seconds = std::chrono::duration<double>(t1 - t0).count();

    if (progress) progress->done.store(true, std::memory_order_release);
    return result;
}

// ------------------------------------------------------------------ volumes

std::vector<Volume> EnumerateVolumes() {
    std::vector<Volume> out;

    const DWORD mask = GetLogicalDrives();
    if (mask == 0) return out;

    // Suppress the "no disk in drive" dialog for empty removable bays.
    const UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS);

    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) continue;

        wchar_t rootPath[4] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};

        const UINT type = GetDriveTypeW(rootPath);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE &&
            type != DRIVE_REMOTE) {
            continue;
        }

        Volume v;
        v.path = rootPath;

        wchar_t label[MAX_PATH + 1] = {};
        wchar_t fsName[64] = {};
        if (GetVolumeInformationW(rootPath, label, MAX_PATH, nullptr, nullptr,
                                  nullptr, fsName, 63)) {
            v.label = label;
            v.fs    = fsName;
            v.ready = true;
        }

        ULARGE_INTEGER avail{}, total{}, freeBytes{};
        if (GetDiskFreeSpaceExW(rootPath, &avail, &total, &freeBytes)) {
            v.capacity = total.QuadPart;
            v.free     = avail.QuadPart;
            v.ready    = true;
        }

        if (v.ready) out.push_back(std::move(v));
    }

    SetErrorMode(oldMode);
    return out;
}

}  // namespace spindle

namespace spindle {

bool WriteTextFileUtf8(const std::wstring& path, const std::wstring& text) {
    if (path.empty()) return false;

    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed < 0) return false;

    std::vector<char> utf8(static_cast<size_t>(needed));
    if (needed > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                            static_cast<int>(text.size()), utf8.data(), needed,
                            nullptr, nullptr);
    }

    const HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool ok = true;
    DWORD written = 0;
    // BOM, so Excel opens UTF-8 as UTF-8 rather than guessing the code page.
    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    if (!WriteFile(h, bom, 3, &written, nullptr)) ok = false;
    if (ok && needed > 0 &&
        !WriteFile(h, utf8.data(), static_cast<DWORD>(needed), &written,
                   nullptr)) {
        ok = false;
    }
    CloseHandle(h);
    return ok;
}

// -------------------------------------------------------------- scan cache
//
// Serialisation lives in core.cpp where the tests can reach it; this is only
// the Windows plumbing around it: where the file lives, and getting bytes in
// and out without ever presenting a torn file to a reader.

static std::wstring CacheDir() {
    // %LOCALAPPDATA%: per-user, per-machine, excluded from roaming - right
    // for data that describes this machine's drives and can be regenerated.
    wchar_t buf[MAX_PATH + 1] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
                                SHGFP_TYPE_CURRENT, buf))) {
        return {};
    }
    std::wstring dir(buf);
    dir += L"\\Spindle";
    CreateDirectoryW(dir.c_str(), nullptr);   // fine if it already exists
    return dir;
}

std::wstring CachePathForVolume(const std::wstring& volumePath) {
    if (volumePath.empty()) return {};
    const std::wstring dir = CacheDir();
    if (dir.empty()) return {};
    // "C:\" -> "...\Spindle\C.spincache". Drive letters only; a UNC root has
    // no stable single-character identity, so it simply never caches.
    const wchar_t letter = volumePath[0];
    if (!((letter >= L'A' && letter <= L'Z') ||
          (letter >= L'a' && letter <= L'z'))) {
        return {};
    }
    std::wstring p = dir;
    p += L'\\';
    p += letter;
    p += L".spincache";
    return p;
}

static uint32_t VolumeSerial(const std::wstring& volumePath) {
    DWORD serial = 0;
    GetVolumeInformationW(volumePath.c_str(), nullptr, 0, &serial, nullptr,
                          nullptr, nullptr, 0);
    return serial;
}

bool LoadScanCache(const std::wstring& volumePath, ScanResult& out,
                   CacheMeta& meta) {
    const std::wstring path = CachePathForVolume(volumePath);
    if (path.empty()) return false;

    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) != 0 && size.QuadPart > 0 &&
              static_cast<uint64_t>(size.QuadPart) <= kMaxCacheBytes;

    std::vector<uint8_t> bytes;
    if (ok) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        size_t got = 0;
        while (ok && got < bytes.size()) {
            const DWORD chunk = static_cast<DWORD>(
                std::min<size_t>(bytes.size() - got, 1u << 24));
            DWORD read = 0;
            if (!ReadFile(h, bytes.data() + got, chunk, &read, nullptr) ||
                read == 0) {
                ok = false;
            } else {
                got += read;
            }
        }
    }
    CloseHandle(h);
    if (!ok) return false;

    if (!DeserializeScan(bytes.data(), bytes.size(), out, meta)) {
        // Unreadable or hostile: it will only fail again, so remove it.
        DeleteFileW(path.c_str());
        return false;
    }
    // A cache written for a different volume that now has this letter is
    // worse than no cache: it is confidently wrong.
    if (meta.volumeSerial != VolumeSerial(volumePath)) return false;
    return true;
}

bool SaveScanCache(const std::wstring& volumePath, const ScanResult& res) {
    const std::wstring path = CachePathForVolume(volumePath);
    if (path.empty()) return false;

    CacheMeta meta;
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    // FILETIME is 100ns ticks since 1601; 11644473600s to the Unix epoch.
    const uint64_t ticks =
        (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    meta.savedUnixMs  = ticks / 10000 - 11644473600000ULL;
    meta.volumeSerial = VolumeSerial(volumePath);

    std::vector<uint8_t> bytes;
    SerializeScan(res, meta, bytes);
    if (bytes.empty() || bytes.size() > kMaxCacheBytes) return false;

    // Write to a sibling and swap, so a crash mid-write leaves either the
    // old cache or none - never a torn file for the next launch to read.
    const std::wstring tmp = path + L".tmp";
    const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool ok = true;
    size_t put = 0;
    while (ok && put < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(bytes.size() - put, 1u << 24));
        DWORD written = 0;
        if (!WriteFile(h, bytes.data() + put, chunk, &written, nullptr) ||
            written == 0) {
            ok = false;
        } else {
            put += written;
        }
    }
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// -------------------------------------------------------------- duplicates

DupReport FindDuplicates(const Node& root, const std::wstring& rootPath,
                         uint64_t minSize, Progress* progress) {
    DupReport rep;

    std::vector<DupFile> candidates = DuplicateCandidates(root, minSize);
    std::vector<DupFile> hashed;
    hashed.reserve(candidates.size());

    // One reusable buffer. 1 MB keeps the read count low on a spinning disk
    // without the allocation itself being a spike.
    std::vector<uint8_t> buf(1u << 20);

    for (DupFile& f : candidates) {
        if (progress && progress->cancel.load(std::memory_order_relaxed)) {
            rep.cancelled = true;
            break;
        }

        std::wstring full = rootPath;
        if (!full.empty() && full.back() != L'\\') full += L'\\';
        full += f.path;
        const std::wstring extended = ExtendedPath(full);

        // FILE_FLAG_OPEN_NO_RECALL is the belt to the placeholder check's
        // braces: if a file became a cloud placeholder between the scan and
        // now, this refuses to pull it down rather than silently costing
        // the user a multi-gigabyte download.
        const HANDLE h = CreateFileW(
            extended.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_NO_RECALL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            // A placeholder that would need fetching reports this rather
            // than downloading itself, which is exactly what was wanted.
            if (err == ERROR_CLOUD_FILE_ACCESS_DENIED ||
                err == ERROR_NOT_READY) {
                ++rep.skippedCloud;
            } else {
                ++rep.skippedUnread;
            }
            continue;
        }

        Hasher hasher;
        bool ok = true;
        uint64_t read = 0;
        for (;;) {
            if (progress && progress->cancel.load(std::memory_order_relaxed)) {
                ok = false;
                rep.cancelled = true;
                break;
            }
            DWORD got = 0;
            if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got,
                          nullptr)) {
                ok = false;
                break;
            }
            if (got == 0) break;   // end of file
            hasher.Update(buf.data(), got);
            read += got;
        }
        CloseHandle(h);

        if (!ok) {
            if (!rep.cancelled) ++rep.skippedUnread;
            if (rep.cancelled) break;
            continue;
        }

        f.digest = hasher.Finish();
        rep.bytesHashed = SatAdd(rep.bytesHashed, read);
        ++rep.filesHashed;
        hashed.push_back(f);

        if (progress) {
            progress->files.store(rep.filesHashed, std::memory_order_relaxed);
            progress->bytes.store(rep.bytesHashed, std::memory_order_relaxed);
        }
    }

    rep.groups = GroupByDigest(hashed);
    for (const DupGroup& g : rep.groups) {
        rep.totalWasted = SatAdd(rep.totalWasted, g.wasted);
    }
    return rep;
}

// ------------------------------------------------------------ force removal
//
// The destructive path. Everything here is bounded by IsProtectedSystemPath,
// checked first and re-checked per directory entry, because the recursion
// below walks whatever the disk hands it.

std::vector<Locker> FindLockers(const std::wstring& path) {
    std::vector<Locker> out;
    if (path.empty()) return out;

    // Restart Manager is the documented way to ask "who has this open" -
    // no driver, no handle-table walking, no injection.
    DWORD  session = 0;
    WCHAR  key[CCH_RM_SESSION_KEY + 1] = {};
    if (RmStartSession(&session, 0, key) != ERROR_SUCCESS) return out;

    LPCWSTR files[1] = {path.c_str()};
    if (RmRegisterResources(session, 1, files, 0, nullptr, 0, nullptr) ==
        ERROR_SUCCESS) {
        UINT needed = 0, got = 0;
        DWORD reason = 0;
        // Ask for the count, then the list. A process can appear between
        // the two calls, so this is a snapshot, not a guarantee.
        UINT result = RmGetList(session, &needed, &got, nullptr, &reason);
        if ((result == ERROR_MORE_DATA || result == ERROR_SUCCESS) &&
            needed > 0 && needed < 1024) {
            std::vector<RM_PROCESS_INFO> info(needed);
            got = needed;
            if (RmGetList(session, &needed, &got, info.data(), &reason) ==
                ERROR_SUCCESS) {
                for (UINT i = 0; i < got; ++i) {
                    Locker l;
                    l.pid  = info[i].Process.dwProcessId;
                    l.name = info[i].strAppName;
                    l.critical = IsCriticalProcess(l.name, l.pid);
                    out.push_back(std::move(l));
                }
            }
        }
    }
    RmEndSession(session);
    return out;
}

namespace {

// Strip the attributes that make DeleteFileW refuse before it starts.
void ClearBlockingAttributes(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return;
    const DWORD blocking =
        FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
        FILE_ATTRIBUTE_SYSTEM;
    if ((attrs & blocking) != 0) {
        SetFileAttributesW(path.c_str(), attrs & ~blocking);
    }
}

// Make the file deletable when the ACL says otherwise: become the owner,
// then grant Administrators full control. Both steps need the process to be
// elevated; unelevated they fail and the delete simply reports access
// denied, which is the correct outcome.
bool TakeOwnershipAndGrant(const std::wstring& path) {
    // SE_TAKE_OWNERSHIP_NAME is not enabled by default even for an admin.
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(),
                         TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        const wchar_t* privileges[] = {SE_TAKE_OWNERSHIP_NAME,
                                       SE_RESTORE_NAME, SE_BACKUP_NAME};
        for (const wchar_t* name : privileges) {
            TOKEN_PRIVILEGES tp{};
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            if (LookupPrivilegeValueW(nullptr, name,
                                      &tp.Privileges[0].Luid)) {
                AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
            }
        }
        CloseHandle(token);
    }

    // The Administrators group, built from its well-known SID rather than a
    // name lookup, which is localised.
    BYTE sidBuf[SECURITY_MAX_SID_SIZE];
    auto* admins = reinterpret_cast<PSID>(sidBuf);
    DWORD sidSize = sizeof(sidBuf);
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins,
                            &sidSize)) {
        return false;
    }

    std::vector<wchar_t> mutablePath(path.begin(), path.end());
    mutablePath.push_back(L'\0');

    if (SetNamedSecurityInfoW(mutablePath.data(), SE_FILE_OBJECT,
                              OWNER_SECURITY_INFORMATION, admins, nullptr,
                              nullptr, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    EXPLICIT_ACCESS_W ea{};
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode        = GRANT_ACCESS;
    ea.grfInheritance       = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType  = TRUSTEE_IS_GROUP;
    ea.Trustee.ptstrName    = static_cast<LPWSTR>(admins);

    PACL newAcl = nullptr;
    if (SetEntriesInAclW(1, &ea, nullptr, &newAcl) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD rc =
        SetNamedSecurityInfoW(mutablePath.data(), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION, nullptr, nullptr,
                              newAcl, nullptr);
    if (newAcl) LocalFree(newAcl);
    return rc == ERROR_SUCCESS;
}

// Delete one file, escalating only as far as it has to.
bool DeleteOneFile(const std::wstring& path, DWORD& lastError) {
    if (DeleteFileW(path.c_str())) return true;
    lastError = GetLastError();

    if (lastError == ERROR_ACCESS_DENIED) {
        ClearBlockingAttributes(path);
        if (DeleteFileW(path.c_str())) return true;
        TakeOwnershipAndGrant(path);
        if (DeleteFileW(path.c_str())) return true;
        lastError = GetLastError();
    }
    return false;
}

bool RemoveOneDirectory(const std::wstring& path, DWORD& lastError) {
    if (RemoveDirectoryW(path.c_str())) return true;
    lastError = GetLastError();

    if (lastError == ERROR_ACCESS_DENIED) {
        ClearBlockingAttributes(path);
        if (RemoveDirectoryW(path.c_str())) return true;
        TakeOwnershipAndGrant(path);
        if (RemoveDirectoryW(path.c_str())) return true;
        lastError = GetLastError();
    }
    return false;
}

// Depth-first, iterative: nesting depth comes off the disk here exactly as
// it does in the scanner, so this must not recurse. Directories are pushed
// twice - once to enumerate, once to remove after their children are gone.
bool DeleteTreeIterative(const std::wstring& root, uint64_t& deleted,
                         DWORD& lastError) {
    struct Frame {
        std::wstring path;
        bool         enumerated;
    };
    std::vector<Frame> stack;
    stack.push_back({root, false});
    bool allOk = true;

    // The same bound the scanner uses; a structure deeper than this is not
    // something to keep walking.
    constexpr size_t kMaxPending = 1u << 20;

    while (!stack.empty()) {
        Frame frame = stack.back();
        stack.pop_back();

        const DWORD attrs = GetFileAttributesW(frame.path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) continue;   // already gone

        const bool isDir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const bool isLink = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

        // A junction is removed as a link, never followed. Recursing
        // through one would delete the target's contents - the same rule
        // as the scanner, with far worse consequences for breaking it.
        if (!isDir || isLink) {
            DWORD err = 0;
            const bool ok = isLink && isDir
                                ? RemoveOneDirectory(frame.path, err)
                                : DeleteOneFile(frame.path, err);
            if (ok) {
                ++deleted;
            } else {
                allOk = false;
                if (err != 0) lastError = err;
            }
            continue;
        }

        if (!frame.enumerated) {
            // Re-check per directory: the walk is following names off the
            // disk, and a junction or a race could have led somewhere the
            // caller never named.
            if (IsProtectedSystemPath(frame.path)) {
                allOk = false;
                continue;
            }
            stack.push_back({frame.path, true});   // remove after children

            WIN32_FIND_DATAW fd{};
            const std::wstring pattern = frame.path + L"\\*";
            const HANDLE h = FindFirstFileExW(pattern.c_str(),
                                              FindExInfoBasic, &fd,
                                              FindExSearchNameMatch, nullptr,
                                              FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) {
                lastError = GetLastError();
                allOk = false;
                continue;
            }
            do {
                const wchar_t* n = fd.cFileName;
                if (n[0] == L'.' &&
                    (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) {
                    continue;
                }
                if (stack.size() >= kMaxPending) { allOk = false; break; }
                stack.push_back({frame.path + L'\\' + n, false});
            } while (FindNextFileW(h, &fd));
            FindClose(h);
            continue;
        }

        DWORD err = 0;
        if (RemoveOneDirectory(frame.path, err)) {
            ++deleted;
        } else {
            allOk = false;
            if (err != 0) lastError = err;
        }
    }
    return allOk;
}

}  // namespace

ForceRemoveResult ForceRemove(const std::wstring& path,
                              bool terminateLockers) {
    ForceRemoveResult res;

    // The refusal lives here, not only in the dialog, so that no future
    // caller can reach the destructive path without passing it.
    if (path.empty() || IsProtectedSystemPath(path)) {
        res.blocked = true;
        return res;
    }

    if (terminateLockers) {
        for (const Locker& l : FindLockers(path)) {
            if (l.critical) {
                res.remaining.push_back(l);
                continue;   // never, whatever the user clicked
            }
            const HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, l.pid);
            if (!h) {
                res.remaining.push_back(l);
                continue;
            }
            if (!TerminateProcess(h, 1)) {
                res.remaining.push_back(l);
            } else {
                // Give the handles a moment to close before retrying the
                // delete; a terminated process does not release instantly.
                WaitForSingleObject(h, 3000);
            }
            CloseHandle(h);
        }
    }

    DWORD lastError = 0;
    res.ok = DeleteTreeIterative(path, res.filesDeleted, lastError);
    res.lastError = lastError;

    // Whatever still has it open, so the interface can say so by name
    // rather than reporting a bare error code.
    if (!res.ok) {
        for (const Locker& l : FindLockers(path)) {
            res.remaining.push_back(l);
        }
    }
    return res;
}

std::wstring CacheDirPath() { return CacheDir(); }

void ClearScanCaches() {
    const std::wstring dir = CacheDir();
    if (dir.empty()) return;

    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = dir + L"\\*.spincache";
    const HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---------------------------------------------------------------- settings

static std::wstring SettingsPath() {
    const std::wstring dir = CacheDir();
    if (dir.empty()) return {};
    return dir + L"\\settings.txt";
}

Settings LoadSettings() {
    Settings s;
    const std::wstring path = SettingsPath();
    if (path.empty()) return s;

    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return s;

    uint8_t buf[kMaxSettingsBytes];
    DWORD got = 0;
    const bool ok = ReadFile(h, buf, sizeof(buf), &got, nullptr) != 0;
    CloseHandle(h);
    if (!ok) return s;
    return ParseSettings(buf, got);
}

bool SaveSettings(const Settings& s) {
    const std::wstring path = SettingsPath();
    if (path.empty()) return false;

    std::vector<uint8_t> bytes;
    SerializeSettings(s, bytes);

    // Same temp-and-swap as the caches: never a torn file.
    const std::wstring tmp = path + L".tmp";
    const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const bool ok =
        WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()),
                  &written, nullptr) != 0 &&
        written == bytes.size();
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace spindle
