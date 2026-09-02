// Tests for the NTFS parser.
//
// This code reads raw on-disk structures while running elevated, so it is the
// highest-value target in the whole program. The tests build well-formed
// structures to prove it parses, then attack it with malformed ones to prove
// it does not read past its buffer. Run under AddressSanitizer, which is what
// actually catches the second class.

#include "../src/ntfs.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace spindle::ntfs;

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { ++g_pass; }                                             \
        else { ++g_fail;                                                    \
               std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); }\
    } while (0)

namespace {

void Put16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
    b[off] = static_cast<uint8_t>(v & 0xFF);
    b[off + 1] = static_cast<uint8_t>(v >> 8);
}
void Put32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b[off + static_cast<size_t>(i)] =
            static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
    }
}
void Put64(std::vector<uint8_t>& b, size_t off, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        b[off + static_cast<size_t>(i)] =
            static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
    }
}

// --- builders -------------------------------------------------------------

std::vector<uint8_t> MakeBootSector(uint16_t bps = 512, uint8_t spc = 8,
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
std::vector<uint8_t> MakeRecord(const std::wstring& name, uint32_t parent,
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

}  // namespace

// ------------------------------------------------------------------- tests

static void TestBootSectorValid() {
    std::printf("Boot sector: valid input\n");
    const auto b = MakeBootSector();
    const BootInfo bi = ParseBootSector(b.data(), b.size());

    CHECK(bi.valid, "accepts a well-formed boot sector");
    CHECK(bi.bytesPerSector == 512, "sector size");
    CHECK(bi.bytesPerCluster == 4096, "cluster size = 512 * 8");
    CHECK(bi.bytesPerRecord == 1024, "record size from 2^(256-246)");
    CHECK(bi.mftStartCluster == 786432, "MFT start cluster");
    std::printf("    bps=%u cluster=%u record=%u mftLcn=%llu\n",
                bi.bytesPerSector, bi.bytesPerCluster, bi.bytesPerRecord,
                static_cast<unsigned long long>(bi.mftStartCluster));
}

static void TestBootSectorRejects() {
    std::printf("Boot sector: malformed input rejected\n");

    CHECK(!ParseBootSector(nullptr, 0).valid, "null buffer");
    CHECK(!ParseBootSector(nullptr, 512).valid, "null with length");

    auto b = MakeBootSector();
    CHECK(!ParseBootSector(b.data(), 100).valid, "truncated buffer");
    CHECK(!ParseBootSector(b.data(), 0).valid, "zero length");

    b = MakeBootSector();
    std::memcpy(&b[3], "FAT32   ", 8);
    CHECK(!ParseBootSector(b.data(), b.size()).valid, "wrong OEM id");

    b = MakeBootSector(); Put16(b, 0x0B, 0);
    CHECK(!ParseBootSector(b.data(), b.size()).valid, "zero sector size");

    b = MakeBootSector(); Put16(b, 0x0B, 513);
    CHECK(!ParseBootSector(b.data(), b.size()).valid, "non-power-of-two sector");

    b = MakeBootSector(); Put16(b, 0x0B, 8192);
    CHECK(!ParseBootSector(b.data(), b.size()).valid, "absurd sector size");

    b = MakeBootSector(); b[0x0D] = 0;
    CHECK(!ParseBootSector(b.data(), b.size()).valid, "zero sectors/cluster");

    b = MakeBootSector(); b[0x0D] = 7;
    CHECK(!ParseBootSector(b.data(), b.size()).valid, "non-power-of-two spc");

    // 2^(256-129) sectors per cluster: an unchecked shift here is UB.
    b = MakeBootSector(); b[0x0D] = 129;
    CHECK(!ParseBootSector(b.data(), b.size()).valid, "shift overflow via spc");

    b = MakeBootSector(); b[0x40] = 0;
    CHECK(!ParseBootSector(b.data(), b.size()).valid, "zero clusters/record");

    b = MakeBootSector(); b[0x40] = 200;   // 2^56 bytes per record
    CHECK(!ParseBootSector(b.data(), b.size()).valid, "shift overflow via cpr");

    b = MakeBootSector(); b[0x40] = 255;   // 2^1 = 2 bytes
    CHECK(!ParseBootSector(b.data(), b.size()).valid, "record too small");

    b = MakeBootSector(); Put64(b, 0x30, 0);
    CHECK(!ParseBootSector(b.data(), b.size()).valid, "MFT at cluster zero");

    b = MakeBootSector(); Put64(b, 0x30, 0xFFFFFFFFFFFFFFFFull);
    CHECK(!ParseBootSector(b.data(), b.size()).valid,
          "MFT start beyond volume");

    b = MakeBootSector(512, 8, 246, 999999999ull, 1000);
    CHECK(!ParseBootSector(b.data(), b.size()).valid,
          "MFT start past a small volume");
}

static void TestRunList() {
    std::printf("Run list decoding\n");

    // header 0x21: 1 length byte, 2 offset bytes.
    const uint8_t simple[] = {0x21, 0x18, 0x34, 0x56, 0x00};
    std::vector<DataRun> runs;
    CHECK(ParseRunList(simple, sizeof(simple), runs), "simple list parses");
    CHECK(runs.size() == 1, "one run");
    if (runs.size() == 1) {
        CHECK(runs[0].clusters == 0x18, "run length");
        CHECK(runs[0].lcn == 0x5634, "run lcn");
    }

    // Two runs, the second at a negative delta from the first.
    const uint8_t twoRuns[] = {0x21, 0x10, 0x00, 0x10,
                               0x21, 0x20, 0x00, 0xF0,
                               0x00};
    CHECK(ParseRunList(twoRuns, sizeof(twoRuns), runs), "two runs parse");
    CHECK(runs.size() == 2, "two runs");
    if (runs.size() == 2) {
        CHECK(runs[0].lcn == 0x1000, "first lcn");
        CHECK(runs[1].lcn == 0x1000 - 0x1000, "negative delta applied");
    }

    // Sparse run: zero offset bytes.
    const uint8_t sparse[] = {0x01, 0x08, 0x00};
    CHECK(ParseRunList(sparse, sizeof(sparse), runs), "sparse parses");
    CHECK(runs.size() == 1 && runs[0].sparse, "sparse flagged");

    std::printf("Run list: malformed input rejected\n");

    CHECK(!ParseRunList(nullptr, 0, runs), "null buffer");

    // Claims 8 length bytes and 8 offset bytes but supplies none.
    const uint8_t truncated[] = {0x88};
    CHECK(!ParseRunList(truncated, sizeof(truncated), runs),
          "header promising more than the buffer holds");

    const uint8_t truncated2[] = {0x21, 0x18};
    CHECK(!ParseRunList(truncated2, sizeof(truncated2), runs),
          "offset bytes missing");

    const uint8_t zeroLen[] = {0x20, 0x00, 0x00, 0x00};
    CHECK(!ParseRunList(zeroLen, sizeof(zeroLen), runs),
          "zero-width length field");

    const uint8_t zeroRun[] = {0x21, 0x00, 0x00, 0x10, 0x00};
    CHECK(!ParseRunList(zeroRun, sizeof(zeroRun), runs), "zero-cluster run");

    // Large negative delta that would drive the lcn below zero.
    const uint8_t negative[] = {0x11, 0x08, 0x80, 0x00};
    CHECK(!ParseRunList(negative, sizeof(negative), runs),
          "delta before volume start");
}

static void TestFixups() {
    std::printf("Fixup application\n");

    auto rec = MakeRecord(L"test.txt", 5, 4096, false);
    std::vector<uint8_t> copy = rec;
    CHECK(ApplyFixups(copy.data(), copy.size(), 512), "valid fixups apply");

    std::printf("Fixup: malformed input rejected\n");
    CHECK(!ApplyFixups(nullptr, 1024, 512), "null record");

    copy = rec;
    CHECK(!ApplyFixups(copy.data(), 1000, 512), "length not a sector multiple");
    CHECK(!ApplyFixups(copy.data(), copy.size(), 0), "zero sector size");
    CHECK(!ApplyFixups(copy.data(), 10, 512), "record shorter than a header");

    // Array claims more sectors than the record contains.
    copy = rec; Put16(copy, 0x06, 99);
    CHECK(!ApplyFixups(copy.data(), copy.size(), 512), "sector count mismatch");

    copy = rec; Put16(copy, 0x06, 1);
    CHECK(!ApplyFixups(copy.data(), copy.size(), 512), "array too small");

    // Array offset pointing into the header, and past the record.
    copy = rec; Put16(copy, 0x04, 0x10);
    CHECK(!ApplyFixups(copy.data(), copy.size(), 512), "array overlaps header");

    copy = rec; Put16(copy, 0x04, 60000);
    CHECK(!ApplyFixups(copy.data(), copy.size(), 512), "array past record end");

    // Check value that does not match a sector tail: a torn write.
    copy = rec; copy[512 - 2] = 0xFF; copy[512 - 1] = 0xFF;
    CHECK(!ApplyFixups(copy.data(), copy.size(), 512), "torn write detected");
}

static void TestRecordParsing() {
    std::printf("Record parsing: valid input\n");

    auto rec = MakeRecord(L"KingdomCome.pak", 42, 512, false);
    CHECK(ApplyFixups(rec.data(), rec.size(), 512), "fixups ok");
    RecordInfo info = ParseRecord(rec.data(), rec.size());

    CHECK(info.valid, "record parses");
    CHECK(info.inUse, "in-use flag");
    CHECK(!info.isDir, "not a directory");
    CHECK(info.hasName, "name found");
    CHECK(info.name == L"KingdomCome.pak", "name matches");
    CHECK(info.parent == 42, "parent reference");
    CHECK(info.size == 512, "resident $DATA size");
    CHECK(!info.isExtension, "not an extension record");
    // parent is 64-bit now (the full $FILE_NAME reference); print it as one.
    std::printf("    parsed '%ls' parent=%llu size=%llu\n", info.name.c_str(),
                static_cast<unsigned long long>(info.parent),
                static_cast<unsigned long long>(info.size));

    rec = MakeRecord(L"Users", 5, 0, true);
    ApplyFixups(rec.data(), rec.size(), 512);
    info = ParseRecord(rec.data(), rec.size());
    CHECK(info.isDir, "directory flag");
    CHECK(info.name == L"Users", "directory name");

    // Non-resident $DATA carries the real size at 0x30.
    rec = MakeRecord(L"big.vmdk", 7, 8ull << 30, false, true, 1, 1024, 512, true);
    ApplyFixups(rec.data(), rec.size(), 512);
    info = ParseRecord(rec.data(), rec.size());
    CHECK(info.size == (8ull << 30), "non-resident size read from real size");
    std::printf("    non-resident size %llu\n",
                static_cast<unsigned long long>(info.size));

    // A deleted record still parses but reports itself unused.
    rec = MakeRecord(L"gone.txt", 5, 100, false, false);
    ApplyFixups(rec.data(), rec.size(), 512);
    info = ParseRecord(rec.data(), rec.size());
    CHECK(info.valid && !info.inUse, "deleted record flagged not in use");

    // A DOS 8.3 alias must lose to nothing; with only a DOS name it is used.
    rec = MakeRecord(L"PROGRA~1", 5, 0, true, true, 2);
    ApplyFixups(rec.data(), rec.size(), 512);
    info = ParseRecord(rec.data(), rec.size());
    CHECK(info.hasName && info.name == L"PROGRA~1",
          "DOS name used when it is the only one");
}

static void TestRecordRejects() {
    std::printf("Record parsing: malformed input rejected\n");

    RecordInfo info = ParseRecord(nullptr, 1024);
    CHECK(!info.valid, "null record");

    auto rec = MakeRecord(L"test.txt", 5, 100, false);
    ApplyFixups(rec.data(), rec.size(), 512);

    info = ParseRecord(rec.data(), 10);
    CHECK(!info.valid, "record shorter than its header");

    auto bad = rec;
    std::memcpy(&bad[0], "BAAD", 4);
    info = ParseRecord(bad.data(), bad.size());
    CHECK(!info.valid, "wrong magic");

    // Attribute offset inside the header, and past the record.
    bad = rec; Put16(bad, 0x14, 4);
    info = ParseRecord(bad.data(), bad.size());
    CHECK(!info.valid, "attribute offset inside header");

    bad = rec; Put16(bad, 0x14, 60000);
    info = ParseRecord(bad.data(), bad.size());
    CHECK(!info.valid, "attribute offset past record");

    // Zero-length attribute: an unguarded parser loops here forever.
    bad = rec; Put32(bad, 0x38 + 4, 0);
    info = ParseRecord(bad.data(), bad.size());
    CHECK(info.valid && !info.hasName, "zero-length attribute stops the walk");

    // Attribute length longer than the record.
    bad = rec; Put32(bad, 0x38 + 4, 0xFFFFFFF8u);
    info = ParseRecord(bad.data(), bad.size());
    CHECK(info.valid && !info.hasName, "over-long attribute rejected");

    // Unaligned attribute length.
    bad = rec; Put32(bad, 0x38 + 4, 17);
    info = ParseRecord(bad.data(), bad.size());
    CHECK(info.valid && !info.hasName, "unaligned attribute length rejected");

    // Name length claiming far more characters than the attribute holds.
    bad = rec; bad[0x38 + 0x18 + 0x40] = 255;
    info = ParseRecord(bad.data(), bad.size());
    CHECK(info.valid, "over-long name does not crash");
    CHECK(!info.hasName || info.name.size() <= kMaxNameChars,
          "name bounded by the attribute");

    // Value offset pointing outside the attribute.
    bad = rec; Put16(bad, 0x38 + 0x14, 60000);
    info = ParseRecord(bad.data(), bad.size());
    CHECK(info.valid && !info.hasName, "value offset outside attribute");

    // Used-size larger than the buffer must not extend the walk.
    bad = rec; Put32(bad, 0x18, 0xFFFFFFFFu);
    info = ParseRecord(bad.data(), bad.size());
    CHECK(info.valid, "over-large used size handled");

    // An extension record is recognised so its attributes are not double
    // counted against the base record.
    bad = rec; Put64(bad, 0x20, 12345);
    info = ParseRecord(bad.data(), bad.size());
    CHECK(info.isExtension, "extension record flagged");
}

// Structured fuzzing: take a valid record and corrupt it, which reaches deeper
// into the parser than uniformly random bytes usually do.
static void FuzzRecords() {
    std::printf("Fuzz: record parsing\n");

    std::mt19937 rng(0xF0FA);
    std::uniform_int_distribution<int> byteDist(0, 255);
    std::uniform_int_distribution<size_t> posDist(0, 1023);
    std::uniform_int_distribution<int> countDist(1, 40);

    const auto base = MakeRecord(L"Windows 10 (Malware)-cl1-s002.vmdk", 5,
                                 4000000000ull, false);

    size_t parsed = 0, named = 0;
    for (int iter = 0; iter < 60000; ++iter) {
        std::vector<uint8_t> rec = base;
        const int flips = countDist(rng);
        for (int i = 0; i < flips; ++i) {
            rec[posDist(rng)] = static_cast<uint8_t>(byteDist(rng));
        }
        // Fixups may legitimately fail on a corrupted record; parse anyway,
        // because on a real volume a torn record still reaches the parser.
        ApplyFixups(rec.data(), rec.size(), 512);
        const RecordInfo info = ParseRecord(rec.data(), rec.size());
        if (info.valid) ++parsed;
        if (info.hasName) {
            ++named;
            if (info.name.size() > kMaxNameChars) {
                std::printf("    name exceeded the NTFS limit: %zu chars\n",
                            info.name.size());
                ++g_fail;
                return;
            }
        }
    }
    std::printf("    60000 corrupted records: %zu parsed, %zu yielded a name, "
                "no overruns\n", parsed, named);
    CHECK(true, "fuzzing completed without a fault");
}

static void FuzzRandomBuffers() {
    std::printf("Fuzz: uniformly random buffers\n");

    std::mt19937 rng(0x5A17);
    std::uniform_int_distribution<int> byteDist(0, 255);
    std::uniform_int_distribution<size_t> lenDist(0, 4096);

    for (int iter = 0; iter < 40000; ++iter) {
        const size_t len = lenDist(rng);
        std::vector<uint8_t> buf(len);
        for (auto& b : buf) b = static_cast<uint8_t>(byteDist(rng));

        // A "FILE" magic on a fifth of the buffers, so the record path is
        // actually reached rather than rejected at the first check.
        if (len >= 4 && (iter % 5) == 0) std::memcpy(buf.data(), "FILE", 4);

        ParseBootSector(buf.data(), len);
        if (len > 0) {
            std::vector<DataRun> runs;
            ParseRunList(buf.data(), len, runs);
        }
        if (len >= 512 && len % 512 == 0) {
            std::vector<uint8_t> copy = buf;
            ApplyFixups(copy.data(), copy.size(), 512);
        }
        ParseRecord(buf.data(), len);
    }
    std::printf("    40000 random buffers across all four entry points\n");
    CHECK(true, "random fuzzing completed without a fault");
}

// Truncation is its own class of bug: a structure that is valid up to the
// point the buffer ends.
static void FuzzTruncation() {
    std::printf("Fuzz: truncation at every offset\n");

    const auto boot = MakeBootSector();
    for (size_t n = 0; n <= boot.size(); ++n) {
        ParseBootSector(boot.data(), n);
    }

    auto rec = MakeRecord(L"Textures-part0.pak", 99, 2000000000ull, false);
    ApplyFixups(rec.data(), rec.size(), 512);
    for (size_t n = 0; n <= rec.size(); ++n) {
        const RecordInfo info = ParseRecord(rec.data(), n);
        if (info.hasName && info.name.size() > kMaxNameChars) {
            ++g_fail;
            std::printf("    truncation at %zu produced an over-long name\n", n);
            return;
        }
    }

    const uint8_t runs[] = {0x21, 0x18, 0x34, 0x56, 0x31, 0x10, 0x00,
                            0x20, 0x00, 0x00};
    for (size_t n = 0; n <= sizeof(runs); ++n) {
        std::vector<DataRun> out;
        ParseRunList(runs, n, out);
    }
    std::printf("    every prefix of a boot sector, record and run list\n");
    CHECK(true, "truncation fuzzing completed without a fault");
}

int main() {
    std::printf("\n=== Spindle NTFS parser tests ===\n\n");

    TestBootSectorValid();
    TestBootSectorRejects();
    TestRunList();
    TestFixups();
    TestRecordParsing();
    TestRecordRejects();
    FuzzRecords();
    FuzzRandomBuffers();
    FuzzTruncation();

    std::printf("\n=== %d passed, %d failed ===\n\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
