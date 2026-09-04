// Spindle - builders for on-disk NTFS structures, shared by the parser
// test and the tree-assembly test. Each writes a well-formed structure the
// way mkntfs would; the tests then corrupt what they need to.
#pragma once

#include "../src/ntfs.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace fixture {

using namespace spindle::ntfs;

// Written through at(), which refuses an offset past the buffer rather
// than trusting the caller, and which keeps the optimiser from guessing
// that the buffer might be empty.
inline void Put16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
    b.at(off) = static_cast<uint8_t>(v & 0xFF);
    b.at(off + 1) = static_cast<uint8_t>(v >> 8);
}
inline void Put32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b.at(off + static_cast<size_t>(i)) =
            static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
    }
}
inline void Put64(std::vector<uint8_t>& b, size_t off, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        b.at(off + static_cast<size_t>(i)) =
            static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
    }
}

// --- builders -------------------------------------------------------------

inline std::vector<uint8_t> MakeBootSector(uint16_t bps = 512, uint8_t spc = 8,
                                    uint8_t cpr = 246 /* 2^10 = 1024 */,
                                    uint64_t mftLcn = 786432,
                                    uint64_t totalSectors = 100000000) {
    std::vector<uint8_t> b(512, 0);
    b[0] = 0xEB; b[1] = 0x52; b[2] = 0x90;
    std::memcpy(&b[3], "NTFS    ", 8);
    Put16(b, 0x0B, bps);
    b[0x0D] = spc;
    Put64(b, 0x28, totalSectors);
    Put64(b, 0x30, mftLcn);
    b[0x40] = cpr;
    Put16(b, 510, 0xAA55);
    return b;
}

// Builds a file record with a $FILE_NAME and a resident or non-resident
// $DATA, then installs a valid update sequence array.
inline std::vector<uint8_t> MakeRecord(const std::wstring& name, uint32_t parent,
                                uint64_t size, bool isDir, bool inUse = true,
                                uint8_t nameSpace = 1,
                                uint32_t recSize = 1024,
                                uint32_t bps = 512,
                                bool nonResidentData = false) {
    std::vector<uint8_t> b(recSize, 0);
    std::memcpy(&b[0], "FILE", 4);

    const uint16_t usaOff = 0x30;
    const uint16_t usaCount = static_cast<uint16_t>(recSize / bps + 1);
    Put16(b, 0x04, usaOff);
    Put16(b, 0x06, usaCount);
    Put16(b, 0x10, 1);                        // sequence number
    Put16(b, 0x12, 1);                        // hard link count
    const uint16_t attrOff = 0x38;
    Put16(b, 0x14, attrOff);
    Put16(b, 0x16, static_cast<uint16_t>((inUse ? 1 : 0) | (isDir ? 2 : 0)));
    Put32(b, 0x1C, recSize);
    Put64(b, 0x20, 0);                        // base record = 0 (not extension)

    size_t off = attrOff;

    // ---- $FILE_NAME (resident)
    const uint32_t nameChars = static_cast<uint32_t>(name.size());
    const uint32_t fnValueLen = 0x42 + nameChars * 2;
    uint32_t fnAttrLen = 0x18 + fnValueLen;
    fnAttrLen = (fnAttrLen + 7) & ~7u;

    Put32(b, off + 0x00, kAttrFileName);
    Put32(b, off + 0x04, fnAttrLen);
    b[off + 0x08] = 0;                        // resident
    b[off + 0x09] = 0;                        // no attribute name
    Put32(b, off + 0x10, fnValueLen);
    Put16(b, off + 0x14, 0x18);               // value offset

    const size_t fnBase = off + 0x18;
    Put64(b, fnBase + 0x00, parent);
    Put64(b, fnBase + 0x30, size);
    b[fnBase + 0x40] = static_cast<uint8_t>(nameChars);
    b[fnBase + 0x41] = nameSpace;
    for (uint32_t i = 0; i < nameChars; ++i) {
        Put16(b, fnBase + 0x42 + i * 2, static_cast<uint16_t>(name[i]));
    }
    off += fnAttrLen;

    // ---- $DATA
    if (nonResidentData) {
        const uint32_t attrLen = 0x48;
        Put32(b, off + 0x00, kAttrData);
        Put32(b, off + 0x04, attrLen);
        b[off + 0x08] = 1;                    // non-resident
        b[off + 0x09] = 0;
        Put16(b, off + 0x20, 0x40);           // run list offset
        Put64(b, off + 0x28, size);           // allocated
        Put64(b, off + 0x30, size);           // real size
        Put64(b, off + 0x38, size);           // initialised
        off += attrLen;
    } else {
        const uint32_t valueLen = static_cast<uint32_t>(size & 0xFFFF);
        uint32_t attrLen = 0x18 + valueLen;
        attrLen = (attrLen + 7) & ~7u;
        if (off + attrLen + 8 < recSize) {
            Put32(b, off + 0x00, kAttrData);
            Put32(b, off + 0x04, attrLen);
            b[off + 0x08] = 0;
            b[off + 0x09] = 0;
            Put32(b, off + 0x10, valueLen);
            Put16(b, off + 0x14, 0x18);
            off += attrLen;
        }
    }

    Put32(b, off + 0, kAttrEnd);
    Put32(b, 0x18, static_cast<uint32_t>(off + 8));   // used size

    // ---- update sequence array: stamp the check value into each sector tail
    const uint16_t check = 0x1234;
    Put16(b, usaOff, check);
    for (size_t i = 0; i < static_cast<size_t>(usaCount) - 1; ++i) {
        const size_t tail = (i + 1) * bps - 2;
        // Save the real bytes into the array, then write the check value.
        b[usaOff + 2 + i * 2]     = b[tail];
        b[usaOff + 2 + i * 2 + 1] = b[tail + 1];
        Put16(b, tail, check);
    }
    return b;
}

// Record 0 as mkntfs writes it: named "$MFT", with a non-resident $DATA
// whose run list puts the table at `lcn` for `clusters` clusters and whose
// real size is `bytes`. MakeRecord leaves eight bytes of run list space in
// its non-resident $DATA, enough for one run with three-byte fields, and
// the fixup tails are nowhere near it, so the run is written afterwards.
inline std::vector<uint8_t> MakeMftRecord(uint64_t bytes, uint32_t lcn,
                                          uint32_t clusters,
                                          uint32_t recSize = 1024,
                                          uint32_t bps = 512) {
    std::vector<uint8_t> b =
        MakeRecord(L"$MFT", kRootRecord, bytes, false, true, 3, recSize, bps,
                   true);
    size_t off = b[0x14] | (static_cast<size_t>(b[0x15]) << 8);
    for (int guard = 0; guard < 16; ++guard) {
        const uint32_t type = static_cast<uint32_t>(b[off]) |
                              (static_cast<uint32_t>(b[off + 1]) << 8) |
                              (static_cast<uint32_t>(b[off + 2]) << 16) |
                              (static_cast<uint32_t>(b[off + 3]) << 24);
        if (type == kAttrEnd) break;
        const uint32_t len = static_cast<uint32_t>(b[off + 4]) |
                             (static_cast<uint32_t>(b[off + 5]) << 8);
        if (type == kAttrData) {
            const size_t run = off + 0x40;     // MakeRecord's run list offset
            b[run]     = 0x33;                 // three-byte length, three-byte offset
            b[run + 1] = static_cast<uint8_t>(clusters & 0xFF);
            b[run + 2] = static_cast<uint8_t>((clusters >> 8) & 0xFF);
            b[run + 3] = static_cast<uint8_t>((clusters >> 16) & 0xFF);
            b[run + 4] = static_cast<uint8_t>(lcn & 0xFF);
            b[run + 5] = static_cast<uint8_t>((lcn >> 8) & 0xFF);
            b[run + 6] = static_cast<uint8_t>((lcn >> 16) & 0xFF);
            b[run + 7] = 0;                    // end of the run list
            break;
        }
        off += len;
    }
    return b;
}

}  // namespace fixture
