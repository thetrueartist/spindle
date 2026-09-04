// Spindle - assembling a tree from Master File Table records.
//
// mft.cpp reads the table off the volume; everything that turns those
// records into the tree lives here, with no Windows in it, so it runs on any
// host against an image made by mkntfs and against tables built to be
// hostile. The rules enforced here are the ones a crafted or corrupt volume
// would otherwise turn into a wrong tree or a dead process: a parent
// reference counts only if it names an in-use directory inside the table,
// a record is placed once however many paths claim it, a cycle cannot
// expand for ever, nesting stops at kMaxTreeDepth, and the root record must
// exist for anything to attach to.
#pragma once

#include "spindle.h"
#include "ntfs.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace spindle::mft {

// Cap on records, and so on the tree the caller ends up holding. A real MFT
// runs to about 1 KB per file; this is a volume with tens of millions.
inline constexpr uint64_t kMaxRecords = 64u << 20;

class Assembler {
public:
    // Sizes the table. False for a count that cannot hold the root record
    // or exceeds kMaxRecords, or when the memory for it is not there; the
    // caller then falls back to the directory walk.
    bool Begin(uint64_t recordCount);

    // One record, fixups applied in place. Records that are not in use, are
    // extensions of another, carry no usable name, or point outside the
    // table are dropped here and never reach the tree.
    void Feed(uint64_t recordIndex, uint8_t* record, uint32_t bytesPerRecord,
              uint32_t bytesPerSector);

    // Consecutive records as they arrive from one read.
    void FeedChunk(uint64_t firstRecord, uint8_t* data, uint32_t bytes,
                   uint32_t bytesPerRecord, uint32_t bytesPerSector);

    // Running totals of what was fed, for the progress display. They count
    // the table, so a record that no path from the root reaches is still
    // in them.
    uint64_t Files() const { return files_; }
    uint64_t Dirs() const { return dirs_; }
    uint64_t Bytes() const { return bytes_; }

    // Builds the tree under `rootName` into `out`, leaving directory sizes
    // for the caller's roll-up. False, with `out` emptied, when nothing
    // usable was fed, the root record never appeared, memory ran out or
    // `cancel` was raised.
    bool Finish(const std::wstring& rootName, ScanResult& out,
                const std::atomic<bool>* cancel);

private:
    // One entry per record, indexed by record number, so a parent reference
    // is a direct index rather than a lookup.
    struct Entry {
        uint32_t parent   = 0;
        uint32_t nameOff  = 0;   // into the pool
        uint16_t nameLen  = 0;
        bool     isDir    = false;
        bool     used     = false;
        bool     hardlink = false;   // more than one name points at it
        uint64_t size     = 0;
    };

    // Names live in one buffer rather than a string per entry. A million
    // separate small allocations costs both time and a header apiece; this
    // is one buffer and a six-byte reference per record.
    struct NamePool {
        std::vector<wchar_t> data;
        static constexpr uint32_t kNoName = 0xFFFFFFFFu;
        uint32_t Add(const std::wstring& s, uint16_t& lenOut);
        std::wstring Get(uint32_t off, uint16_t len) const;
    };

    std::vector<Entry> entries_;
    NamePool           pool_;
    uint64_t           recordCount_   = 0;
    uint64_t           files_         = 0;
    uint64_t           dirs_          = 0;
    uint64_t           bytes_         = 0;
    uint64_t           hardlinkFiles_ = 0;
    uint64_t           hardlinkBytes_ = 0;
};

}  // namespace spindle::mft
