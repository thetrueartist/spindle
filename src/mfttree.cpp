// Spindle - assembling a tree from Master File Table records. See mfttree.h.

#include "mfttree.h"

#include <algorithm>
#include <cstring>

namespace spindle::mft {

// Returns kNoName once the pool has grown past what a 32-bit offset can
// address. Truncating instead would hand back a reference into the wrong
// part of the buffer, and a name is what paths are built from.
uint32_t Assembler::NamePool::Add(const std::wstring& s, uint16_t& lenOut) {
    if (data.size() >= kNoName) {
        lenOut = 0;
        return kNoName;
    }
    const uint32_t off = static_cast<uint32_t>(data.size());
    const size_t n = std::min<size_t>(s.size(), 0xFFFF);
    data.insert(data.end(), s.begin(),
                s.begin() + static_cast<std::ptrdiff_t>(n));
    lenOut = static_cast<uint16_t>(n);
    return off;
}

std::wstring Assembler::NamePool::Get(uint32_t off, uint16_t len) const {
    if (off == kNoName) return std::wstring();
    if (len == 0 || off > data.size() || data.size() - off < len) {
        return std::wstring();
    }
    return std::wstring(data.begin() + off, data.begin() + off + len);
}

bool Assembler::Begin(uint64_t recordCount) {
    // kRootRecord (5) is indexed unconditionally by Finish, so a table that
    // cannot contain it is not merely empty: every vector sized from this
    // count would be read past its end. A volume claiming a handful of
    // records is malformed or hostile either way.
    if (recordCount <= ntfs::kRootRecord || recordCount > kMaxRecords) {
        return false;
    }
    try {
        entries_.assign(static_cast<size_t>(recordCount), Entry{});
        // Hundreds of megabytes on a large volume, and failing to get it
        // must mean the directory walk rather than the end of the scan.
        pool_.data.clear();
        pool_.data.reserve(static_cast<size_t>(recordCount) * 12);
    } catch (...) {
        std::vector<Entry>().swap(entries_);
        std::vector<wchar_t>().swap(pool_.data);
        recordCount_ = 0;
        return false;
    }
    recordCount_ = recordCount;
    files_ = dirs_ = bytes_ = hardlinkFiles_ = hardlinkBytes_ = 0;
    return true;
}

void Assembler::Feed(uint64_t recordIndex, uint8_t* record,
                     uint32_t bytesPerRecord, uint32_t bytesPerSector) {
    if (record == nullptr || recordIndex >= recordCount_) return;
    if (bytesPerRecord < 4 || std::memcmp(record, "FILE", 4) != 0) return;
    if (!ntfs::ApplyFixups(record, bytesPerRecord, bytesPerSector)) return;

    const ntfs::RecordInfo info = ntfs::ParseRecord(record, bytesPerRecord);
    if (!info.valid || !info.inUse || info.isExtension) return;

    const size_t idx = static_cast<size_t>(recordIndex);

    // The root directory names itself "." in its own $FILE_NAME, which the
    // name filter rightly refuses as a path component. The root is not
    // one: its name is the volume, set by the caller, and this record only
    // has to exist so that every top-level entry finds its parent in use.
    // Without it the table is counted in full and the tree comes back
    // empty.
    if (idx == ntfs::kRootRecord) {
        if (info.isDir) {
            Entry& e = entries_[idx];
            e.used   = true;
            e.isDir  = true;
            e.parent = ntfs::kRootRecord;
        }
        return;
    }

    if (!info.hasName) return;
    // Compared at full width, so a 48-bit reference past the table is
    // rejected instead of wrapping onto a valid index.
    if (info.parent >= recordCount_) return;   // dangling parent

    Entry& e = entries_[idx];
    e.used    = true;
    e.parent  = static_cast<uint32_t>(info.parent);   // range-checked
    e.isDir   = info.isDir;
    e.size    = info.isDir ? 0 : info.size;
    e.nameOff = pool_.Add(info.name, e.nameLen);
    // One record is one file however many names point at it, so the totals
    // are already right; this only records that the file is reachable from
    // somewhere else too, which is what decides whether deleting it frees
    // anything.
    e.hardlink = !info.isDir && info.links > 1;

    if (info.isDir) {
        ++dirs_;
    } else {
        ++files_;
        bytes_ = SatAdd(bytes_, info.size);
        if (e.hardlink) {
            ++hardlinkFiles_;
            hardlinkBytes_ = SatAdd(hardlinkBytes_, info.size);
        }
    }
}

void Assembler::FeedChunk(uint64_t firstRecord, uint8_t* data, uint32_t bytes,
                          uint32_t bytesPerRecord, uint32_t bytesPerSector) {
    if (data == nullptr || bytesPerRecord == 0) return;
    for (uint32_t p = 0, ri = 0; p + bytesPerRecord <= bytes;
         p += bytesPerRecord, ++ri) {
        const uint64_t recordIndex = firstRecord + ri;
        if (recordIndex >= recordCount_) break;
        Feed(recordIndex, data + p, bytesPerRecord, bytesPerSector);
    }
}

bool Assembler::Finish(const std::wstring& rootName, ScanResult& out,
                       const std::atomic<bool>* cancel) {
    const auto cancelled = [cancel] {
        return cancel != nullptr && cancel->load(std::memory_order_relaxed);
    };
    out.root.children.clear();
    if (recordCount_ == 0) return false;               // Begin never ran
    if (files_ == 0 && dirs_ == 0) return false;       // nothing usable
    // Without a usable root record nothing can attach to the tree, and an
    // empty tree under a full file count is worse than the slow path.
    if (!entries_[static_cast<size_t>(ntfs::kRootRecord)].used) return false;

    // Everything from here to the end is inside one try: these are the
    // largest allocations of the whole scan, and a failure must mean the
    // directory walk rather than a dead process. Without it a crafted
    // volume turned "unavailable fast path" into a crash, and the headless
    // export had no catch above it at all.
    try {

    // ---- pass one: count children so every vector is sized exactly -------
    // Polled here and in pass two so a drive switch that cancels this scan
    // returns promptly instead of waiting out the whole tree build.
    if (cancelled()) return false;

    const size_t count = static_cast<size_t>(recordCount_);
    std::vector<uint32_t> childCount(count, 0);
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        if (!e.used) continue;
        if (i == ntfs::kRootRecord) continue;
        if (e.parent == i) continue;                  // self-parent: skip
        if (!entries_[e.parent].used) continue;       // orphan
        if (!entries_[e.parent].isDir) continue;      // parent is not a folder
        ++childCount[e.parent];
    }

    // Bucket children by parent in one pass so the expansion below does
    // not rescan the whole table per directory.
    std::vector<uint32_t> childStart(count + 1, 0);
    for (size_t i = 0; i < count; ++i) {
        childStart[i + 1] = childStart[i] + childCount[i];
    }
    std::vector<uint32_t>().swap(childCount);   // no longer needed

    std::vector<uint32_t> childIds(childStart.back(), 0);
    {
        std::vector<uint32_t> cursor(childStart.begin(), childStart.end() - 1);
        for (size_t i = 0; i < entries_.size(); ++i) {
            const Entry& e = entries_[i];
            if (!e.used || i == ntfs::kRootRecord) continue;
            if (e.parent == i) continue;
            if (!entries_[e.parent].used || !entries_[e.parent].isDir) continue;
            childIds[cursor[e.parent]++] = static_cast<uint32_t>(i);
        }
    }

    // ---- pass two: build the tree, iteratively ---------------------------

    out.root.name = rootName;
    out.root.dir  = true;
    out.root.cat  = Cat::Directory;
    out.root.children.clear();

    // From the root record outwards. Recursion here would be bounded by
    // directory nesting depth, which a hostile volume controls. Depth
    // travels with the frame: Node owns its children by value, so a tree
    // deeper than the stack can unwind cannot even be destroyed, and that
    // crash would land when the tree is released rather than when the
    // hostile volume was read.
    struct Pending { uint32_t record; Node* node; uint32_t depth; };
    std::vector<Pending> queue;
    queue.push_back(Pending{ntfs::kRootRecord, &out.root, 0});

    // Guards against a parent cycle, which a corrupt MFT can express and
    // which would otherwise expand for ever.
    std::vector<bool> visited(count, false);
    visited[ntfs::kRootRecord] = true;

    // Which id produced which child, so the second loop below can pair
    // them up directly; deriving the pairing from the loop index instead
    // desynchronises the moment any child is skipped. One buffer for the
    // whole build rather than one allocation per directory.
    std::vector<uint32_t> added;

    uint64_t builtGuard = 0;
    while (!queue.empty()) {
        if ((++builtGuard & 0x3FFF) == 0 && cancelled()) {
            out.root.children.clear();
            return false;
        }
        const Pending p = queue.back();
        queue.pop_back();

        const uint32_t first = childStart[p.record];
        const uint32_t last  = childStart[p.record + 1];
        if (last <= first) continue;
        if (p.depth >= kMaxTreeDepth) continue;   // stop, do not descend

        p.node->children.reserve(last - first);
        added.clear();

        for (uint32_t k = first; k < last; ++k) {
            const uint32_t id = childIds[k];
            if (id >= count || visited[id]) continue;
            visited[id] = true;

            const Entry& e = entries_[id];
            Node child(pool_.Get(e.nameOff, e.nameLen), e.isDir);
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
        // reserved for the full range and never exceeds it, so these
        // pointers stay valid for the rest of the build.
        for (size_t i = 0; i < added.size() && i < p.node->children.size();
             ++i) {
            if (entries_[added[i]].isDir) {
                queue.push_back(
                    Pending{added[i], &p.node->children[i], p.depth + 1});
            }
        }
    }

    out.stats.fileCount     = files_;
    out.stats.dirCount      = dirs_;
    out.stats.hardlinkFiles = hardlinkFiles_;
    out.stats.hardlinkBytes = hardlinkBytes_;
    out.stats.bytes         = 0;   // filled in by the caller's roll-up

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

}  // namespace spindle::mft
