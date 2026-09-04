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

#include "check.h"

#include "ntfs_fixture.h"

using namespace fixture;

// ------------------------------------------------------------------- tests

SUITE(TestBootSectorValid, "Boot sector: valid input") {
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

SUITE(TestBootSectorRejects, "Boot sector: malformed input rejected") {

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

SUITE(TestRunList, "Run list decoding") {

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

SUITE(TestFixups, "Fixup application") {

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

SUITE(TestRecordParsing, "Record parsing: valid input") {

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

SUITE(TestRecordRejects, "Record parsing: malformed input rejected") {

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
SUITE(FuzzRecords, "Fuzz: record parsing") {

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
                CHECK(false, "a corrupted record yielded a name past the NTFS limit");
                return;
            }
        }
    }
    std::printf("    60000 corrupted records: %zu parsed, %zu yielded a name, "
                "no overruns\n", parsed, named);
    CHECK(true, "fuzzing completed without a fault");
}

SUITE(FuzzRandomBuffers, "Fuzz: uniformly random buffers") {

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
SUITE(FuzzTruncation, "Fuzz: truncation at every offset") {

    const auto boot = MakeBootSector();
    for (size_t n = 0; n <= boot.size(); ++n) {
        ParseBootSector(boot.data(), n);
    }

    auto rec = MakeRecord(L"Textures-part0.pak", 99, 2000000000ull, false);
    ApplyFixups(rec.data(), rec.size(), 512);
    for (size_t n = 0; n <= rec.size(); ++n) {
        const RecordInfo info = ParseRecord(rec.data(), n);
        if (info.hasName && info.name.size() > kMaxNameChars) {
            std::printf("    truncation at %zu produced an over-long name\n", n);
            CHECK(false, "a truncated record yielded a name past the NTFS limit");
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

SUITE(TestMftDataRuns, "The $MFT record: where the table lives") {
    auto rec = MakeMftRecord(64u << 20, 786432, 16384);
    CHECK(ApplyFixups(rec.data(), rec.size(), 512), "the fixture's fixups hold");
    std::vector<DataRun> runs;
    uint64_t bytes = 0;
    CHECK(ParseMftDataRuns(rec.data(), rec.size(), runs, bytes),
          "a well-formed $MFT record yields its run list");
    CHECK(bytes == (64u << 20), "and the table's real size");
    CHECK(runs.size() == 1 && runs[0].lcn == 786432 &&
              runs[0].clusters == 16384 && !runs[0].sparse,
          "one run, at the cluster the boot sector names");

    auto big = MakeMftRecord(kMaxMftBytes + 1, 786432, 16384);
    ApplyFixups(big.data(), big.size(), 512);
    CHECK(!ParseMftDataRuns(big.data(), big.size(), runs, bytes),
          "a table past kMaxMftBytes is refused");
    auto zero = MakeMftRecord(0, 786432, 16384);
    ApplyFixups(zero.data(), zero.size(), 512);
    CHECK(!ParseMftDataRuns(zero.data(), zero.size(), runs, bytes),
          "a zero-length table is refused");
    auto resident = MakeRecord(L"$MFT", kRootRecord, 100, false);
    ApplyFixups(resident.data(), resident.size(), 512);
    CHECK(!ParseMftDataRuns(resident.data(), resident.size(), runs, bytes),
          "a record without a non-resident $DATA is refused");
    auto notFile = rec;
    std::memcpy(notFile.data(), "BAAD", 4);
    CHECK(!ParseMftDataRuns(notFile.data(), notFile.size(), runs, bytes),
          "a buffer that is not a file record is refused");
    CHECK(!ParseMftDataRuns(rec.data(), 40, runs, bytes),
          "a truncated record is refused");
    CHECK(!ParseMftDataRuns(nullptr, 0, runs, bytes), "no buffer, no runs");

    // Corrupted copies must never read outside the record. ASan is the
    // judge; the checks are that nothing absurd comes back either.
    std::mt19937 rng(0xABCDEF);
    std::uniform_int_distribution<size_t> posDist(0, rec.size() - 1);
    std::uniform_int_distribution<int> byteDist(0, 255);
    size_t accepted = 0, absurd = 0;
    for (int iter = 0; iter < 20000; ++iter) {
        std::vector<uint8_t> r = rec;
        const int flips = 1 + iter % 24;
        for (int i = 0; i < flips; ++i) {
            r[posDist(rng)] = static_cast<uint8_t>(byteDist(rng));
        }
        if (ParseMftDataRuns(r.data(), r.size(), runs, bytes)) {
            ++accepted;
            if (bytes == 0 || bytes > kMaxMftBytes || runs.size() > kMaxRuns) {
                ++absurd;
            }
        }
    }
    CHECK(absurd == 0, "whatever is accepted stays within the bounds");
    std::printf("    20000 corrupted $MFT records: %zu still parsed, no overruns\n",
                accepted);
}

int main(int argc, char** argv) {
    return spindle::testing::Main("Spindle NTFS parser tests", argc, argv);
}
