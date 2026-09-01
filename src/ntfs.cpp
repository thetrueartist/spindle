// Spindle - NTFS on-disk structure parsing.
// See ntfs.h for the security posture. Short version: none of this input is
// trusted, and every read is bounds-checked against the supplied buffer.

#include "ntfs.h"

#include <climits>
#include <cstring>

namespace spindle::ntfs {
namespace {

// A cursor that cannot read past its buffer. Every field access in this file
// goes through it, so an out-of-range offset yields zero rather than a read
// of whatever happens to follow in memory.
class Reader {
public:
    Reader(const uint8_t* data, size_t len) : d_(data), n_(len) {}

    bool Has(size_t off, size_t bytes) const {
        return d_ != nullptr && off <= n_ && bytes <= n_ - off;
    }

    uint8_t U8(size_t off) const {
        return Has(off, 1) ? d_[off] : uint8_t{0};
    }

    uint16_t U16(size_t off) const {
        if (!Has(off, 2)) return 0;
        return static_cast<uint16_t>(d_[off] |
                                     (static_cast<uint16_t>(d_[off + 1]) << 8));
    }

    uint32_t U32(size_t off) const {
        if (!Has(off, 4)) return 0;
        return static_cast<uint32_t>(d_[off]) |
               (static_cast<uint32_t>(d_[off + 1]) << 8) |
               (static_cast<uint32_t>(d_[off + 2]) << 16) |
               (static_cast<uint32_t>(d_[off + 3]) << 24);
    }

    uint64_t U64(size_t off) const {
        if (!Has(off, 8)) return 0;
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) {
            v = (v << 8) | static_cast<uint64_t>(d_[off + static_cast<size_t>(i)]);
        }
        return v;
    }

    const uint8_t* Ptr(size_t off) const { return d_ + off; }
    size_t Size() const { return n_; }

private:
    const uint8_t* d_ = nullptr;
    size_t         n_ = 0;
};

bool IsPowerOfTwo(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

// A name off the disk becomes a path component, and a path component is
// later joined with separators and handed to the Win32 file APIs. NTFS's
// POSIX namespace permits characters Win32 does not - a backslash, a colon,
// a NUL - so a crafted volume could otherwise name a file "..\..\Windows"
// and have the join produce a path pointing somewhere else entirely.
// Rejected here, at the point the bytes arrive, rather than anywhere later.
bool IsSafeName(const std::wstring& name) {
    if (name.empty()) return false;
    if (name == L"." || name == L"..") return false;
    for (wchar_t c : name) {
        if (c < 0x20) return false;               // NUL and other controls
        if (c == L'\\' || c == L'/' || c == L':') return false;
        if (c == L'*' || c == L'?' || c == L'"') return false;
        if (c == L'<' || c == L'>' || c == L'|') return false;
    }
    // Win32 silently trims these from a component, so a name ending in one
    // resolves to a different object than the one enumerated.
    if (name.back() == L' ' || name.back() == L'.') return false;
    return true;
}

}  // namespace

// ------------------------------------------------------------- boot sector

BootInfo ParseBootSector(const uint8_t* data, size_t len) {
    BootInfo bi;
    const Reader r(data, len);
    if (!r.Has(0, 512)) return bi;

    // OEM identifier. Not a security control -- anyone can write these eight
    // bytes -- but it rejects the common case of pointing at a non-NTFS
    // volume before any of the numeric fields are interpreted.
    if (std::memcmp(r.Ptr(3), "NTFS    ", 8) != 0) return bi;

    const uint32_t bps = r.U16(0x0B);
    if (!IsPowerOfTwo(bps) || bps < 256 || bps > kMaxBytesPerSector) return bi;

    const uint8_t spc = r.U8(0x0D);
    uint32_t clusterSize = 0;
    if (spc == 0) {
        return bi;
    } else if (spc <= 0x80) {
        if (!IsPowerOfTwo(spc)) return bi;
        clusterSize = bps * spc;
    } else {
        // Values above 0x80 encode 2^(256-spc) sectors per cluster. Bound the
        // shift: an unchecked value here shifts by more than the width of the
        // type, which is undefined behaviour.
        const uint32_t shift = 256u - spc;
        if (shift > 24) return bi;
        clusterSize = bps << shift;
    }
    if (clusterSize == 0 || clusterSize > kMaxClusterSize) return bi;

    // Record size uses the same dual encoding: a positive value is clusters
    // per record, a value above 0x80 is 2^(256-v) bytes.
    const uint8_t cpr = r.U8(0x40);
    uint32_t recordSize = 0;
    if (cpr == 0) {
        return bi;
    } else if (cpr <= 0x80) {
        recordSize = clusterSize * cpr;
    } else {
        const uint32_t shift = 256u - cpr;
        if (shift > 20) return bi;
        recordSize = 1u << shift;
    }
    if (recordSize < kMinRecordSize || recordSize > kMaxRecordSize) return bi;
    if (!IsPowerOfTwo(recordSize)) return bi;

    // A record must be a whole number of sectors or the fixup arithmetic in
    // ApplyFixups cannot describe it.
    if (recordSize % bps != 0) return bi;

    const uint64_t mftLcn = r.U64(0x30);
    const uint64_t total  = r.U64(0x28);
    if (mftLcn == 0) return bi;

    // The MFT cannot start beyond the end of the volume. Checked because the
    // caller turns this into a byte offset for a seek.
    if (total != 0) {
        const uint64_t clustersTotal = total / (clusterSize / bps);
        if (mftLcn >= clustersTotal) return bi;
    }
    // Guard the multiplication the caller will perform.
    if (mftLcn > (UINT64_MAX / clusterSize)) return bi;

    bi.bytesPerSector  = bps;
    bi.bytesPerCluster = clusterSize;
    bi.bytesPerRecord  = recordSize;
    bi.mftStartCluster = mftLcn;
    bi.totalSectors    = total;
    bi.valid           = true;
    return bi;
}

// --------------------------------------------------------------- run lists

bool ParseRunList(const uint8_t* data, size_t len, std::vector<DataRun>& out) {
    out.clear();
    const Reader r(data, len);
    if (data == nullptr) return false;

    size_t pos = 0;
    int64_t lcn = 0;   // run offsets are deltas against the previous run

    while (pos < len) {
        const uint8_t header = r.U8(pos);
        if (header == 0) break;   // end of list

        const size_t lenBytes = header & 0x0Fu;
        const size_t offBytes = (header >> 4) & 0x0Fu;

        // A zero length field describes a run of unknown size: malformed.
        if (lenBytes == 0 || lenBytes > 8 || offBytes > 8) return false;
        if (!r.Has(pos + 1, lenBytes + offBytes)) return false;

        uint64_t runLen = 0;
        for (size_t i = 0; i < lenBytes; ++i) {
            runLen |= static_cast<uint64_t>(r.U8(pos + 1 + i)) << (i * 8);
        }
        if (runLen == 0) return false;

        DataRun run;
        run.clusters = runLen;

        if (offBytes == 0) {
            // Sparse run: no cluster is allocated.
            run.sparse = true;
            run.lcn = 0;
        } else {
            // Assemble unsigned, then sign-extend from the encoded width.
            // Doing this in a signed type shifts a 1 into, and past, the sign
            // bit -- undefined behaviour that a crafted run list reaches with
            // an 8-byte offset field.
            uint64_t raw = 0;
            for (size_t i = 0; i < offBytes; ++i) {
                raw |= static_cast<uint64_t>(r.U8(pos + 1 + lenBytes + i))
                       << (i * 8);
            }

            if (offBytes < 8) {
                const unsigned bits = static_cast<unsigned>(offBytes) * 8u;
                const uint64_t signBit = uint64_t{1} << (bits - 1);
                if (raw & signBit) {
                    // Set every bit above the encoded width.
                    raw |= ~((uint64_t{1} << bits) - 1);
                }
            }
            // Width 8 needs no extension: the value already fills the type.
            const int64_t delta = static_cast<int64_t>(raw);

            // Checked addition. The running LCN is signed and a hostile run
            // list can drive it past either bound, which is undefined
            // behaviour before it is ever a bad seek.
            if (delta > 0 && lcn > INT64_MAX - delta) return false;
            if (delta < 0 && lcn < INT64_MIN - delta) return false;
            lcn += delta;

            if (lcn < 0) return false;   // would seek before the volume start
            run.lcn = lcn;
        }

        out.push_back(run);
        if (out.size() > kMaxRuns) return false;

        pos += 1 + lenBytes + offBytes;
    }
    return true;
}

// ------------------------------------------------------------------ fixups

bool ApplyFixups(uint8_t* record, size_t len, uint32_t bytesPerSector) {
    if (record == nullptr || bytesPerSector < 256) return false;
    if (len < 48 || len % bytesPerSector != 0) return false;

    const Reader r(record, len);
    const uint16_t usaOffset = r.U16(0x04);
    const uint16_t usaCount  = r.U16(0x06);

    // usaCount counts the check value plus one entry per sector.
    if (usaCount < 2) return false;
    const size_t sectors = static_cast<size_t>(usaCount) - 1;
    if (sectors != len / bytesPerSector) return false;

    // The array must lie inside the record and must not overlap the header.
    if (usaOffset < 0x30) return false;
    const size_t usaBytes = static_cast<size_t>(usaCount) * 2;
    if (!r.Has(usaOffset, usaBytes)) return false;

    const uint16_t check = r.U16(usaOffset);

    for (size_t i = 0; i < sectors; ++i) {
        const size_t tail = (i + 1) * bytesPerSector - 2;
        if (!r.Has(tail, 2)) return false;

        const uint16_t present = r.U16(tail);
        // A mismatch means the sector was not written as part of this record:
        // a torn write, or a record that is not what the header claims.
        if (present != check) return false;

        const size_t src = static_cast<size_t>(usaOffset) + 2 + i * 2;
        record[tail]     = record[src];
        record[tail + 1] = record[src + 1];
    }
    return true;
}

// ------------------------------------------------------------ file records

RecordInfo ParseRecord(const uint8_t* record, size_t len) {
    RecordInfo info;
    const Reader r(record, len);
    if (!r.Has(0, 48)) return info;

    if (std::memcmp(r.Ptr(0), "FILE", 4) != 0) return info;

    const uint16_t flags = r.U16(0x16);
    info.inUse = (flags & 0x0001) != 0;
    info.isDir = (flags & 0x0002) != 0;
    info.links = r.U16(0x12);

    // A non-zero base reference means this record continues another one. Its
    // attributes belong to the base record, not to a file of its own.
    info.isExtension = (r.U64(0x20) & 0x0000FFFFFFFFFFFFull) != 0;

    const uint32_t usedSize = r.U32(0x18);
    // The header's own idea of how much of the record is live must fit inside
    // the record. Attribute walking is bounded by the smaller of the two.
    const size_t limit =
        (usedSize >= 48 && usedSize <= len) ? usedSize : len;

    size_t off = r.U16(0x14);
    if (off < 48 || off >= limit) return info;

    // Best name found so far. NTFS may store several: a Win32 name, a DOS 8.3
    // alias, or both. The 8.3 alias is preferred by nothing and shown by
    // nobody, so it only wins if there is no alternative.
    int bestNamespace = -1;
    bool sawData = false;

    for (size_t guard = 0; guard < kMaxAttrsPerRec; ++guard) {
        if (!r.Has(off, 4)) break;
        const uint32_t type = r.U32(off);
        if (type == kAttrEnd) break;
        if (!r.Has(off, 16)) break;

        const uint32_t attrLen = r.U32(off + 4);
        // Zero or unaligned lengths would loop forever or step into the
        // middle of a structure; an over-long one walks off the record.
        if (attrLen < 16 || (attrLen % 8) != 0) break;
        if (attrLen > limit - off) break;

        const uint8_t nonResident = r.U8(off + 8);

        if (type == kAttrFileName && nonResident == 0) {
            const uint32_t valueLen = r.U32(off + 0x10);
            const uint16_t valueOff = r.U16(off + 0x14);
            const size_t base = off + valueOff;

            // 0x42 is the fixed part of $FILE_NAME before the name itself.
            if (valueLen >= 0x42 && valueOff < attrLen &&
                valueLen <= attrLen - valueOff && r.Has(base, 0x42)) {
                const uint32_t nameChars = r.U8(base + 0x40);
                const uint8_t  nameSpace = r.U8(base + 0x41);
                const size_t   nameBytes = static_cast<size_t>(nameChars) * 2;

                if (nameChars > 0 && nameChars <= kMaxNameChars &&
                    r.Has(base + 0x42, nameBytes) &&
                    nameBytes <= valueLen - 0x42) {
                    // Namespace: 0 POSIX, 1 Win32, 2 DOS, 3 Win32+DOS.
                    // Rank so that anything beats a bare DOS alias.
                    const int rank = (nameSpace == 2) ? 0
                                   : (nameSpace == 0) ? 1
                                   : 2;
                    if (rank > bestNamespace) {
                        std::wstring candidate;
                        candidate.reserve(nameChars);
                        for (size_t i = 0; i < nameChars; ++i) {
                            candidate.push_back(static_cast<wchar_t>(
                                r.U16(base + size_t{0x42} + i * 2)));
                        }
                        // A name that cannot be a path component is not
                        // used, and does not displace a safe one.
                        if (!IsSafeName(candidate)) {
                            off += attrLen;
                            if (off >= limit) break;
                            continue;
                        }

                        bestNamespace = rank;
                        info.parent =
                            r.U64(base) & 0x0000FFFFFFFFFFFFull;

                        info.name.clear();
                        info.name.reserve(nameChars);
                        for (size_t i = 0; i < nameChars; ++i) {
                            // Index arithmetic done in size_t. Computing
                            // i * 2 in uint32_t and widening afterwards is
                            // how an offset overflows before it is checked;
                            // nameChars is bounded above, but the parser
                            // should not depend on a bound to be safe.
                            info.name.push_back(static_cast<wchar_t>(
                                r.U16(base + size_t{0x42} + i * 2)));
                        }
                        info.hasName = true;
                    }
                }
            }
        } else if (type == kAttrData && !sawData) {
            // Only the unnamed $DATA stream counts as the file's size. Named
            // streams are separate content that Explorer does not show as
            // part of the file.
            const uint8_t nameLen = r.U8(off + 9);
            if (nameLen == 0) {
                if (nonResident == 0) {
                    info.size = r.U32(off + 0x10);
                } else if (r.Has(off + 0x30, 8)) {
                    // Real (logical) size, matching what the directory-walk
                    // scanner reports, so the two paths agree.
                    info.size = r.U64(off + 0x30);
                }
                sawData = true;
            }
        }

        off += attrLen;
        if (off >= limit) break;
    }

    info.valid = true;
    return info;
}

}  // namespace spindle::ntfs
