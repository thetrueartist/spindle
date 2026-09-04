// Fuzz target: everything that reads NTFS structures off a disk, and the
// tree assembled from them. The first byte picks the parser; the rest is
// the input. Every byte is attacker-controlled on a real volume, and this
// code runs elevated, so nothing here may read outside its buffer (the
// sanitizers judge that) or hand back a value past its own bounds (the
// FUZZ_REQUIREs judge that).
#include "fuzz_common.h"
#include "../src/mfttree.h"
#include "../src/ntfs.h"
#include "ntfs_fixture.h"

#include <cstring>

using namespace spindle;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    const uint8_t which = data[0] % 5;
    const uint8_t* d = data + 1;
    const size_t n = size - 1;
    switch (which) {
        case 0: {
            const ntfs::BootInfo bi = ntfs::ParseBootSector(d, n);
            if (bi.valid) {
                FUZZ_REQUIRE(bi.bytesPerRecord >= ntfs::kMinRecordSize &&
                             bi.bytesPerRecord <= ntfs::kMaxRecordSize);
                FUZZ_REQUIRE(bi.bytesPerSector > 0 &&
                             bi.bytesPerSector <= ntfs::kMaxBytesPerSector);
                FUZZ_REQUIRE(bi.bytesPerCluster > 0 &&
                             bi.bytesPerCluster <= ntfs::kMaxClusterSize);
            }
            break;
        }
        case 1: {
            std::vector<ntfs::DataRun> runs;
            ntfs::ParseRunList(d, n, runs);
            FUZZ_REQUIRE(runs.size() <= ntfs::kMaxRuns);
            break;
        }
        case 2: {
            std::vector<uint8_t> rec(d, d + n);
            ntfs::ApplyFixups(rec.data(), rec.size(), 512);
            const ntfs::RecordInfo info = ntfs::ParseRecord(rec.data(), rec.size());
            if (info.hasName) {
                FUZZ_REQUIRE(!info.name.empty() &&
                             info.name.size() <= ntfs::kMaxNameChars);
                FUZZ_REQUIRE(info.parent <= 0x0000FFFFFFFFFFFFull);
            }
            break;
        }
        case 3: {
            std::vector<ntfs::DataRun> runs;
            uint64_t bytes = 0;
            if (ntfs::ParseMftDataRuns(d, n, runs, bytes)) {
                FUZZ_REQUIRE(bytes > 0 && bytes <= ntfs::kMaxMftBytes);
                FUZZ_REQUIRE(runs.size() <= ntfs::kMaxRuns);
            }
            break;
        }
        case 4: {
            // The input is a table of 1 KB records, fed after a real root.
            const size_t recs = n / 1024;
            if (recs == 0 || recs > 256) break;
            const uint64_t count = recs + 8;
            mft::Assembler a;
            if (!a.Begin(count)) break;
            std::vector<uint8_t> root =
                fixture::MakeRecord(L".", ntfs::kRootRecord, 0, true);
            a.Feed(ntfs::kRootRecord, root.data(), 1024, 512);
            std::vector<uint8_t> table(d, d + recs * 1024);
            a.FeedChunk(8, table.data(), static_cast<uint32_t>(table.size()),
                        1024, 512);
            ScanResult out;
            if (a.Finish(L"F:\\", out, nullptr)) {
                RollUp(out.root);
                FUZZ_REQUIRE(out.root.files <= a.Files());
            }
            break;
        }
    }
    return 0;
}

void FuzzSeeds(std::vector<std::vector<uint8_t>>& out) {
    auto add = [&](uint8_t which, const std::vector<uint8_t>& body) {
        std::vector<uint8_t> s{which};
        s.insert(s.end(), body.begin(), body.end());
        out.push_back(s);
    };
    add(0, fixture::MakeBootSector());
    // Two runs: eight clusters at 0xC0000, then sixteen a little further on.
    add(1, {0x31, 0x08, 0x00, 0x00, 0x0C, 0x21, 0x10, 0x00, 0x10, 0x00});
    add(2, fixture::MakeRecord(L"notes.md", ntfs::kRootRecord, 5000, false,
                               true, 1, 1024, 512, true));
    add(2, fixture::MakeRecord(L"Program Files", ntfs::kRootRecord, 0, true));
    add(3, fixture::MakeMftRecord(64u << 20, 786432, 16384));
    std::vector<uint8_t> table;
    const auto dir  = fixture::MakeRecord(L"Users", ntfs::kRootRecord, 0, true);
    const auto file = fixture::MakeRecord(L"pagefile.sys", 8, 1ull << 30,
                                          false, true, 1, 1024, 512, true);
    const auto tiny = fixture::MakeRecord(L"desktop.ini", 8, 120, false);
    table.insert(table.end(), dir.begin(), dir.end());
    table.insert(table.end(), file.begin(), file.end());
    table.insert(table.end(), tiny.begin(), tiny.end());
    add(4, table);
}
