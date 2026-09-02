// Spindle - NTFS Master File Table scanner.
//
// The fast path. Instead of a syscall round trip per directory, this reads the
// volume's own index in a handful of large sequential reads and reconstructs
// the tree from it. On a volume with a million-plus files that is the
// difference between several seconds and well under one.
//
// It needs three things, and falls back to the directory walk without
// complaint if any is missing: an NTFS volume, a local disk, and elevation
// (raw volume access is an administrator privilege).
//
// This file does I/O and assembly only. Every byte that comes off the disk is
// interpreted by src/ntfs.cpp, which is fuzz-tested separately, because the
// parsing is the part that is dangerous and the part worth isolating.

#include "spindle.h"
#include "ntfs.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace spindle {
namespace {

// Read the MFT in chunks rather than whole: a large volume's MFT can run to
// hundreds of megabytes, and a single allocation that size is both a needless
// spike and an easy way for a hostile volume to induce one.
constexpr uint32_t kChunkBytes = 8u << 20;   // 8 MB

// Refuse absurd MFT sizes outright. A real MFT is roughly 1 KB per file; this
// bound corresponds to a volume with hundreds of millions of files.
constexpr uint64_t kMaxMftBytes = 8ull << 30;

// Cap on records, and so on the tree the caller ends up holding.
constexpr uint64_t kMaxRecords = 64u << 20;

class Handle {
public:
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { if (h_ != INVALID_HANDLE_VALUE && h_) CloseHandle(h_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return h_; }
    bool valid() const { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }

private:
    HANDLE h_;
};

// Synchronous read on the overlapped volume handle, for the small one-off
// reads (boot sector, run list). Its own event, so it can never mistake a
// pipelined read's completion for its own. Volume reads are returned in
// whole sectors, so a short read is a genuine failure, not a retry.
bool ReadAt(HANDLE h, uint64_t offset, void* buf, uint32_t bytes) {
    const HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) return false;

    OVERLAPPED ov{};
    ov.Offset     = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    ov.hEvent     = ev;

    DWORD got = 0;
    bool ok = true;
    if (!ReadFile(h, buf, bytes, nullptr, &ov)) {
        ok = (GetLastError() == ERROR_IO_PENDING);
    }
    if (ok) ok = GetOverlappedResult(h, &ov, &got, TRUE) != 0;
    CloseHandle(ev);
    return ok && got == bytes;
}

// One in-flight overlapped read with its own buffer and event. Two of these
// alternate in the MFT loop, so the disk is filling the next chunk while the
// CPU parses this one; the read and the parse each cost real time, and there
// is no reason to pay for them in sequence.
class AsyncRead {
public:
    AsyncRead(HANDLE vol, uint32_t bufBytes)
        : vol_(vol), event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        try {
            buf_.resize(bufBytes);
        } catch (...) {
            buf_.clear();   // Ready() reports it; the caller falls back
        }
    }
    ~AsyncRead() {
        // An in-flight read writes into buf_; both must outlive it.
        Drain();
        if (event_) CloseHandle(event_);
    }
    AsyncRead(const AsyncRead&) = delete;
    AsyncRead& operator=(const AsyncRead&) = delete;

    bool Ready() const { return event_ != nullptr && !buf_.empty(); }
    uint8_t* Data() { return buf_.data(); }   // fixups patch in place

    bool Issue(uint64_t offset, uint32_t bytes) {
        if (!Ready() || inFlight_ || bytes > buf_.size()) return false;
        ResetEvent(event_);
        ov_ = OVERLAPPED{};
        ov_.Offset     = static_cast<DWORD>(offset);
        ov_.OffsetHigh = static_cast<DWORD>(offset >> 32);
        ov_.hEvent     = event_;
        want_ = bytes;
        if (!ReadFile(vol_, buf_.data(), bytes, nullptr, &ov_) &&
            GetLastError() != ERROR_IO_PENDING) {
            return false;
        }
        inFlight_ = true;
        return true;
    }

    // Wait out the issued read; true only if every requested byte arrived.
    bool Wait() {
        if (!inFlight_) return false;
        inFlight_ = false;
        DWORD got = 0;
        if (!GetOverlappedResult(vol_, &ov_, &got, TRUE)) return false;
        return got == want_;
    }

private:
    void Drain() {
        if (!inFlight_) return;
        CancelIoEx(vol_, &ov_);
        DWORD got = 0;
        GetOverlappedResult(vol_, &ov_, &got, TRUE);
        inFlight_ = false;
    }

    HANDLE               vol_;
    HANDLE               event_;
    std::vector<uint8_t> buf_;
    OVERLAPPED           ov_{};
    uint32_t             want_ = 0;
    bool                 inFlight_ = false;
};

bool IsElevated() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
    Handle t(raw);

    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    if (!GetTokenInformation(t.get(), TokenElevation, &elevation,
                             sizeof(elevation), &size)) {
        return false;
    }
    return elevation.TokenIsElevated != 0;
}

// One entry per MFT record. Indexed by record number, so the parent reference
// in a record is a direct index rather than a lookup.
struct Entry {
    uint32_t parent   = 0;
    uint32_t nameOff  = 0;   // into the name pool
    uint16_t nameLen  = 0;
    bool     isDir    = false;
    bool     used     = false;
    bool     hardlink = false;   // more than one name points at this record
    uint64_t size     = 0;
};

// Names live in one contiguous pool rather than a string per entry. A million
// separate small allocations costs both time and a per-allocation header;
// this is one buffer and a 6-byte reference per record.
struct NamePool {
    std::vector<wchar_t> data;

    // Returns kNoName when the pool has grown past what a 32-bit offset
    // can address. Truncating instead would hand back a reference to the
    // wrong part of the buffer, and a name is what paths are built from.
    static constexpr uint32_t kNoName = 0xFFFFFFFFu;

    uint32_t Add(const std::wstring& s, uint16_t& lenOut) {
        if (data.size() >= kNoName) {
            lenOut = 0;
            return kNoName;
        }
        const uint32_t off = static_cast<uint32_t>(data.size());
        const size_t n = std::min<size_t>(s.size(), 0xFFFF);
        data.insert(data.end(), s.begin(), s.begin() + static_cast<long>(n));
        lenOut = static_cast<uint16_t>(n);
        return off;
    }

    std::wstring Get(uint32_t off, uint16_t len) const {
        if (off == kNoName) return std::wstring();
        if (len == 0 || off > data.size() || data.size() - off < len) {
            return std::wstring();
        }
        return std::wstring(data.begin() + off, data.begin() + off + len);
    }
};

// Walk the $MFT record itself to find where the rest of the table lives.
bool ReadMftRunList(HANDLE vol, const ntfs::BootInfo& bi,
                    std::vector<ntfs::DataRun>& runs, uint64_t& mftBytes) {
    std::vector<uint8_t> rec(bi.bytesPerRecord);
    const uint64_t mftOffset = bi.mftStartCluster * bi.bytesPerCluster;

    if (!ReadAt(vol, mftOffset, rec.data(), bi.bytesPerRecord)) return false;
    if (!ntfs::ApplyFixups(rec.data(), rec.size(), bi.bytesPerSector)) {
        return false;
    }
    if (std::memcmp(rec.data(), "FILE", 4) != 0) return false;

    // Walk to the unnamed non-resident $DATA and take its run list. Done here
    // rather than in ntfs.cpp because only this one record needs it, and the
    // bounds logic is the same shape as ParseRecord's.
    const size_t len = rec.size();
    auto u16 = [&](size_t o) -> uint16_t {
        return (o + 2 <= len) ? static_cast<uint16_t>(rec[o] | (rec[o + 1] << 8))
                              : uint16_t{0};
    };
    auto u32 = [&](size_t o) -> uint32_t {
        if (o + 4 > len) return 0;
        return static_cast<uint32_t>(rec[o]) |
               (static_cast<uint32_t>(rec[o + 1]) << 8) |
               (static_cast<uint32_t>(rec[o + 2]) << 16) |
               (static_cast<uint32_t>(rec[o + 3]) << 24);
    };
    auto u64 = [&](size_t o) -> uint64_t {
        if (o + 8 > len) return 0;
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | rec[o + static_cast<size_t>(i)];
        return v;
    };

    const uint32_t used = u32(0x18);
    const size_t limit = (used >= 48 && used <= len) ? used : len;
    size_t off = u16(0x14);
    if (off < 48 || off >= limit) return false;

    for (size_t guard = 0; guard < ntfs::kMaxAttrsPerRec; ++guard) {
        const uint32_t type = u32(off);
        if (type == ntfs::kAttrEnd) break;
        const uint32_t attrLen = u32(off + 4);
        if (attrLen < 16 || (attrLen % 8) != 0 || attrLen > limit - off) break;

        if (type == ntfs::kAttrData && rec[off + 8] == 1 && rec[off + 9] == 0) {
            const uint16_t runOff = u16(off + 0x20);
            if (runOff >= attrLen) return false;
            mftBytes = u64(off + 0x30);          // real size
            if (mftBytes == 0 || mftBytes > kMaxMftBytes) return false;
            return ntfs::ParseRunList(rec.data() + off + runOff,
                                      attrLen - runOff, runs);
        }
        off += attrLen;
        if (off >= limit) break;
    }
    return false;
}

}  // namespace

// Attempts an MFT scan. Returns false -- having changed nothing -- whenever
// the fast path is unavailable, so the caller falls back to the directory
// walk. There is deliberately no error surfaced to the user for this: the
// slower path produces the same answer.
bool ScanMft(const std::wstring& root, Progress* progress, ScanResult& out) {
    if (root.size() < 2 || root[1] != L':') return false;
    if (!IsElevated()) return false;

    // MFT reading only makes sense for a local NTFS volume.
    {
        wchar_t rootPath[4] = {root[0], L':', L'\\', 0};
        if (GetDriveTypeW(rootPath) != DRIVE_FIXED) return false;

        wchar_t fs[16] = {};
        if (!GetVolumeInformationW(rootPath, nullptr, 0, nullptr, nullptr,
                                   nullptr, fs, 15)) {
            return false;
        }
        if (wcscmp(fs, L"NTFS") != 0) return false;
    }

    const std::wstring volPath = std::wstring(L"\\\\.\\") + root[0] + L':';

    // FILE_READ_DATA only, and sharing everything: this must never interfere
    // with a volume that is in active use.
    // Overlapped, so the record loop below can keep one chunk read in
    // flight while it parses the previous one.
    Handle vol(CreateFileW(volPath.c_str(), FILE_READ_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                           nullptr));
    if (!vol.valid()) return false;

    std::vector<uint8_t> boot(512);
    if (!ReadAt(vol.get(), 0, boot.data(), 512)) return false;

    const ntfs::BootInfo bi = ntfs::ParseBootSector(boot.data(), boot.size());
    if (!bi.valid) return false;

    std::vector<ntfs::DataRun> runs;
    uint64_t mftBytes = 0;
    if (!ReadMftRunList(vol.get(), bi, runs, mftBytes) || runs.empty()) {
        return false;
    }

    const uint64_t recordCount = mftBytes / bi.bytesPerRecord;
    // kRootRecord (5) is indexed unconditionally by the tree build below, so
    // a table that does not contain it is not merely empty - it would read
    // past the end of every vector sized from this count. A volume claiming
    // a handful of records is malformed or hostile either way.
    if (recordCount <= ntfs::kRootRecord || recordCount > kMaxRecords) {
        return false;
    }

    std::vector<Entry> entries;
    try {
        entries.resize(static_cast<size_t>(recordCount));
    } catch (...) {
        return false;   // fall back rather than dying on a huge volume
    }

    NamePool pool;
    // Inside a try for the same reason `entries` is: this is hundreds of
    // megabytes on a large volume, and failing to get it should fall back
    // to the directory walk rather than end the scan.
    try {
        pool.data.reserve(static_cast<size_t>(recordCount) * 12);
    } catch (...) {
        return false;
    }

    // ---- pass one: read the MFT and parse every record -------------------
    //
    // The read plan comes first: every non-sparse run split into
    // record-aligned chunks, each knowing which record number it starts at.
    // Laying the plan out up front is what lets the loop keep the next
    // chunk's read in flight while this one is parsed - the disk and the
    // CPU work at the same time instead of taking turns.

    struct Segment {
        uint64_t offset;
        uint32_t bytes;
        uint64_t firstRecord;
    };
    std::vector<Segment> segments;
    {
        uint64_t rec = 0;
        for (const ntfs::DataRun& run : runs) {
            if (run.sparse) {
                // Unallocated stretch of the MFT: skip the record numbers
                // it covers rather than misaligning everything after it.
                rec += SatMul(run.clusters, bi.bytesPerCluster) /
                       bi.bytesPerRecord;
                if (rec >= recordCount) break;
                continue;
            }
            // Both fields come off the disk and are bounded only by
            // their width, so the products wrap without this. A wrapped
            // plan reads the right volume at the wrong offsets and files
            // records under the wrong numbers: a silently wrong tree.
            if (bi.bytesPerCluster != 0 &&
                (run.clusters > UINT64_MAX / bi.bytesPerCluster ||
                 static_cast<uint64_t>(run.lcn) >
                     UINT64_MAX / bi.bytesPerCluster)) {
                continue;   // implausible run: skip it, keep the rest
            }
            uint64_t remaining = run.clusters * bi.bytesPerCluster;
            uint64_t off = static_cast<uint64_t>(run.lcn) * bi.bytesPerCluster;
            while (remaining > 0 && rec < recordCount) {
                const uint32_t want = static_cast<uint32_t>(
                    std::min<uint64_t>(remaining, kChunkBytes));
                const uint32_t aligned = want - (want % bi.bytesPerRecord);
                if (aligned == 0) break;
                segments.push_back(Segment{off, aligned, rec});
                off += aligned;
                remaining -= aligned;
                rec += aligned / bi.bytesPerRecord;
            }
        }
    }

    uint64_t files = 0, dirs = 0, bytes = 0;
    uint64_t hardlinkFiles = 0, hardlinkBytes = 0;

    // Declared after `vol` on purpose: destruction runs in reverse, so an
    // in-flight read is drained before the handle it targets closes.
    AsyncRead ioA(vol.get(), kChunkBytes);
    AsyncRead ioB(vol.get(), kChunkBytes);
    AsyncRead* io[2] = {&ioA, &ioB};
    if (!ioA.Ready() || !ioB.Ready()) return false;

    size_t issued = 0;   // segments handed to the disk so far

    for (size_t s = 0; s < segments.size(); ++s) {
        if (progress && progress->cancel.load(std::memory_order_relaxed)) {
            return false;   // AsyncRead destructors drain in-flight I/O
        }

        // Normally segment s went out an iteration ago; the first segment,
        // or any whose Issue failed, is picked up synchronously here.
        if (issued == s) {
            if (io[s & 1]->Issue(segments[s].offset, segments[s].bytes)) {
                ++issued;
            }
        }

        AsyncRead& cur = *io[s & 1];
        const bool ok = (issued > s) && cur.Wait();

        // Fill the other buffer before touching this one.
        if (issued == s + 1 && issued < segments.size()) {
            if (io[issued & 1]->Issue(segments[issued].offset,
                                      segments[issued].bytes)) {
                ++issued;
            }
        }

        const Segment& seg = segments[s];
        if (!ok) {
            // A bad sector mid-table is not fatal: skip the chunk and keep
            // going. The result is incomplete, not wrong.
            continue;
        }

        for (uint32_t p = 0, ri = 0; p + bi.bytesPerRecord <= seg.bytes;
             p += bi.bytesPerRecord, ++ri) {
            const uint64_t recordIndex = seg.firstRecord + ri;
            if (recordIndex >= recordCount) break;
            const size_t idx = static_cast<size_t>(recordIndex);

            uint8_t* r = cur.Data() + p;
            if (std::memcmp(r, "FILE", 4) != 0) continue;
            if (!ntfs::ApplyFixups(r, bi.bytesPerRecord,
                                   bi.bytesPerSector)) {
                continue;
            }

            const ntfs::RecordInfo info =
                ntfs::ParseRecord(r, bi.bytesPerRecord);
            if (!info.valid || !info.inUse) continue;
            if (info.isExtension) continue;

            // The root directory names itself "." in its own $FILE_NAME,
            // which the name filter rightly refuses as a path component.
            // The root is not one: its name is the volume, set by the
            // caller, and this record only has to exist so that every
            // top-level entry finds its parent in use. Without it the
            // table is counted in full and the tree comes back empty.
            if (idx == ntfs::kRootRecord) {
                if (info.isDir) {
                    Entry& e = entries[idx];
                    e.used   = true;
                    e.isDir  = true;
                    e.parent = ntfs::kRootRecord;
                }
                continue;
            }

            if (!info.hasName) continue;
            // Compared at full width, so a 48-bit reference past the
            // table is rejected instead of wrapping onto a valid index.
            if (info.parent >= recordCount) continue;   // dangling parent

            Entry& e = entries[idx];
            e.used   = true;
            e.parent = static_cast<uint32_t>(info.parent);   // range-checked
            e.isDir  = info.isDir;
            e.size   = info.isDir ? 0 : info.size;
            e.nameOff = pool.Add(info.name, e.nameLen);
            // One record is one file however many names point at it, so the
            // totals here are already right - this only records that the
            // file is reachable from somewhere else too, which is what
            // decides whether deleting it frees anything.
            e.hardlink = !info.isDir && info.links > 1;

            if (info.isDir) {
                ++dirs;
            } else {
                ++files;
                bytes = SatAdd(bytes, info.size);
                if (e.hardlink) {
                    ++hardlinkFiles;
                    hardlinkBytes = SatAdd(hardlinkBytes, info.size);
                }
            }
        }

        if (progress) {
            progress->files.store(files, std::memory_order_relaxed);
            progress->dirs.store(dirs, std::memory_order_relaxed);
            progress->bytes.store(bytes, std::memory_order_relaxed);
        }
    }

    if (files == 0 && dirs == 0) return false;   // nothing usable; fall back

    // Without a usable root record nothing can attach to the tree, and an
    // empty tree under a full file count is worse than the slow path. This
    // is checked here, after the reads have drained, rather than inside
    // the loop where a read may still be in flight.
    if (recordCount <= ntfs::kRootRecord ||
        !entries[static_cast<size_t>(ntfs::kRootRecord)].used) {
        return false;
    }

    // Everything from here to the end of the tree build is inside one
    // try: these are the five largest allocations in the function, and
    // this returns false on failure so the caller falls back to the
    // directory walk. Without it a crafted volume turned "unavailable
    // fast path" into a dead process, and the headless export had no
    // catch above it at all.
    try {

    // ---- pass two: count children so every vector is sized exactly -------

    std::vector<uint32_t> childCount(static_cast<size_t>(recordCount), 0);
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        if (!e.used) continue;
        if (i == ntfs::kRootRecord) continue;
        if (e.parent == i) continue;                 // self-parent: skip
        if (!entries[e.parent].used) continue;       // orphan
        if (!entries[e.parent].isDir) continue;      // parent is not a folder
        ++childCount[e.parent];
    }

    // ---- pass three: build the tree, iteratively --------------------------

    out.root.name = root;
    out.root.dir  = true;
    out.root.cat  = Cat::Directory;
    out.root.children.clear();

    // Breadth-first from the root record. Recursion here would be bounded by
    // directory nesting depth, which a hostile volume controls.
    // Depth travels with the frame: `Node` owns its children by value, so a
    // tree deeper than the stack can unwind cannot be destroyed, and that
    // crash would land when the tree is released rather than when the
    // hostile volume was read.
    struct Pending { uint32_t record; Node* node; uint32_t depth; };
    std::vector<Pending> queue;
    queue.push_back(Pending{ntfs::kRootRecord, &out.root, 0});

    // Guards against a parent cycle, which a corrupt MFT can express and
    // which would otherwise expand for ever.
    std::vector<bool> visited(static_cast<size_t>(recordCount), false);
    visited[ntfs::kRootRecord] = true;

    // Bucket children by parent in one pass so the expansion below does not
    // rescan the whole table per directory.
    std::vector<uint32_t> childStart(static_cast<size_t>(recordCount) + 1, 0);
    for (size_t i = 0; i < childCount.size(); ++i) {
        childStart[i + 1] = childStart[i] + childCount[i];
    }
    std::vector<uint32_t>().swap(childCount);   // no longer needed

    std::vector<uint32_t> childIds(childStart.back(), 0);
    {
        std::vector<uint32_t> cursor(childStart.begin(), childStart.end() - 1);
        for (size_t i = 0; i < entries.size(); ++i) {
            const Entry& e = entries[i];
            if (!e.used || i == ntfs::kRootRecord) continue;
            if (e.parent == i) continue;
            if (!entries[e.parent].used || !entries[e.parent].isDir) continue;
            childIds[cursor[e.parent]++] = static_cast<uint32_t>(i);
        }
    }

    while (!queue.empty()) {
        const Pending p = queue.back();
        queue.pop_back();

        const uint32_t first = childStart[p.record];
        const uint32_t last  = childStart[p.record + 1];
        if (last <= first) continue;
        if (p.depth >= kMaxTreeDepth) continue;   // stop, do not descend

        p.node->children.reserve(last - first);

        // Record which id produced which child, so the second loop can pair
        // them up directly. Deriving the pairing from the loop index instead
        // desynchronises the moment any child is skipped.
        std::vector<uint32_t> added;
        added.reserve(last - first);

        for (uint32_t k = first; k < last; ++k) {
            const uint32_t id = childIds[k];
            if (id >= recordCount || visited[id]) continue;
            visited[id] = true;

            const Entry& e = entries[id];
            Node child(pool.Get(e.nameOff, e.nameLen), e.isDir);
            child.hardlink = e.hardlink;
            if (e.isDir) {
                child.cat = Cat::Directory;
            } else {
                child.size  = e.size;
                child.files = 1;
                child.cat   = CategoryForFile(child.name);
            }
            p.node->children.push_back(std::move(child));
            added.push_back(id);
        }

        // Queue directories only now the vector has stopped growing: it was
        // reserved for the full range and never exceeds it, so these pointers
        // stay valid for the rest of the build.
        for (size_t i = 0; i < added.size() && i < p.node->children.size();
             ++i) {
            if (entries[added[i]].isDir) {
                queue.push_back(
                    Pending{added[i], &p.node->children[i], p.depth + 1});
            }
        }
    }

    out.stats.fileCount     = files;
    out.stats.dirCount      = dirs;
    out.stats.hardlinkFiles = hardlinkFiles;
    out.stats.hardlinkBytes = hardlinkBytes;
    out.stats.bytes     = 0;   // filled in by the caller's roll-up

    } catch (...) {
        // The contract is that a failure here changes nothing and the
        // caller falls back to the directory walk. The tree may be
        // half-built by this point, so it goes too.
        out.root.children.clear();
        out.stats = ScanStats{};
        return false;
    }
    return true;
}

}  // namespace spindle
