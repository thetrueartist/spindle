// Spindle - the parallel directory scanner, volume enumeration and file
// writing. Everything in this file needs windows.h; the queue it runs on and
// the tree it produces do not, which is what makes those testable.

#include "spindle.h"

#include "workqueue.h"  // pulls in sync.h and, on Windows, windows.h

#include <process.h>

#include <exception>
#include <utility>

namespace spindle {

// ------------------------------------------------------------- file writing

// UTF-8 with a BOM: Excel is the main consumer of these exports and without
// the BOM it guesses the codepage, mangling every non-ASCII filename.
bool WriteTextFileUtf8(const std::wstring& path, const std::wstring& text) {
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    bool ok = true;
    const auto write = [&](const void* p, DWORD n) {
        DWORD written = 0;
        if (!WriteFile(f, p, n, &written, nullptr) || written != n) {
            ok = false;
        }
    };

    static const uint8_t kBom[3] = {0xEF, 0xBB, 0xBF};
    write(kBom, 3);

    // Converted in bounded chunks: WideCharToMultiByte takes an int, and a
    // full-volume export can be large enough to care. A chunk may not end
    // between a surrogate pair or the pair converts as two broken
    // replacement characters.
    constexpr size_t kChunk = 1u << 20;
    std::string bytes;
    size_t off = 0;
    while (ok && off < text.size()) {
        size_t n = (text.size() - off < kChunk) ? text.size() - off : kChunk;
        const wchar_t last = text[off + n - 1];
        if (n < text.size() - off && last >= 0xD800 && last <= 0xDBFF) --n;
        if (n == 0) break;

        const int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str() + off,
                                             static_cast<int>(n), nullptr, 0,
                                             nullptr, nullptr);
        if (need <= 0) {
            ok = false;
            break;
        }
        bytes.resize(static_cast<size_t>(need));
        WideCharToMultiByte(CP_UTF8, 0, text.c_str() + off,
                            static_cast<int>(n), bytes.data(), need, nullptr,
                            nullptr);
        write(bytes.data(), static_cast<DWORD>(need));
        off += n;
    }

    CloseHandle(f);
    return ok;
}

// ------------------------------------------------------------------ volumes

std::vector<Volume> EnumerateVolumes() {
    std::vector<Volume> out;
    const DWORD mask = GetLogicalDrives();

    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) continue;

        const wchar_t root[4] = {static_cast<wchar_t>(L'A' + i), L':', L'\\',
                                 L'\0'};
        const UINT type = GetDriveTypeW(root);
        // Optical drives and RAM disks are excluded; a mapped network drive
        // is not, because the directory walker handles it fine - it just
        // never gets the MFT fast path.
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE &&
            type != DRIVE_REMOTE) {
            continue;
        }

        Volume v;
        v.path = root;

        wchar_t label[MAX_PATH + 1] = {};
        if (GetVolumeInformationW(root, label, MAX_PATH, nullptr, nullptr,
                                  nullptr, nullptr, 0)) {
            v.label = label;
        }

        ULARGE_INTEGER freeBytes{};
        ULARGE_INTEGER totalBytes{};
        if (GetDiskFreeSpaceExW(root, nullptr, &totalBytes, &freeBytes)) {
            v.capacity = totalBytes.QuadPart;
            v.free = freeBytes.QuadPart;
        }
        out.push_back(std::move(v));
    }
    return out;
}

// ------------------------------------------------------------------- walker

namespace {

struct WorkItem {
    Node* node = nullptr;
    std::wstring path;   // display form, e.g. "C:\Users" - no \\?\ prefix
};

struct WalkContext {
    WorkQueue<WorkItem>* queue = nullptr;
    Progress* progress = nullptr;
    std::atomic<uint64_t>* denied = nullptr;
    std::atomic<bool>* faulted = nullptr;
};

// FindClose on every exit path. The enumeration loop below allocates, and an
// exception part-way through would otherwise leak the handle - sixteen
// workers leaking one per failure exhausts them quickly.
class FindGuard {
public:
    explicit FindGuard(HANDLE h) : h_(h) {}
    ~FindGuard() {
        if (h_ != INVALID_HANDLE_VALUE) FindClose(h_);
    }
    FindGuard(const FindGuard&) = delete;
    FindGuard& operator=(const FindGuard&) = delete;

private:
    HANDLE h_;
};

inline bool IsDotEntry(const wchar_t* n) {
    return n[0] == L'.' &&
           (n[1] == L'\0' || (n[1] == L'.' && n[2] == L'\0'));
}

void EnumerateDirectory(const WorkItem& item, const WalkContext& ctx) {
    // Extended-length prefix on every query: nesting on a real volume runs
    // far past MAX_PATH, and the prefix costs nothing on short paths.
    std::wstring pattern;
    pattern.reserve(item.path.size() + 8);
    pattern += L"\\\\?\\";
    pattern += item.path;
    pattern += L"\\*";

    WIN32_FIND_DATAW fd;
    // FindExInfoBasic skips the short-name lookup, and the large fetch flag
    // batches the directory read; together they are most of the reason this
    // outruns a naive FindFirstFile walk.
    const HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                      FindExSearchNameMatch, nullptr,
                                      FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        ctx.denied->fetch_add(1, std::memory_order_relaxed);
        return;
    }
    FindGuard guard(h);

    struct Entry {
        std::wstring name;
        uint64_t size = 0;
        bool dir = false;
        bool traverse = false;
    };
    std::vector<Entry> entries;

    do {
        if (IsDotEntry(fd.cFileName)) continue;

        const bool isDir =
            (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        // Junctions and symlinks are recorded but never walked into: they
        // would loop, or count a target twice. Their target's size is not
        // this directory's size either, so they carry none.
        const bool reparse =
            (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

        Entry e;
        e.name = fd.cFileName;
        e.dir = isDir;
        e.traverse = isDir && !reparse;
        if (!isDir && !reparse) {
            e.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) |
                     fd.nFileSizeLow;
        }
        entries.push_back(std::move(e));
    } while (FindNextFileW(h, &fd) != 0);

    // The children vector is reserved to its exact final count before any
    // pointer into it leaves this function. That is the whole locking
    // strategy: a queued child pointer stays valid because the vector can
    // never reallocate afterwards.
    Node* node = item.node;
    node->children.reserve(entries.size());

    uint64_t files = 0;
    uint64_t dirs = 0;
    uint64_t bytes = 0;
    for (Entry& e : entries) {
        Node child;
        child.dir = e.dir;
        child.size = e.size;
        child.files = e.dir ? 0 : 1;
        child.cat = e.dir ? Cat::Directory : CategoryForFile(e.name);
        child.name = std::move(e.name);
        node->children.push_back(std::move(child));
        if (e.dir) {
            ++dirs;
        } else {
            ++files;
            bytes = SatAdd(bytes, e.size);
        }
    }

    ctx.progress->files.fetch_add(files, std::memory_order_relaxed);
    ctx.progress->dirs.fetch_add(dirs, std::memory_order_relaxed);
    ctx.progress->bytes.fetch_add(bytes, std::memory_order_relaxed);

    for (size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].traverse) continue;
        WorkItem w;
        w.node = &node->children[i];
        w.path.reserve(item.path.size() + 1 + node->children[i].name.size());
        w.path += item.path;
        w.path += L'\\';
        w.path += node->children[i].name;
        ctx.queue->Push(std::move(w));
    }
}

// Thread entry. Wrapped end to end: an exception escaping a thread function
// calls std::terminate, and a scan this size performs millions of
// allocations - one bad_alloc must degrade to an incomplete tree, not kill
// the process.
unsigned __stdcall WalkWorker(void* param) {
    auto* ctx = static_cast<WalkContext*>(param);
    try {
        WorkItem item;
        while (ctx->queue->Pop(item)) {
            if (ctx->progress->cancel.load(std::memory_order_relaxed)) {
                ctx->queue->Cancel();
                ctx->queue->Done();
                continue;   // the next Pop observes the cancel and exits
            }
            EnumerateDirectory(item, *ctx);
            ctx->queue->Done();
        }
    } catch (...) {
        ctx->faulted->store(true, std::memory_order_relaxed);
        ctx->queue->Cancel();
    }
    return 0;
}

void WalkTree(const std::wstring& rootPath, uint32_t threads, Node& root,
              ScanStats& stats, Progress* progress) {
    WorkQueue<WorkItem> queue;
    std::atomic<uint64_t> denied{0};
    std::atomic<bool> faulted{false};

    WalkContext ctx;
    ctx.queue = &queue;
    ctx.progress = progress;
    ctx.denied = &denied;
    ctx.faulted = &faulted;

    WorkItem seed;
    seed.node = &root;
    seed.path = root.name;
    queue.Push(std::move(seed));

    // Sixteen is where the returns flatten on real volumes: past it the
    // walk is bound by the filesystem, not the CPU.
    uint32_t want = threads;
    if (want < 1) want = 1;
    if (want > 16) want = 16;

    // The pool runs with however many threads it can get. Every worker holds
    // a pointer to `ctx`, which outlives them because this function joins
    // them all before returning; if none start at all, the scan runs inline
    // on this thread instead of failing.
    HANDLE pool[16] = {};
    uint32_t started = 0;
    for (uint32_t i = 0; i < want; ++i) {
        const uintptr_t h =
            _beginthreadex(nullptr, 0, WalkWorker, &ctx, 0, nullptr);
        if (h != 0) pool[started++] = reinterpret_cast<HANDLE>(h);
    }

    if (started == 0) {
        WalkWorker(&ctx);
    } else {
        WaitForMultipleObjects(started, pool, TRUE, INFINITE);
        for (uint32_t i = 0; i < started; ++i) CloseHandle(pool[i]);
    }

    stats.deniedCount = denied.load(std::memory_order_relaxed);
    stats.faulted = faulted.load(std::memory_order_relaxed);
}

}  // namespace

// --------------------------------------------------------------------- scan

ScanResult Scan(const std::wstring& root, uint32_t threads,
                Progress* progress) {
    LARGE_INTEGER freq{};
    LARGE_INTEGER t0{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    ScanResult result;
    result.root.dir = true;
    result.root.cat = Cat::Directory;
    result.root.name = root;
    while (!result.root.name.empty() && result.root.name.back() == L'\\') {
        result.root.name.pop_back();
    }

    // The MFT fast path applies to a local NTFS volume in an elevated
    // process; ScanWithMft checks its own preconditions and reports false on
    // any of them, or on anything unexpected in the volume itself. The
    // answer is the same either way, so the fallback is silent.
    if (ScanWithMft(root, progress, result.root, result.stats)) {
        result.stats.usedMft = true;
    } else if (!progress->cancel.load(std::memory_order_relaxed)) {
        result.root.children.clear();
        WalkTree(root, threads, result.root, result.stats, progress);
        result.stats.fileCount =
            progress->files.load(std::memory_order_relaxed);
        result.stats.dirCount = progress->dirs.load(std::memory_order_relaxed);
        result.stats.bytes = progress->bytes.load(std::memory_order_relaxed);
    }

    RollUp(result.root);
    SortTree(result.root);

    LARGE_INTEGER t1{};
    QueryPerformanceCounter(&t1);
    if (freq.QuadPart > 0) {
        result.stats.seconds =
            static_cast<double>(t1.QuadPart - t0.QuadPart) /
            static_cast<double>(freq.QuadPart);
    }

    progress->done.store(true, std::memory_order_relaxed);
    return result;
}

}  // namespace spindle
