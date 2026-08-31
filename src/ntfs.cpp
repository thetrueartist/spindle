// Spindle - NTFS on-disk structure parsing.
//
// Pure bytes in, validated values out. Nothing in this file touches the
// operating system, and every offset, length and count read from the input
// is checked before use, because the input is a disk and disks lie: the
// volume may be corrupt, or crafted by someone who knows this code runs
// elevated.

#include "ntfs.h"

namespace spindle {
namespace ntfs {

namespace {

inline bool IsPow2(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

// Little-endian read of 1..8 bytes, used by the run-list decoder where the
// field width itself comes off the disk.
inline uint64_t ReadLe(const uint8_t* p, uint32_t n) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

}  // namespace

// -------------------------------------------------------------- boot sector

bool ParseBootSector(const uint8_t* data, size_t len, BootSector& out) {
    if (data == nullptr || len < 512) return false;
    ByteCursor c(data, len);

    c.Seek(3);
    const uint8_t* oem = c.Peek(8);
    if (oem == nullptr) return false;
    static const uint8_t kOem[8] = {'N', 'T', 'F', 'S', ' ', ' ', ' ', ' '};
    for (size_t i = 0; i < 8; ++i) {
        if (oem[i] != kOem[i]) return false;
    }

    c.Seek(0x0B);
    const uint32_t bps = c.U16();
    // 512 through 4096 covers every sector size NTFS has ever shipped on.
    if (!IsPow2(bps) || bps < 512 || bps > 4096) return false;

    const uint32_t rawSpc = c.U8();
    // Values above 0x80 are two's-complement exponents: 0xF8 means 2^8
    // sectors per cluster. Introduced alongside the >64K cluster sizes.
    uint64_t spc = 0;
    if (rawSpc > 0x80) {
        const uint32_t shift = 256u - rawSpc;
        if (shift > 25) return false;
        spc = 1ull << shift;
    } else {
        spc = rawSpc;
    }
    const uint64_t clusterBytes = spc * bps;
    // 2 MB is the largest cluster NTFS supports (Windows 10 1709 onwards).
    if (!IsPow2(spc) || clusterBytes == 0 || clusterBytes > (2ull << 20)) {
        return false;
    }

    c.Seek(0x28);
    const uint64_t totalSectors = c.U64();
    const uint64_t mftLcn = c.U64();
    if (totalSectors == 0) return false;
    const uint64_t totalClusters = totalSectors / spc;
    if (totalClusters == 0 || mftLcn >= totalClusters) return false;

    c.Seek(0x40);
    const uint8_t rawRec = c.U8();
    if (!c.ok()) return false;
    // Positive: clusters per record. Negative (two's complement): the record
    // is 2^|value| bytes, which is how every volume with clusters larger
    // than the record size expresses it.
    uint64_t recordBytes = 0;
    if (rawRec > 0x80) {
        const uint32_t shift = 256u - rawRec;
        if (shift > 12) return false;
        recordBytes = 1ull << shift;
    } else {
        recordBytes = static_cast<uint64_t>(rawRec) * clusterBytes;
    }
    // The reader slices the MFT into fixed records; anything outside the
    // sizes Windows actually formats is treated as corruption.
    if (!IsPow2(recordBytes) || recordBytes < 512 || recordBytes > 4096) {
        return false;
    }

    out.bytesPerSector = bps;
    out.sectorsPerCluster = static_cast<uint32_t>(spc);
    out.clusterBytes = clusterBytes;
    out.totalSectors = totalSectors;
    out.totalClusters = totalClusters;
    out.mftLcn = mftLcn;
    out.recordBytes = static_cast<uint32_t>(recordBytes);
    return true;
}

// ------------------------------------------------------------------- fixups

bool ApplyFixups(uint8_t* record, size_t len, uint32_t bytesPerSector) {
    if (record == nullptr || bytesPerSector == 0) return false;
    if (len < bytesPerSector || len % bytesPerSector != 0) return false;

    ByteCursor c(record, len);
    c.Seek(4);
    const uint16_t usaOffset = c.U16();
    const uint16_t usaCount = c.U16();
    if (!c.ok()) return false;

    // One sequence number plus one saved word per sector. A count that does
    // not match the record's own size means the header is lying about one of
    // them, and neither can then be trusted.
    const size_t sectors = len / bytesPerSector;
    if (usaCount != sectors + 1) return false;
    if (usaOffset < 6 ||
        static_cast<size_t>(usaOffset) + 2 * usaCount > len) {
        return false;
    }

    const uint8_t usn0 = record[usaOffset];
    const uint8_t usn1 = record[usaOffset + 1];

    for (size_t s = 0; s < sectors; ++s) {
        uint8_t* tail = record + (s + 1) * bytesPerSector - 2;
        // A tail that no longer carries the sequence number is a torn
        // multi-sector write; the record content is not usable.
        if (tail[0] != usn0 || tail[1] != usn1) return false;
        const size_t saved = static_cast<size_t>(usaOffset) + 2 * (s + 1);
        tail[0] = record[saved];
        tail[1] = record[saved + 1];
    }
    return true;
}

// ---------------------------------------------------------------- run lists

bool DecodeRunList(const uint8_t* data, size_t len, uint64_t maxClusters,
                   std::vector<Run>& out) {
    out.clear();
    if (data == nullptr || maxClusters == 0) return false;

    ByteCursor c(data, len);
    uint64_t lcn = 0;
    uint64_t total = 0;

    for (;;) {
        const uint8_t header = c.U8();
        if (!c.ok()) return false;    // ran off the end before the terminator
        if (header == 0) break;

        const uint32_t lenBytes = header & 0x0F;
        const uint32_t offBytes = header >> 4;
        if (lenBytes == 0 || lenBytes > 8 || offBytes > 8) return false;

        const uint8_t* lp = c.Peek(lenBytes);
        if (lp == nullptr) return false;
        c.Skip(lenBytes);
        const uint64_t clusters = ReadLe(lp, lenBytes);
        if (clusters == 0) return false;

        // A hostile list may claim any length at all; refusing anything the
        // volume could not physically hold keeps allocation and read sizes
        // derived from this list bounded.
        if (clusters > maxClusters - total) return false;
        total += clusters;

        if (offBytes == 0) {
            // Sparse run: no clusters on disk.
            Run r;
            r.clusters = clusters;
            r.sparse = true;
            out.push_back(r);
        } else {
            const uint8_t* op = c.Peek(offBytes);
            if (op == nullptr) return false;
            c.Skip(offBytes);
            uint64_t delta = ReadLe(op, offBytes);

            // Sign-extend the delta - but only when the field is narrower
            // than 64 bits. An eight-byte field is already complete, and the
            // original "value << (64 - 8*offBytes)" style extension shifted
            // by 64 exactly there: undefined behaviour, found by the fuzzer.
            if (offBytes < 8) {
                const uint64_t signBit = 1ull << (8 * offBytes - 1);
                if (delta & signBit) {
                    delta |= ~((signBit << 1) - 1);
                }
            }

            // Accumulate in unsigned arithmetic, where wraparound is defined,
            // then range-check the result. This replaces a signed
            // accumulation that a crafted delta could overflow - the second
            // fuzzer finding in this file.
            lcn += delta;
            if (lcn >= maxClusters) return false;

            Run r;
            r.lcn = lcn;
            r.clusters = clusters;
            out.push_back(r);
        }

        // A run describes at least one cluster, so a legitimate list can
        // never have more runs than the volume has clusters; combined with
        // the total check this is only reachable by redundant hostile input.
        if (out.size() > (1u << 20)) return false;
    }
    return true;
}

// ------------------------------------------------------------- file records

namespace {

// The fixed part of one attribute header, validated against the record.
struct AttrHeader {
    uint32_t type = 0;
    uint32_t length = 0;
    bool nonResident = false;
    uint8_t nameLen = 0;
    uint32_t valueLen = 0;     // resident only
    uint16_t valueOffset = 0;  // resident only
    uint16_t runOffset = 0;    // non-resident only
    uint64_t startVcn = 0;     // non-resident only
    uint64_t realSize = 0;     // non-resident only
};

// Reads the attribute at `off`, checking every offset and length against the
// record bounds. Returns false on structural corruption; sets `end` when the
// terminator is reached.
bool ReadAttrHeader(ByteCursor& c, size_t off, size_t limit, AttrHeader& a,
                    bool& end) {
    end = false;
    if (!c.Seek(off)) return false;

    a.type = c.U32();
    if (!c.ok()) return false;
    if (a.type == kAttrEnd) {
        end = true;
        return true;
    }

    a.length = c.U32();
    // Minimum resident header is 24 bytes; attributes are 8-aligned. An
    // unaligned or overlong length would make the iteration walk off the
    // record or loop in place.
    if (a.length < 24 || a.length % 8 != 0 || a.length > limit - off) {
        return false;
    }

    a.nonResident = c.U8() != 0;
    a.nameLen = c.U8();
    c.U16();  // name offset
    c.U16();  // flags
    c.U16();  // attribute id

    if (!a.nonResident) {
        a.valueLen = c.U32();
        a.valueOffset = c.U16();
        if (!c.ok()) return false;
        if (a.valueOffset > a.length ||
            static_cast<uint64_t>(a.valueLen) >
                static_cast<uint64_t>(a.length) - a.valueOffset) {
            return false;
        }
    } else {
        a.startVcn = c.U64();
        c.U64();  // end VCN
        a.runOffset = c.U16();
        c.U16();  // compression unit
        c.U32();  // padding
        c.U64();  // allocated size
        a.realSize = c.U64();
        if (!c.ok()) return false;
        if (a.runOffset < 64 || a.runOffset > a.length) return false;
    }
    return c.ok();
}

}  // namespace

bool ParseFileRecord(const uint8_t* data, size_t len, FileRecord& out) {
    out = FileRecord{};
    if (data == nullptr || len < 48) return false;

    ByteCursor c(data, len);
    if (c.U8() != 'F' || c.U8() != 'I' || c.U8() != 'L' || c.U8() != 'E') {
        return false;
    }

    c.Seek(0x14);
    const uint16_t firstAttr = c.U16();
    const uint16_t flags = c.U16();
    const uint32_t bytesInUse = c.U32();
    c.U32();  // bytes allocated
    const uint64_t baseRef = c.U64();
    if (!c.ok()) return false;

    out.inUse = (flags & 0x1) != 0;
    out.isDir = (flags & 0x2) != 0;
    // The upper sixteen bits of a file reference are the sequence number;
    // only the low 48 identify the record.
    out.baseFrs = baseRef & 0x0000FFFFFFFFFFFFull;

    // A record that is unused or belongs to another record carries nothing
    // this caller uses; both are valid parses.
    if (!out.inUse || out.baseFrs != 0) return true;

    const size_t limit = (bytesInUse < len) ? bytesInUse : len;
    if (firstAttr < 0x18 || firstAttr % 8 != 0 || firstAttr >= limit) {
        return false;
    }

    bool sawData = false;
    uint64_t fileNameSize = 0;
    bool nameIsDos = false;

    size_t off = firstAttr;
    for (;;) {
        AttrHeader a;
        bool end = false;
        if (!ReadAttrHeader(c, off, limit, a, end)) return false;
        if (end) break;

        if (a.type == kAttrFileName && !a.nonResident && a.nameLen == 0) {
            ByteCursor v(data + off + a.valueOffset, a.valueLen);
            const uint64_t parentRef = v.U64();
            v.Seek(0x30);
            const uint64_t fnReal = v.U64();
            const uint32_t fnFlags = v.U32();
            v.Seek(0x40);
            const uint8_t nameChars = v.U8();
            const uint8_t nameSpace = v.U8();
            const uint8_t* nameBytes =
                v.Peek(static_cast<size_t>(nameChars) * 2);
            if (v.ok() && nameBytes != nullptr) {
                // Keep the first name seen, upgrading once if that was the
                // DOS 8.3 alias: files carry Win32+DOS pairs and the alias
                // is never the name anyone recognises.
                if (!out.hasName || (nameIsDos && nameSpace != kNameSpaceDos)) {
                    std::wstring name;
                    name.reserve(nameChars);
                    for (size_t i = 0; i < nameChars; ++i) {
                        const uint16_t u = static_cast<uint16_t>(
                            static_cast<uint16_t>(nameBytes[2 * i]) |
                            static_cast<uint16_t>(nameBytes[2 * i + 1]) << 8);
                        // Path separators and NUL cannot appear in a name the
                        // Win32 layer produced, so from a raw record they are
                        // an attack on path assembly. Neutralised here, and
                        // visibly: U+FFFD survives every later sanitiser.
                        const bool hostile = (u == 0) || (u == L'\\') ||
                                             (u == L'/');
                        name.push_back(hostile ? L'\xFFFD'
                                               : static_cast<wchar_t>(u));
                    }
                    out.name = std::move(name);
                    out.hasName = true;
                    out.parentFrs = parentRef & 0x0000FFFFFFFFFFFFull;
                    nameIsDos = (nameSpace == kNameSpaceDos);
                    if ((fnFlags & 0x0400u) != 0) out.isReparse = true;
                    fileNameSize = fnReal;
                }
            }
        } else if (a.type == kAttrData && a.nameLen == 0 && !sawData) {
            // The unnamed stream is the file's content. Alternate (named)
            // streams are not what the map shows, matching what the
            // directory-walk scanner reports.
            out.size = a.nonResident ? a.realSize : a.valueLen;
            sawData = true;
        } else if (a.type == kAttrReparsePoint) {
            out.isReparse = true;
        }

        off += a.length;
    }

    // No $DATA in the base record (a directory, or content pushed to an
    // extension record by attribute-list growth): fall back to the size the
    // $FILE_NAME carries.
    if (!sawData && !out.isDir) out.size = fileNameSize;
    if (out.isDir) out.size = 0;
    return true;
}

bool ExtractDataRuns(const uint8_t* data, size_t len, uint64_t maxClusters,
                     std::vector<Run>& runs, uint64_t& dataSize) {
    runs.clear();
    dataSize = 0;
    if (data == nullptr || len < 48) return false;

    ByteCursor c(data, len);
    if (c.U8() != 'F' || c.U8() != 'I' || c.U8() != 'L' || c.U8() != 'E') {
        return false;
    }
    c.Seek(0x14);
    const uint16_t firstAttr = c.U16();
    c.U16();
    const uint32_t bytesInUse = c.U32();
    if (!c.ok()) return false;

    const size_t limit = (bytesInUse < len) ? bytesInUse : len;
    if (firstAttr < 0x18 || firstAttr % 8 != 0 || firstAttr >= limit) {
        return false;
    }

    size_t off = firstAttr;
    for (;;) {
        AttrHeader a;
        bool end = false;
        if (!ReadAttrHeader(c, off, limit, a, end)) return false;
        if (end) break;

        if (a.type == kAttrData && a.nameLen == 0 && a.nonResident &&
            a.startVcn == 0) {
            if (!DecodeRunList(data + off + a.runOffset,
                               a.length - a.runOffset, maxClusters, runs)) {
                return false;
            }
            dataSize = a.realSize;
            return !runs.empty();
        }
        off += a.length;
    }
    return false;
}

}  // namespace ntfs
}  // namespace spindle
