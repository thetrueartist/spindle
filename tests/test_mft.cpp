// Tests for the tree assembly behind the MFT scan.
//
// mfttree.cpp turns file records into the tree, and it does so while the
// process runs elevated on bytes a hostile volume controls. These feed it
// tables built to be wrong (parents outside the table, cycles, a chain
// deeper than any stack, a table with no root) and, given --image, one
// real volume: an image written by mkntfs and filled through ntfs-3g,
// whose every path and size the tree must reproduce. Run under
// AddressSanitizer, which is what catches the overrun class.
#include "../src/mfttree.h"
#include "../src/ntfs.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "check.h"
#include "ntfs_fixture.h"

using namespace spindle;
using spindle::mft::Assembler;

// core.cpp's CSV writer defers the actual file write to the platform.
// Nothing here exports, so the host build supplies a stub.
namespace spindle {
bool WriteTextFileUtf8(const std::wstring&, const std::wstring&) { return false; }
}  // namespace spindle

namespace {

constexpr uint32_t kRec    = 1024;
constexpr uint32_t kSector = 512;

// A table of records indexed by record number, with the shorthand the
// suites use to lay out a volume.
struct Table {
    std::vector<std::vector<uint8_t>> recs;
    explicit Table(size_t n) : recs(n) {}

    void Root() {
        recs[ntfs::kRootRecord] =
            fixture::MakeRecord(L".", ntfs::kRootRecord, 0, true);
    }
    void Dir(uint32_t i, const std::wstring& name, uint32_t parent) {
        recs[i] = fixture::MakeRecord(name, parent, 0, true);
    }
    void File(uint32_t i, const std::wstring& name, uint32_t parent,
              uint64_t size, uint16_t links = 1) {
        // Anything past a few hundred bytes is non-resident on a real
        // volume, and the fixture only holds small data in the record.
        recs[i] = fixture::MakeRecord(name, parent, size, false, true, 1,
                                      kRec, kSector, size > 700);
        // The link count lives in the header, clear of the fixup tails.
        fixture::Put16(recs[i], 0x12, links);
    }
    // Everything in order, the way the reader hands records over.
    void FeedAll(Assembler& a) {
        for (size_t i = 0; i < recs.size(); ++i) {
            if (!recs[i].empty()) a.Feed(i, recs[i].data(), kRec, kSector);
        }
    }
};

// A node by its backslash path below the root, or null.
const Node* Find(const Node& root, const std::wstring& path) {
    const Node* n = &root;
    size_t at = 0;
    for (;;) {
        const size_t sep = path.find(L'\\', at);
        const std::wstring part =
            path.substr(at, sep == std::wstring::npos ? std::wstring::npos
                                                      : sep - at);
        const Node* next = nullptr;
        for (const Node& c : n->children) {
            if (c.name == part) { next = &c; break; }
        }
        if (!next) return nullptr;
        n = next;
        if (sep == std::wstring::npos) return n;
        at = sep + 1;
    }
}

bool HasChild(const Node& n, const std::wstring& name) {
    for (const Node& c : n.children) {
        if (c.name == name) return true;
    }
    return false;
}

struct Flat {
    std::wstring path;
    bool         dir;
    uint64_t     size;
    bool         hardlink;
};

// Every node below the root as a path, iteratively. Root-level names that
// begin with '$' are the volume's own metadata files, which a mount does
// not list, so they and their subtrees are left out.
std::vector<Flat> Flatten(const Node& root) {
    std::vector<Flat> out;
    struct Frame { const Node* node; std::wstring path; };
    std::vector<Frame> stack;
    for (const Node& c : root.children) {
        if (!c.name.empty() && c.name[0] == L'$') continue;
        stack.push_back(Frame{&c, c.name});
    }
    while (!stack.empty()) {
        Frame f = std::move(stack.back());
        stack.pop_back();
        out.push_back(Flat{f.path, f.node->dir, f.node->size, f.node->hardlink});
        for (const Node& c : f.node->children) {
            stack.push_back(Frame{&c, f.path + L"\\" + c.name});
        }
    }
    return out;
}

uint32_t Deepest(const Node& root) {
    uint32_t deepest = 0;
    struct Frame { const Node* node; uint32_t depth; };
    std::vector<Frame> stack{Frame{&root, 0}};
    while (!stack.empty()) {
        const Frame f = stack.back();
        stack.pop_back();
        deepest = std::max(deepest, f.depth);
        for (const Node& c : f.node->children) {
            stack.push_back(Frame{&c, f.depth + 1});
        }
    }
    return deepest;
}

}  // namespace

SUITE(TestSmallTree, "Assembly: a small tree") {
    Table t(32);
    t.Root();
    t.Dir(16, L"Users", ntfs::kRootRecord);
    t.Dir(17, L"sam", 16);
    t.File(18, L"notes.md", 17, 5000);
    t.File(19, L"pagefile.sys", ntfs::kRootRecord, 1ull << 30);
    t.Dir(20, L"empty", ntfs::kRootRecord);
    t.File(21, L"tiny.txt", 17, 120);

    Assembler a;
    CHECK(a.Begin(32), "a table of 32 records is accepted");
    t.FeedAll(a);
    CHECK(a.Files() == 3, "three files fed");
    CHECK(a.Dirs() == 3, "three directories fed");
    CHECK(a.Bytes() == 5000 + (1ull << 30) + 120, "bytes are the files' sizes");

    ScanResult out;
    CHECK(a.Finish(L"D:\\", out, nullptr), "the tree builds");
    RollUp(out.root);
    CHECK(out.root.name == L"D:\\" && out.root.dir &&
              out.root.cat == Cat::Directory,
          "the root is the volume, named by the caller");

    const Node* notes = Find(out.root, L"Users\\sam\\notes.md");
    CHECK(notes && !notes->dir && notes->size == 5000 && notes->files == 1,
          "a file two levels down carries its size");
    CHECK(notes && notes->cat != Cat::Directory,
          "a file gets its category from its name");
    const Node* sam = Find(out.root, L"Users\\sam");
    CHECK(sam && sam->dir && sam->size == 5120 && sam->files == 2,
          "a directory rolls up its files");
    CHECK(out.root.size == 5000 + (1ull << 30) + 120 && out.root.files == 3,
          "the root rolls up the volume");
    const Node* empty = Find(out.root, L"empty");
    CHECK(empty && empty->dir && empty->children.empty() && empty->size == 0,
          "an empty directory is present and empty");
    CHECK(out.stats.fileCount == 3 && out.stats.dirCount == 3,
          "the stats count the table");
}

SUITE(TestDropped, "Assembly: what never reaches the tree") {
    Table t(64);
    t.Root();
    t.Dir(16, L"keep", ntfs::kRootRecord);
    t.File(17, L"kept.txt", 16, 10);
    // Not in use: a deleted file whose record still holds its name.
    t.recs[18] = fixture::MakeRecord(L"deleted.txt", ntfs::kRootRecord, 10,
                                     false, false);
    // An extension record: its base reference names another record.
    t.recs[19] = fixture::MakeRecord(L"extension.txt", ntfs::kRootRecord, 10,
                                     false);
    fixture::Put64(t.recs[19], 0x20, 16);
    t.File(20, L"dangling.txt", 5000, 10);   // parent past the table
    t.File(21, L"self.txt", 21, 10);         // its own parent
    t.File(22, L"child-of-file.txt", 17, 10);
    t.Dir(23, L"orphan", 40);                // parent record never appears
    t.File(24, L"lost.txt", 23, 10);
    // A name that cannot be a path component is no name at all.
    t.recs[25] = fixture::MakeRecord(L"bad\\name.txt", ntfs::kRootRecord, 10,
                                     false);
    // An 8.3 alias is a poor name, but the only one here.
    t.recs[26] = fixture::MakeRecord(L"ALIAS~1.TXT", ntfs::kRootRecord, 10,
                                     false, true, 2);

    Assembler a;
    CHECK(a.Begin(64), "begin");
    t.FeedAll(a);
    ScanResult out;
    CHECK(a.Finish(L"D:\\", out, nullptr), "the tree builds around the junk");

    CHECK(Find(out.root, L"keep\\kept.txt"), "the ordinary file is there");
    CHECK(!HasChild(out.root, L"deleted.txt"), "a record not in use is dropped");
    CHECK(!HasChild(out.root, L"extension.txt"), "an extension record is dropped");
    CHECK(!HasChild(out.root, L"dangling.txt"), "a parent past the table is dropped");
    CHECK(!HasChild(out.root, L"self.txt"), "a self-parent is dropped");
    const Node* kept = Find(out.root, L"keep\\kept.txt");
    CHECK(kept && kept->children.empty() && !HasChild(out.root, L"child-of-file.txt"),
          "a parent that is a file adopts nothing");
    CHECK(!HasChild(out.root, L"orphan"), "an orphan subtree is dropped whole");
    CHECK(!HasChild(out.root, L"bad\\name.txt") && !HasChild(out.root, L"bad"),
          "an unsafe name never becomes a component");
    CHECK(HasChild(out.root, L"ALIAS~1.TXT"),
          "an 8.3 alias is used when it is the only name");

    // The running totals describe the table, not the tree: a record with a
    // usable name and a parent inside the table is counted whether or not
    // a path from the root ever reaches it.
    CHECK(a.Files() == 5, "files fed: kept, self, child-of-file, lost, alias");
    CHECK(a.Dirs() == 2, "directories fed: keep and the orphan");
    RollUp(out.root);
    CHECK(out.root.files == 2, "files in the tree: kept and the alias");
}

SUITE(TestCyclesAndDepth, "Assembly: cycles and depth") {
    {
        Table t(32);
        t.Root();
        t.File(16, L"real.txt", ntfs::kRootRecord, 1);
        t.Dir(17, L"a", 18);
        t.Dir(18, L"b", 17);
        t.File(19, L"inside.txt", 17, 1);
        t.Dir(20, L"loop", 20);

        Assembler a;
        CHECK(a.Begin(32), "begin");
        t.FeedAll(a);
        ScanResult out;
        CHECK(a.Finish(L"D:\\", out, nullptr),
              "a cycle elsewhere does not stop the build");
        CHECK(HasChild(out.root, L"real.txt") && !HasChild(out.root, L"a") &&
                  !HasChild(out.root, L"b") && !HasChild(out.root, L"loop"),
              "a parent cycle has no path from the root and vanishes whole");
        RollUp(out.root);
        CHECK(out.root.files == 1, "only the reachable file is in the tree");
    }
    {
        // A chain past the depth limit, with a file at the bottom.
        const uint32_t levels = static_cast<uint32_t>(kMaxTreeDepth) + 60;
        Table t(levels + 20);
        t.Root();
        uint32_t parent = ntfs::kRootRecord;
        for (uint32_t i = 0; i < levels; ++i) {
            t.Dir(16 + i, L"d", parent);
            parent = 16 + i;
        }
        t.File(16 + levels, L"bottom.txt", parent, 7);

        Assembler a;
        CHECK(a.Begin(levels + 20), "begin");
        t.FeedAll(a);
        ScanResult out;
        CHECK(a.Finish(L"D:\\", out, nullptr),
              "a chain past the depth limit still builds");
        CHECK(Deepest(out.root) == kMaxTreeDepth,
              "descent stops at kMaxTreeDepth");
        RollUp(out.root);   // iterative too, so this cannot overflow either
        CHECK(out.root.files == 0, "what lies below the limit is not in the tree");
        CHECK(out.stats.dirCount == levels,
              "but the table's own count still says how much there was");
    }
}

SUITE(TestHardlinksAndSizes, "Assembly: hardlinks and saturation") {
    Table t(32);
    t.Root();
    t.File(16, L"linked.dat", ntfs::kRootRecord, 4096, 2);
    t.File(17, L"plain.dat", ntfs::kRootRecord, 100);
    t.File(18, L"huge-a.bin", ntfs::kRootRecord, UINT64_MAX - 1);
    t.File(19, L"huge-b.bin", ntfs::kRootRecord, 5);

    Assembler a;
    CHECK(a.Begin(32), "begin");
    t.FeedAll(a);
    CHECK(a.Bytes() == UINT64_MAX, "bytes saturate rather than wrap");
    ScanResult out;
    CHECK(a.Finish(L"D:\\", out, nullptr), "finish");
    const Node* linked = Find(out.root, L"linked.dat");
    CHECK(linked && linked->hardlink,
          "a record with two names is marked hardlinked");
    const Node* plain = Find(out.root, L"plain.dat");
    CHECK(plain && !plain->hardlink, "a record with one name is not");
    CHECK(out.stats.hardlinkFiles == 1 && out.stats.hardlinkBytes == 4096,
          "hardlinked bytes are totalled separately");
    RollUp(out.root);
    CHECK(out.root.size == UINT64_MAX, "the roll-up saturates too");
}

SUITE(TestRefusals, "Assembly: refusals") {
    {
        Assembler a;
        CHECK(!a.Begin(ntfs::kRootRecord),
              "a table that cannot hold the root record is refused");
        CHECK(!a.Begin(0), "an empty table is refused");
        CHECK(!a.Begin(mft::kMaxRecords + 1), "an absurd record count is refused");
        ScanResult out;
        CHECK(!a.Finish(L"D:\\", out, nullptr), "finishing without a table is refused");
    }
    {
        Assembler a;
        CHECK(a.Begin(32), "begin");
        ScanResult out;
        CHECK(!a.Finish(L"D:\\", out, nullptr), "a table with nothing fed yields no tree");
    }
    {
        Table t(32);   // no root record
        t.Dir(16, L"x", ntfs::kRootRecord);
        t.File(17, L"y", 16, 1);
        Assembler a;
        CHECK(a.Begin(32), "begin");
        t.FeedAll(a);
        ScanResult out;
        CHECK(!a.Finish(L"D:\\", out, nullptr),
              "without a root record nothing can attach, so the walk runs instead");
        CHECK(out.root.children.empty(), "and the result is left empty");
    }
    {
        Table t(32);
        t.Root();
        t.File(16, L"y", ntfs::kRootRecord, 1);
        Assembler a;
        CHECK(a.Begin(32), "begin");
        t.FeedAll(a);
        std::atomic<bool> cancel{true};
        ScanResult out;
        CHECK(!a.Finish(L"D:\\", out, &cancel), "a cancelled build returns nothing");
        CHECK(out.root.children.empty(), "and leaves nothing behind");
    }
    {
        Table t(32);
        t.Root();
        t.File(16, L"y", ntfs::kRootRecord, 1);
        Assembler a;
        CHECK(a.Begin(32), "begin");
        t.FeedAll(a);
        auto beyond = fixture::MakeRecord(L"beyond.txt", ntfs::kRootRecord, 1, false);
        a.Feed(32, beyond.data(), kRec, kSector);
        a.Feed(1ull << 40, beyond.data(), kRec, kSector);
        CHECK(a.Files() == 1, "a record past the table is ignored");
        a.Feed(17, nullptr, kRec, kSector);
        CHECK(a.Files() == 1, "a missing buffer is ignored");
        auto torn = fixture::MakeRecord(L"torn.txt", ntfs::kRootRecord, 1, false);
        torn[510] ^= 1;   // the first sector's check value no longer matches
        a.Feed(18, torn.data(), kRec, kSector);
        CHECK(a.Files() == 1, "a torn record is dropped");
        auto notFile = fixture::MakeRecord(L"z.txt", ntfs::kRootRecord, 1, false);
        std::memcpy(notFile.data(), "BAAD", 4);
        a.Feed(19, notFile.data(), kRec, kSector);
        CHECK(a.Files() == 1, "a record that is not FILE is dropped");
    }
}

SUITE(TestChunks, "Assembly: chunks as the reader hands them over") {
    Table t(24);
    t.Root();
    for (uint32_t i = 16; i < 24; ++i) {
        t.File(i, L"f" + std::to_wstring(i), ntfs::kRootRecord, i);
    }
    Assembler a;
    CHECK(a.Begin(24), "begin");
    a.Feed(ntfs::kRootRecord, t.recs[ntfs::kRootRecord].data(), kRec, kSector);

    // Four records and a partial fifth in one buffer.
    std::vector<uint8_t> chunk;
    for (uint32_t i = 16; i < 20; ++i) {
        chunk.insert(chunk.end(), t.recs[i].begin(), t.recs[i].end());
    }
    chunk.insert(chunk.end(), t.recs[20].begin(), t.recs[20].begin() + 300);
    a.FeedChunk(16, chunk.data(), static_cast<uint32_t>(chunk.size()), kRec,
                kSector);
    CHECK(a.Files() == 4, "whole records are fed; a partial tail is not");

    // A chunk that runs past the end of the table.
    std::vector<uint8_t> past;
    for (uint32_t i = 20; i < 24; ++i) {
        past.insert(past.end(), t.recs[i].begin(), t.recs[i].end());
    }
    for (int extra = 0; extra < 3; ++extra) {
        auto more = fixture::MakeRecord(L"more.txt", ntfs::kRootRecord, 1, false);
        past.insert(past.end(), more.begin(), more.end());
    }
    a.FeedChunk(20, past.data(), static_cast<uint32_t>(past.size()), kRec,
                kSector);
    CHECK(a.Files() == 8, "records past the table's end are ignored");
    a.FeedChunk(16, nullptr, 4096, kRec, kSector);
    a.FeedChunk(16, past.data(), 4096, 0, kSector);
    CHECK(a.Files() == 8, "a missing buffer or a zero record size feeds nothing");

    ScanResult out;
    CHECK(a.Finish(L"D:\\", out, nullptr), "finish");
    RollUp(out.root);
    CHECK(out.root.files == 8, "all eight files are in the tree");
}

// The image written by tools/make-ntfs-image.sh, named with --image. Its
// manifest lists what was written; the tree assembled from its table must
// match it entry for entry.
SUITE(TestRealImage, "A real NTFS image") {
    const std::string image = spindle::testing::Option("image");
    if (image.empty()) {
        std::printf("    skipped: no --image given (make test-image builds one)\n");
        return;
    }
    FILE* f = std::fopen(image.c_str(), "rb");
    CHECK(f != nullptr, "the image opens");
    if (!f) return;
    auto readAt = [&](uint64_t off, std::vector<uint8_t>& buf, size_t n) {
        buf.resize(n);
        if (std::fseek(f, static_cast<long>(off), SEEK_SET) != 0) return false;
        return std::fread(buf.data(), 1, n, f) == n;
    };

    std::vector<uint8_t> boot;
    CHECK(readAt(0, boot, 512), "the boot sector reads");
    const ntfs::BootInfo bi = ntfs::ParseBootSector(boot.data(), boot.size());
    CHECK(bi.valid, "mkntfs wrote a boot sector the parser accepts");
    CHECK(bi.bytesPerSector == 512 && bi.bytesPerCluster == 4096 &&
              bi.bytesPerRecord == 1024,
          "sector, cluster and record sizes are the ones the image was made with");
    if (!bi.valid) { std::fclose(f); return; }

    std::vector<uint8_t> rec0;
    CHECK(readAt(bi.mftStartCluster * bi.bytesPerCluster, rec0, bi.bytesPerRecord),
          "record 0 reads");
    CHECK(ntfs::ApplyFixups(rec0.data(), rec0.size(), bi.bytesPerSector),
          "record 0's fixups hold");
    std::vector<ntfs::DataRun> runs;
    uint64_t mftBytes = 0;
    CHECK(ntfs::ParseMftDataRuns(rec0.data(), rec0.size(), runs, mftBytes) &&
              !runs.empty(),
          "record 0 says where the table lives");
    const uint64_t recordCount = mftBytes / bi.bytesPerRecord;
    std::printf("    table: %llu records in %zu run(s)\n",
                static_cast<unsigned long long>(recordCount), runs.size());

    Assembler a;
    CHECK(a.Begin(recordCount), "the table's size is accepted");
    uint64_t rec = 0;
    std::vector<uint8_t> buf;
    bool readsOk = true;
    for (const ntfs::DataRun& run : runs) {
        const uint64_t runBytes = run.clusters * bi.bytesPerCluster;
        if (run.sparse) { rec += runBytes / bi.bytesPerRecord; continue; }
        uint64_t off = static_cast<uint64_t>(run.lcn) * bi.bytesPerCluster;
        uint64_t remaining = runBytes;
        while (remaining > 0 && rec < recordCount) {
            const uint32_t want = static_cast<uint32_t>(
                std::min<uint64_t>(remaining, 1u << 20));
            const uint32_t aligned = want - want % bi.bytesPerRecord;
            if (aligned == 0) break;
            if (!readAt(off, buf, aligned)) { readsOk = false; break; }
            a.FeedChunk(rec, buf.data(), aligned, bi.bytesPerRecord,
                        bi.bytesPerSector);
            off += aligned;
            remaining -= aligned;
            rec += aligned / bi.bytesPerRecord;
        }
    }
    std::fclose(f);
    CHECK(readsOk, "every run of the table reads");

    ScanResult out;
    CHECK(a.Finish(L"X:\\", out, nullptr), "the tree builds from a real table");
    RollUp(out.root);

    // The manifest: type, size, inode, path; the inode is the record
    // number, so hard links share one.
    struct Expect { bool dir; uint64_t size; uint64_t inode; };
    std::map<std::wstring, Expect> manifest;
    std::map<uint64_t, std::vector<std::wstring>> byInode;
    {
        FILE* m = std::fopen((image + ".manifest").c_str(), "rb");
        CHECK(m != nullptr, "the manifest opens");
        if (!m) return;
        char line[4096];
        while (std::fgets(line, sizeof line, m)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            const size_t t1 = s.find('\t'), t2 = s.find('\t', t1 + 1),
                         t3 = s.find('\t', t2 + 1);
            if (t1 == std::string::npos || t2 == std::string::npos ||
                t3 == std::string::npos) continue;
            Expect e;
            e.dir   = s[0] == 'd';
            e.size  = std::strtoull(s.c_str() + t1 + 1, nullptr, 10);
            e.inode = std::strtoull(s.c_str() + t2 + 1, nullptr, 10);
            std::wstring path = Utf8ToWide(s.substr(t3 + 1));
            for (wchar_t& c : path) if (c == L'/') c = L'\\';
            manifest[path] = e;
            if (!e.dir) byInode[e.inode].push_back(path);
        }
        std::fclose(m);
    }
    CHECK(manifest.size() > 600, "the manifest is the tree the script wrote");

    std::map<std::wstring, Flat> tree;
    for (Flat& fl : Flatten(out.root)) tree[fl.path] = fl;

    size_t dirsOk = 0, filesOk = 0, extra = 0, missingDirs = 0;
    for (const auto& kv : manifest) {
        if (!kv.second.dir) continue;
        const auto it = tree.find(kv.first);
        if (it != tree.end() && it->second.dir) ++dirsOk; else ++missingDirs;
    }
    CHECK(missingDirs == 0, "every directory written is in the tree, as a directory");

    size_t groupsOk = 0, groupsWrong = 0;
    uint64_t expectedBytes = 0;
    for (const auto& kv : byInode) {
        // One record, one node: exactly one of a hard-linked group's paths
        // appears, and it is marked as shared.
        size_t present = 0;
        bool sizeOk = true, flagOk = true;
        for (const std::wstring& p : kv.second) {
            const auto it = tree.find(p);
            if (it == tree.end() || it->second.dir) continue;
            ++present;
            sizeOk = sizeOk && it->second.size == manifest[p].size;
            flagOk = flagOk && it->second.hardlink == (kv.second.size() > 1);
        }
        expectedBytes += manifest[kv.second.front()].size;
        if (present == 1 && sizeOk && flagOk) { ++groupsOk; ++filesOk; }
        else ++groupsWrong;
    }
    CHECK(groupsWrong == 0,
          "every file is in the tree once, with its size, and a hard link is marked");

    for (const auto& kv : tree) {
        if (manifest.find(kv.first) == manifest.end()) ++extra;
    }
    CHECK(extra == 0, "nothing is in the tree that was not written");

    // The root also holds the volume's own metadata files ($MFT, $LogFile
    // and the rest), which a mount hides, so the totals are compared over
    // the entries a mount shows and the root is only required to hold at
    // least that much.
    size_t treeFiles = 0;
    uint64_t treeBytes = 0;
    for (const auto& kv : tree) {
        if (kv.second.dir) continue;
        ++treeFiles;
        treeBytes += kv.second.size;
    }
    CHECK(treeFiles == byInode.size(),
          "the tree holds one file per record with data");
    CHECK(treeBytes == expectedBytes,
          "and the bytes written, counting a hard link once");
    CHECK(out.root.files >= treeFiles && out.root.size >= treeBytes,
          "the root's roll-up covers them and the metadata files besides");
    std::printf("    %zu directories, %zu files, %zu hard-link group(s) matched; "
                "%llu bytes\n",
                dirsOk, filesOk,
                std::count_if(byInode.begin(), byInode.end(),
                              [](const auto& g) { return g.second.size() > 1; }),
                static_cast<unsigned long long>(expectedBytes));
}

int main(int argc, char** argv) {
    return spindle::testing::Main("Spindle MFT assembly tests", argc, argv);
}
