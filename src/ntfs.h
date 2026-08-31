// Spindle - NTFS on-disk structures.
//
// Everything in this header and its .cpp handles nothing but bytes: no
// windows.h, no file handles, no assumption that the input came from a real
// volume. That is a security decision, not a style one. Reading the Master
// File Table means interpreting raw disk structures while running elevated,
// and a crafted VHD, a corrupt volume or a hand-edited USB stick all reach
// this code. Keeping it pure lets the whole parser run under ASan and UBSan
// on any host, and be fuzzed - which is how two real bugs were found in it.
//
// Layout references: the NTFS documentation in the Linux-NTFS project and
// Microsoft's published $Boot/$MFT descriptions. Offsets below are the
// on-disk little-endian layout, stable since NTFS 3.1 (Windows XP).

#ifndef SPINDLE_NTFS_H_
#define SPINDLE_NTFS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace spindle {
namespace ntfs {

// The file record number of the volume root directory, fixed by the format.
inline constexpr uint64_t kRootFrs = 5;

// Attribute type codes.
inline constexpr uint32_t kAttrStandardInfo = 0x10;
inline constexpr uint32_t kAttrAttributeList = 0x20;
inline constexpr uint32_t kAttrFileName = 0x30;
inline constexpr uint32_t kAttrData = 0x80;
inline constexpr uint32_t kAttrReparsePoint = 0xC0;
inline constexpr uint32_t kAttrEnd = 0xFFFFFFFF;

// $FILE_NAME namespaces. DOS is the 8.3 alias and never the display name.
inline constexpr uint8_t kNameSpaceDos = 2;

// ------------------------------------------------------------------- cursor

// Every field read out of a disk buffer goes through one of these. A read
// past the end does not return garbage or fault: it marks the cursor failed
// and yields zero, and the caller checks ok() before trusting anything
// derived from the values. This is the property the fuzzer leans on.
class ByteCursor {
public:
    ByteCursor(const uint8_t* data, size_t len) : p_(data), len_(len) {}

    bool ok() const { return !fail_; }
    size_t offset() const { return off_; }
    size_t remaining() const { return fail_ ? 0 : len_ - off_; }

    bool Seek(size_t off) {
        if (off > len_) {
            fail_ = true;
            return false;
        }
        off_ = off;
        return true;
    }

    bool Skip(size_t n) {
        if (n > len_ - off_ || off_ > len_) {
            fail_ = true;
            return false;
        }
        off_ += n;
        return true;
    }

    uint8_t U8() {
        if (fail_ || len_ - off_ < 1) {
            fail_ = true;
            return 0;
        }
        return p_[off_++];
    }

    uint16_t U16() {
        if (fail_ || len_ - off_ < 2) {
            fail_ = true;
            return 0;
        }
        const uint16_t v = static_cast<uint16_t>(
            static_cast<uint16_t>(p_[off_]) |
            static_cast<uint16_t>(p_[off_ + 1]) << 8);
        off_ += 2;
        return v;
    }

    uint32_t U32() {
        if (fail_ || len_ - off_ < 4) {
            fail_ = true;
            return 0;
        }
        uint32_t v = 0;
        for (size_t i = 0; i < 4; ++i) {
            v |= static_cast<uint32_t>(p_[off_ + i]) << (8 * i);
        }
        off_ += 4;
        return v;
    }

    uint64_t U64() {
        if (fail_ || len_ - off_ < 8) {
            fail_ = true;
            return 0;
        }
        uint64_t v = 0;
        for (size_t i = 0; i < 8; ++i) {
            v |= static_cast<uint64_t>(p_[off_ + i]) << (8 * i);
        }
        off_ += 8;
        return v;
    }

    // A window of `n` bytes at the current offset, or nullptr. The cursor
    // does not advance; pair with Skip.
    const uint8_t* Peek(size_t n) {
        if (fail_ || len_ - off_ < n || off_ > len_) {
            fail_ = true;
            return nullptr;
        }
        return p_ + off_;
    }

private:
    const uint8_t* p_;
    size_t len_;
    size_t off_ = 0;
    bool fail_ = false;
};

// -------------------------------------------------------------- boot sector

struct BootSector {
    uint32_t bytesPerSector = 0;
    uint32_t sectorsPerCluster = 0;
    uint64_t clusterBytes = 0;
    uint64_t totalSectors = 0;
    uint64_t totalClusters = 0;
    uint64_t mftLcn = 0;         // first cluster of the $MFT
    uint32_t recordBytes = 0;    // size of one FILE record
};

// Parses and validates the first sector of an NTFS volume. Rejects anything
// that is not self-consistent: wrong OEM id, sector or cluster sizes that are
// not sane powers of two, an MFT start beyond the volume, a record size the
// reader could not handle.
bool ParseBootSector(const uint8_t* data, size_t len, BootSector& out);

// ------------------------------------------------------------------- fixups

// Multi-sector records end every sector with a two-byte update sequence
// number, with the displaced real bytes kept in the update sequence array.
// Verifies each sector's tail matches and restores the original bytes, in
// place. A mismatch means a torn write and the record cannot be trusted.
bool ApplyFixups(uint8_t* record, size_t len, uint32_t bytesPerSector);

// ---------------------------------------------------------------- run lists

// One extent of a non-resident attribute. `sparse` runs occupy no clusters
// on disk and carry no meaningful lcn.
struct Run {
    uint64_t lcn = 0;
    uint64_t clusters = 0;
    bool sparse = false;
};

// Decodes a mapping-pairs array. Offsets are signed deltas from the previous
// run's LCN with a field width of 0 to 8 bytes; both fuzz-found bugs in this
// file lived here (see ntfs.cpp). Rejects a list whose total length exceeds
// `maxClusters` or whose LCN leaves [0, maxClusters), so a hostile list
// cannot direct reads at arbitrary disk offsets or claim absurd sizes.
bool DecodeRunList(const uint8_t* data, size_t len, uint64_t maxClusters,
                   std::vector<Run>& out);

// ------------------------------------------------------------- file records

struct FileRecord {
    bool inUse = false;
    bool isDir = false;
    bool isReparse = false;
    bool hasName = false;
    uint64_t baseFrs = 0;     // nonzero: extension record, not a file
    uint64_t parentFrs = 0;
    uint64_t size = 0;        // unnamed $DATA stream, resident or not
    std::wstring name;        // best $FILE_NAME (never the DOS alias)
};

// Parses one FILE record that has already had its fixups applied. Returns
// false when the record is malformed; a false return leaves no partial state
// worth reading. A record with no unnamed $DATA (a directory, or a file
// whose data lives in an extension record) reports the $FILE_NAME size
// instead, which Windows keeps only lazily up to date - documented in the
// known limits.
bool ParseFileRecord(const uint8_t* data, size_t len, FileRecord& out);

// Extracts the unnamed $DATA attribute's run list from a FILE record, for
// reading the $MFT itself. Returns false if the record has no non-resident
// unnamed $DATA. `dataSize` reports the attribute's real (byte) size.
bool ExtractDataRuns(const uint8_t* data, size_t len, uint64_t maxClusters,
                     std::vector<Run>& runs, uint64_t& dataSize);

}  // namespace ntfs
}  // namespace spindle

#endif  // SPINDLE_NTFS_H_
