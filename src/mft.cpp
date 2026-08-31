// Spindle - the MFT fast path.
//
// Instead of a syscall round trip per directory, read the volume's own index:
// a handful of large sequential reads of the Master File Table, parsed by the
// pure-byte code in ntfs.cpp. This is the technique WizTree is built around,
// and it is the difference between several seconds and well under one on a
// million-file drive.
//
// Everything here is deliberately paranoid about failure. Any precondition
// not met, any structure that does not validate, any read that comes up
// short: return false and let scan.cpp fall back to the directory walk. The
// user gets the same answer either way, just slower.

#include "spindle.h"

#include "ntfs.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <utility>

namespace spindle {

namespace {

// Raw volume access is an administrator privilege; asking first avoids a
// guaranteed CreateFile failure and is the sole reason OpenProcessToken is
// imported.
bool ProcessIsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD got = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &got);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

class HandleGuard {
public:
    explicit HandleGuard(HANDLE h) : h_(h) {}
    ~HandleGuard() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;

private:
    HANDLE h_;
};

// Positioned read. Volume handles require sector-aligned offsets and whole
// sectors; every caller reads in cluster multiples, which satisfies that.
bool ReadAt(HANDLE h, uint64_t offset, void* dst, DWORD len) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD got = 0;
    if (!ReadFile(h, dst, len, &got, &ov)) return false;
    return got == len;
}

// One MFT record, reduced to what the tree needs. The transient array of
// these is the fast path's peak memory: ~64 bytes plus the name per record.
struct RawEntry {
    std::wstring name;
    uint64_t parent = 0;
    uint64_t size = 0;
    bool used = false;
    bool isDir = false;
};

// Hard cap on how many records the fast path will hold in memory at once
// (32M records is far beyond any volume this tool is pointed at, and the
// walker still exists for anything bigger).
constexpr uint64_t kMaxRecords = 1ull << 25;

}  // namespace

bool ScanWithMft(const std::wstring& root, Progress* progress, Node& outRoot,
                 ScanStats& outStats) {
    // Only a plain local drive root qualifies. Anything else - UNC, a
    // subdirectory, an odd spelling - goes to the walker.
    if (root.size() != 3 || root[1] != L':' || root[2] != L'\\') return false;
    if (!ProcessIsElevated()) return false;

    wchar_t fsName[MAX_PATH + 1] = {};
    if (!GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr,
                               nullptr, fsName, MAX_PATH)) {
        return false;
    }
    if (lstrcmpW(fsName, L"NTFS") != 0) return false;

    const wchar_t device[7] = {L'\\', L'\\', L'.',  L'\\',
                               root[0], L':', L'\0'};
    const HANDLE vol = CreateFileW(
        device, FILE_READ_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, 0, nullptr);
    if (vol == INVALID_HANDLE_VALUE) return false;
    HandleGuard volGuard(vol);

    // 4096 bytes covers the boot sector at every sector size NTFS ships on,
    // and is a whole number of sectors at all of them.
    std::vector<uint8_t> boot(4096);
    if (!ReadAt(vol, 0, boot.data(), 4096)) return false;

    ntfs::BootSector bs;
    if (!ntfs::ParseBootSector(boot.data(), boot.size(), bs)) return false;

    // The $MFT's own record is the first thing in the table and describes,
    // via its $DATA run list, where the rest of the table lives.
    const uint64_t mftByteOffset = bs.mftLcn * bs.clusterBytes;
    std::vector<uint8_t> first(4096);
    if (!ReadAt(vol, mftByteOffset, first.data(), 4096)) return false;

    if (!ntfs::ApplyFixups(first.data(), bs.recordBytes, bs.bytesPerSector)) {
        return false;
    }
    std::vector<ntfs::Run> runs;
    uint64_t mftBytes = 0;
    if (!ntfs::ExtractDataRuns(first.data(), bs.recordBytes, bs.totalClusters,
                               runs, mftBytes)) {
        return false;
    }

    // The run list is already bounded by the volume's cluster count; the
    // byte size must agree with it, and the record count has a hard cap.
    uint64_t runClusters = 0;
    for (const ntfs::Run& r : runs) {
        if (r.sparse) return false;   // the MFT itself is never sparse
        runClusters += r.clusters;
    }
    if (mftBytes == 0 || mftBytes > runClusters * bs.clusterBytes) {
        return false;
    }
    const uint64_t recordCount = mftBytes / bs.recordBytes;
    if (recordCount < 16 || recordCount > kMaxRecords) return false;

    std::vector<RawEntry> entries(static_cast<size_t>(recordCount));

    // Read the table run by run in large chunks. A record can straddle a run
    // boundary when clusters are smaller than records, so undersized tails
    // are carried over rather than assumed away.
    constexpr uint64_t kChunkBytes = 4ull << 20;   // multiple of any cluster
    std::vector<uint8_t> chunk;
    std::vector<uint8_t> carry;
    carry.reserve(bs.recordBytes);

    uint64_t frs = 0;          // next record index to fill
    uint64_t consumed = 0;     // bytes of the MFT handled so far

    const auto parseRecord = [&](uint8_t* rec) {
        const uint64_t index = frs++;
        // Unused slots and the tail of the table are zeroed; no magic, no
        // record, not an error.
        if (rec[0] != 'F' || rec[1] != 'I' || rec[2] != 'L' || rec[3] != 'E') {
            return;
        }
        if (!ntfs::ApplyFixups(rec, bs.recordBytes, bs.bytesPerSector)) {
            return;   // torn record: skip it, keep the scan
        }
        ntfs::FileRecord fr;
        if (!ntfs::ParseFileRecord(rec, bs.recordBytes, fr)) return;
        if (!fr.inUse || fr.baseFrs != 0 || !fr.hasName) return;
        if (fr.parentFrs >= recordCount) return;

        RawEntry& e = entries[static_cast<size_t>(index)];
        e.used = true;
        e.isDir = fr.isDir;
        e.size = fr.isDir ? 0 : fr.size;
        e.parent = fr.parentFrs;
        e.name = std::move(fr.name);

        if (fr.isDir) {
            progress->dirs.fetch_add(1, std::memory_order_relaxed);
        } else {
            progress->files.fetch_add(1, std::memory_order_relaxed);
            progress->bytes.fetch_add(e.size, std::memory_order_relaxed);
        }
    };

    for (const ntfs::Run& r : runs) {
        uint64_t runOff = 0;
        const uint64_t runBytes = r.clusters * bs.clusterBytes;
        while (runOff < runBytes && consumed < mftBytes &&
               frs < recordCount) {
            if (progress->cancel.load(std::memory_order_relaxed)) {
                return false;
            }

            uint64_t want = runBytes - runOff;
            if (want > kChunkBytes) want = kChunkBytes;
            if (want > mftBytes - consumed) {
                // Round the final read up to whole clusters; only mftBytes
                // of it will be parsed.
                const uint64_t rem = mftBytes - consumed;
                want = ((rem + bs.clusterBytes - 1) / bs.clusterBytes) *
                       bs.clusterBytes;
                if (want > runBytes - runOff) want = runBytes - runOff;
            }

            chunk.resize(static_cast<size_t>(want));
            const uint64_t diskOff =
                r.lcn * bs.clusterBytes + runOff;
            if (!ReadAt(vol, diskOff, chunk.data(),
                        static_cast<DWORD>(want))) {
                return false;
            }

            uint64_t usable = want;
            if (usable > mftBytes - consumed) usable = mftBytes - consumed;

            size_t pos = 0;
            if (!carry.empty()) {
                const size_t need = bs.recordBytes - carry.size();
                if (usable < need) {
                    carry.insert(carry.end(), chunk.begin(),
                                 chunk.begin() +
                                     static_cast<ptrdiff_t>(usable));
                    consumed += usable;
                    runOff += want;
                    continue;
                }
                carry.insert(carry.end(), chunk.begin(),
                             chunk.begin() + static_cast<ptrdiff_t>(need));
                parseRecord(carry.data());
                carry.clear();
                pos = need;
            }
            while (pos + bs.recordBytes <= usable && frs < recordCount) {
                parseRecord(chunk.data() + pos);
                pos += bs.recordBytes;
            }
            if (pos < usable) {
                carry.assign(chunk.begin() + static_cast<ptrdiff_t>(pos),
                             chunk.begin() + static_cast<ptrdiff_t>(usable));
            }

            consumed += usable;
            runOff += want;
        }
    }
    if (frs == 0) return false;

    // ---- tree assembly, iterative throughout.
    //
    // Each record names its parent, so the children lists are built as one
    // flat CSR-style index: count per parent, prefix-sum, scatter. Every
    // record lands in exactly one parent's list, which is also what makes
    // the build immune to crafted cycles - a loop not reachable from the
    // root is simply never visited.
    const size_t n = static_cast<size_t>(recordCount);
    std::vector<uint32_t> childCount(n + 1, 0);

    const auto keep = [&](uint64_t i) {
        const RawEntry& e = entries[i];
        if (!e.used || e.name.empty()) return false;
        // The metadata files ($MFT, $LogFile, $Bitmap...) live below the
        // root but are invisible to the Win32 layer; the walker cannot see
        // them, so the fast path reporting them would make the two paths
        // disagree about the same volume.
        if (i < 16 && i != ntfs::kRootFrs) return false;
        if (e.parent == ntfs::kRootFrs && !e.name.empty() &&
            e.name[0] == L'$') {
            return false;
        }
        if (!entries[static_cast<size_t>(e.parent)].used ||
            !entries[static_cast<size_t>(e.parent)].isDir) {
            return false;
        }
        return true;
    };

    for (uint64_t i = 0; i < recordCount; ++i) {
        if (i == ntfs::kRootFrs) continue;   // the root is nobody's child
        if (keep(i)) ++childCount[static_cast<size_t>(entries[i].parent)];
    }

    std::vector<uint32_t> start(n + 1, 0);
    for (size_t i = 0; i < n; ++i) start[i + 1] = start[i] + childCount[i];
    std::vector<uint32_t> childIndex(start[n]);
    {
        std::vector<uint32_t> fill(start.begin(), start.end() - 1);
        for (uint64_t i = 0; i < recordCount; ++i) {
            if (i == ntfs::kRootFrs || !keep(i)) continue;
            childIndex[fill[static_cast<size_t>(entries[i].parent)]++] =
                static_cast<uint32_t>(i);
        }
    }

    uint64_t attachedFiles = 0;
    uint64_t attachedDirs = 0;
    uint64_t attachedBytes = 0;

    struct BuildFrame {
        Node* node;
        uint32_t frsIndex;
        uint32_t next;
    };
    std::vector<BuildFrame> stack;
    outRoot.children.clear();
    stack.push_back(BuildFrame{&outRoot,
                               static_cast<uint32_t>(ntfs::kRootFrs), 0});
    outRoot.children.reserve(childCount[ntfs::kRootFrs]);

    while (!stack.empty()) {
        BuildFrame& f = stack.back();
        const uint32_t begin = start[f.frsIndex];
        const uint32_t count = childCount[f.frsIndex];
        if (f.next < count) {
            const uint32_t ci = childIndex[begin + f.next];
            ++f.next;
            RawEntry& e = entries[ci];

            Node child;
            child.dir = e.isDir;
            child.size = e.isDir ? 0 : e.size;
            child.files = e.isDir ? 0 : 1;
            child.cat = e.isDir ? Cat::Directory : CategoryForFile(e.name);
            child.name = std::move(e.name);
            f.node->children.push_back(std::move(child));

            if (e.isDir) {
                ++attachedDirs;
            } else {
                ++attachedFiles;
                attachedBytes = SatAdd(attachedBytes, e.size);
            }

            if (e.isDir && childCount[ci] > 0) {
                Node* placed = &f.node->children.back();
                placed->children.reserve(childCount[ci]);
                stack.push_back(BuildFrame{placed, ci, 0});
            }
            continue;
        }
        stack.pop_back();
    }

    // Nothing attached means the root record itself did not survive
    // validation. An empty tree is not a successful scan of a real volume;
    // hand it to the walker rather than presenting a blank map.
    if (attachedFiles + attachedDirs == 0) return false;

    outStats.fileCount = attachedFiles;
    outStats.dirCount = attachedDirs;
    outStats.bytes = attachedBytes;

    // Progress counted parsed records, which can exceed what was attached;
    // snap the public counters to the truth now the tree exists.
    progress->files.store(attachedFiles, std::memory_order_relaxed);
    progress->dirs.store(attachedDirs, std::memory_order_relaxed);
    progress->bytes.store(attachedBytes, std::memory_order_relaxed);
    return true;
}

}  // namespace spindle
