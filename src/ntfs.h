// Spindle - NTFS on-disk structure parsing.
//
// Reading the Master File Table directly is what makes a scan take seconds
// instead of minutes: one large sequential read of the volume's own index,
// rather than a syscall round trip per directory.
//
// Everything here operates on plain byte buffers and knows nothing about
// Windows, for two reasons. It can be compiled and fuzzed on any host, and
// the separation keeps the parsing -- which is the dangerous part -- away
// from the I/O.
//
// SECURITY POSTURE: every byte handled here is attacker-controlled. A crafted
// or corrupt filesystem, a malicious VHD, or a USB stick with a hand-edited
// boot sector all reach this code, and it runs elevated because raw volume
// access requires it. Every field read is bounds-checked against the buffer
// actually supplied; no structure is trusted to describe itself honestly, and
// no length, offset or count from the disk is used without validation.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace spindle::ntfs {

// ------------------------------------------------------------- boot sector

struct BootInfo {
    uint32_t bytesPerSector   = 0;
    uint32_t bytesPerCluster  = 0;
    uint32_t bytesPerRecord   = 0;   // MFT file record size, normally 1024
    uint64_t mftStartCluster  = 0;
    uint64_t totalSectors     = 0;
    bool     valid            = false;
};

// Parses the volume boot record. Returns valid=false for anything that is not
// a plausible NTFS boot sector, including sane-looking values that are
// self-inconsistent.
BootInfo ParseBootSector(const uint8_t* data, size_t len);

// --------------------------------------------------------------- run lists

struct DataRun {
    int64_t  lcn      = 0;       // starting cluster; meaningless when sparse
    uint64_t clusters = 0;
    bool     sparse   = false;
};

// Decodes a non-resident attribute's run list. Run lists are a compact
// variable-length encoding, so a corrupt length nibble is the obvious way to
// walk a parser off the end of its buffer; every step is bounds-checked and
// the run count is capped.
bool ParseRunList(const uint8_t* data, size_t len, std::vector<DataRun>& out);

// ------------------------------------------------------------ file records

inline constexpr uint32_t kAttrStandardInfo = 0x10;
inline constexpr uint32_t kAttrAttributeList = 0x20;
inline constexpr uint32_t kAttrFileName     = 0x30;
inline constexpr uint32_t kAttrData         = 0x80;
inline constexpr uint32_t kAttrEnd          = 0xFFFFFFFF;

// The NTFS root directory is always MFT record 5.
inline constexpr uint32_t kRootRecord = 5;

struct RecordInfo {
    bool     valid     = false;  // parsed successfully
    bool     inUse     = false;  // record flag 0x01
    bool     isDir     = false;  // record flag 0x02
    bool     hasName   = false;
    bool     isExtension = false;  // continuation of another record
    uint64_t size      = 0;      // unnamed $DATA logical size
    // All 48 bits of the $FILE_NAME parent reference. Narrowing this to
    // 32 before the caller's range check let an index far past the table
    // truncate onto a valid one, most usefully onto the root, which
    // grafts a file to the volume root so every path shown for it, and
    // every action taken on it, names somewhere it does not live.
    uint64_t parent    = 0;      // parent directory's MFT index
    // Number of directory entries pointing at this record. Above one the
    // file exists in several places at once and deleting any single one of
    // them frees nothing -- which is the whole of the WinSxS illusion.
    uint16_t links     = 0;
    std::wstring name;
};

// Applies the update sequence array ("fixups") to a file record in place.
//
// NTFS stores a check value in the last two bytes of every sector of a
// record and keeps the real bytes in an array in the header. Skipping this
// leaves the record subtly corrupt at each sector boundary. Returns false if
// the array does not describe the buffer it was handed, or if a check value
// does not match -- which means a torn write.
bool ApplyFixups(uint8_t* record, size_t len, uint32_t bytesPerSector);

// Parses one MFT file record. `record` must already have had fixups applied.
RecordInfo ParseRecord(const uint8_t* record, size_t len);

// ------------------------------------------------------------- $MFT itself

// Refuse absurd table sizes outright. A real MFT is roughly 1 KB per file;
// this bound corresponds to a volume with hundreds of millions of files.
inline constexpr uint64_t kMaxMftBytes = 8ull << 30;

// Reads the extent of the table from record 0, the $MFT file's own record,
// which must already have had fixups applied: the run list of its unnamed
// non-resident $DATA, and the table's real size in bytes. False for a
// record that is not a file record, has no such stream, or claims a size
// of zero or past kMaxMftBytes.
bool ParseMftDataRuns(const uint8_t* record, size_t len,
                      std::vector<DataRun>& runs, uint64_t& mftBytes);

// ------------------------------------------------------------------ limits

// Caps that bound the work a hostile filesystem can induce. Each is far above
// anything a real volume produces.
inline constexpr size_t   kMaxRuns          = 1u << 20;
inline constexpr size_t   kMaxAttrsPerRec   = 256;
inline constexpr uint32_t kMaxNameChars     = 255;   // NTFS limit
inline constexpr uint32_t kMinRecordSize    = 256;
inline constexpr uint32_t kMaxRecordSize    = 4096;
inline constexpr uint32_t kMaxBytesPerSector = 4096;
inline constexpr uint32_t kMaxClusterSize   = 1u << 26;   // 64 MB

}  // namespace spindle::ntfs
