// Spindle - benchmarks for the portable hot paths.
//
// Builds a synthetic volume (a million files by default, in tens of
// thousands of folders, with realistic names and log-distributed sizes)
// and times what the window does with such a tree: the roll-up, the
// treemap layout, hit testing, the side panels, search, the cache round
// trip, the comparison, the duplicate candidates, and the tree assembled
// from a Master File Table of matching size. Each figure is the median of
// several runs on an optimised build, so `make bench` before and after a
// change says whether it was worth making.
//
//   make bench                       a million files, five runs each
//   make bench ARGS='--files 200000 --runs 3'
#include "../src/spindle.h"
#include "../src/mfttree.h"
#include "../src/ntfs.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "ntfs_fixture.h"

using namespace spindle;

// core.cpp's CSV writer defers the actual file write to the platform.
namespace spindle {
bool WriteTextFileUtf8(const std::wstring&, const std::wstring&) { return false; }
}  // namespace spindle

namespace {

struct Timer {
    using Clock = std::chrono::steady_clock;
    Clock::time_point t0 = Clock::now();
    double Ms() const {
        return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    }
};

template <class F>
double Median(int runs, F&& f) {
    std::vector<double> t;
    for (int i = 0; i < runs; ++i) {
        Timer tm;
        f();
        t.push_back(tm.Ms());
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

void Row(const char* what, double ms, const std::string& note = std::string()) {
    std::printf("  %-30s %9.2f ms  %s\n", what, ms, note.c_str());
}

std::string Thousands(uint64_t v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

const wchar_t* const kExts[] = {
    L"dll", L"exe", L"txt", L"jpg", L"png", L"mp4", L"mkv", L"pak", L"bin",
    L"zip", L"7z",  L"iso", L"vmdk", L"cpp", L"h",  L"js",  L"json", L"xml",
    L"log", L"db",  L"sqlite", L"pdf", L"docx", L"flac", L"mp3", L"dat", L"",
    L"tmp", L"cab", L"msi", L"nef", L"heic", L"pst", L"sav", L"bk2", L"wav"};

// A deterministic volume. Folders fan out widely near the top and narrow
// with depth, the way a real drive does; every file gets one of the
// extensions above and a size drawn log-uniformly from a hundred bytes to
// a few gigabytes, which is skewed small the way real files are.
struct Generator {
    std::mt19937_64 rng{20260904};
    uint64_t files = 0, dirs = 0, bytes = 0;
    std::vector<std::wstring> names;   // every file name, for CategoryForFile

    uint64_t Size() {
        const double e = std::uniform_real_distribution<double>(2.0, 9.5)(rng);
        return static_cast<uint64_t>(std::pow(10.0, e));
    }

    void Build(Node& root, uint64_t targetFiles) {
        struct Pending { Node* node; int depth; };
        std::vector<Pending> queue{Pending{&root, 0}};
        size_t head = 0;
        while (head < queue.size() && files < targetFiles) {
            const Pending p = queue[head++];
            Node& d = *p.node;
            // About one folder per ten files, which is what a real drive
            // has once the leaf folders are counted.
            const int subdirs = p.depth >= 7 ? 0
                              : p.depth == 0 ? 64
                              : static_cast<int>(1 + rng() % 5);
            const int nfiles = static_cast<int>(4 + rng() % 60);
            d.children.reserve(static_cast<size_t>(subdirs + nfiles));
            for (int i = 0; i < nfiles && files < targetFiles; ++i) {
                const wchar_t* ext = kExts[rng() % (sizeof kExts / sizeof kExts[0])];
                std::wstring name = L"file-" + std::to_wstring(files);
                if (*ext) name += std::wstring(L".") + ext;
                Node f(name, false);
                f.size  = Size();
                f.files = 1;
                f.cat   = CategoryForFile(f.name);
                bytes += f.size;
                names.push_back(f.name);
                d.children.push_back(std::move(f));
                ++files;
            }
            for (int i = 0; i < subdirs; ++i) {
                Node sub(L"folder-" + std::to_wstring(dirs), true);
                sub.cat = Cat::Directory;
                d.children.push_back(std::move(sub));
                ++dirs;
            }
            // Children are stable now: the vector was reserved for the lot.
            for (size_t i = static_cast<size_t>(nfiles); i < d.children.size(); ++i) {
                queue.push_back(Pending{&d.children[i], p.depth + 1});
            }
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    uint64_t targetFiles = 1000000;
    int runs = 5;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--files" && i + 1 < argc) targetFiles = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--runs" && i + 1 < argc) runs = std::atoi(argv[++i]);
    }

    ScanResult res;
    res.root = Node(L"D:\\", true);
    res.root.cat = Cat::Directory;
    Generator gen;
    {
        Timer t;
        gen.Build(res.root, targetFiles);
        std::printf("\n=== Spindle core benchmarks: %s files, %s folders, built in %.0f ms ===\n\n",
                    Thousands(gen.files).c_str(), Thousands(gen.dirs).c_str(), t.Ms());
    }

    Row("roll-up", Median(runs, [&] { RollUp(res.root); }));
    res.stats.fileCount = gen.files;
    res.stats.dirCount  = gen.dirs;
    res.stats.bytes     = res.root.size;

    Row("category by name", Median(runs, [&] {
        uint32_t acc = 0;
        for (const std::wstring& n : gen.names) acc += static_cast<uint32_t>(CategoryForFile(n));
        if (acc == 0xFFFFFFFFu) std::puts("");
    }), Thousands(gen.names.size()) + " names");

    std::vector<Cell> cells;
    const Rect bounds{0, 0, 1000, 700};
    const float minArea = std::max(6.0f, bounds.w * bounds.h / 42000.0f);
    {
        const double ms = Median(runs, [&] {
            BuildTreemap(res.root, bounds, 5, minArea, cells);
        });
        Row("treemap layout", ms, Thousands(cells.size()) + " cells");
    }

    {
        std::mt19937 rng(7);
        std::vector<float> xs, ys;
        for (int i = 0; i < 20000; ++i) {
            xs.push_back(static_cast<float>(rng() % 1000));
            ys.push_back(static_cast<float>(rng() % 700));
        }
        const double ms = Median(runs, [&] {
            int acc = 0;
            for (size_t i = 0; i < xs.size(); ++i) acc += HitTestIndex(cells, xs[i], ys[i]);
            if (acc == -12345678) std::puts("");
        });
        Row("hit test", ms, "20,000 points");
    }

    Row("kinds panel", Median(runs, [&] {
        const auto stats = ExtensionBreakdown(res.root, 40);
        if (stats.empty()) std::puts("");
    }));
    Row("largest panel", Median(runs, [&] {
        const auto hits = LargestFiles(res.root, 100);
        if (hits.empty()) std::puts("");
    }));
    {
        const Query q = ParseQuery(L"kind:media >100mb -tmp");
        Row("find (query)", Median(runs, [&] {
            const auto hits = FindMatching(res.root, q, 500);
            if (hits.empty()) std::puts("");
        }));
    }
    Row("find (name)", Median(runs, [&] {
        const auto hits = FindByName(res.root, L"file-77", 500);
        if (hits.empty()) std::puts("");
    }));

    std::vector<uint8_t> cache;
    CacheMeta meta;
    meta.savedUnixMs  = 1756742400000ull;
    meta.volumeSerial = 0x1234ABCD;
    {
        const double ms = Median(runs, [&] {
            cache.clear();
            SerializeScan(res, meta, cache);
        });
        Row("cache: serialise", ms, Thousands(cache.size() >> 10) + " KB");
    }
    Row("cache: parse", Median(runs, [&] {
        ScanResult back;
        CacheMeta m;
        if (!DeserializeScan(cache.data(), cache.size(), back, m, nullptr)) std::puts("  parse failed");
    }));

    {
        ScanResult later = res;   // deep copy
        std::mt19937_64 rng(99);
        struct Frame { Node* node; };
        std::vector<Frame> stack{Frame{&later.root}};
        while (!stack.empty()) {
            Node* n = stack.back().node;
            stack.pop_back();
            for (Node& c : n->children) {
                if (c.dir) stack.push_back(Frame{&c});
                else if (rng() % 50 == 0) c.size += 5u << 20;
            }
        }
        RollUp(later.root);
        Row("compare with cache", Median(runs, [&] {
            const DiffReport d = DiffTrees(res.root, later.root, 1u << 20);
            if (d.changes.empty()) std::puts("");
        }));
    }

    Row("duplicate candidates", Median(runs, [&] {
        auto c = DuplicateCandidates(res.root, 1u << 20);
        c = FilterBySharedSize(std::move(c));
        if (c.empty()) std::puts("");
    }));

    // The MFT assembly for a table of the same shape: one record per node,
    // in the order the reader hands them over.
    {
        const size_t records = static_cast<size_t>(std::min<uint64_t>(gen.files + gen.dirs, 400000)) + 16;
        std::vector<uint8_t> table;
        table.reserve(records * 1024);
        {
            auto root = fixture::MakeRecord(L".", ntfs::kRootRecord, 0, true);
            std::vector<uint8_t> blank(1024, 0);
            for (size_t i = 0; i < 16; ++i) {
                const auto& r = (i == ntfs::kRootRecord) ? root : blank;
                table.insert(table.end(), r.begin(), r.end());
            }
            std::mt19937_64 rng(3);
            size_t made = 16;
            // Every 40th record is a folder whose parent is an earlier folder.
            std::vector<uint32_t> folders{ntfs::kRootRecord};
            while (made < records) {
                const uint32_t parent = folders[rng() % folders.size()];
                if (made % 40 == 0) {
                    auto r = fixture::MakeRecord(L"folder-" + std::to_wstring(made), parent, 0, true);
                    table.insert(table.end(), r.begin(), r.end());
                    folders.push_back(static_cast<uint32_t>(made));
                } else {
                    const wchar_t* ext = kExts[rng() % (sizeof kExts / sizeof kExts[0])];
                    std::wstring name = L"file-" + std::to_wstring(made);
                    if (*ext) name += std::wstring(L".") + ext;
                    const uint64_t size = gen.Size();
                    auto r = fixture::MakeRecord(name, parent, size, false, true, 1, 1024, 512, size > 700);
                    table.insert(table.end(), r.begin(), r.end());
                }
                ++made;
            }
        }
        std::vector<uint8_t> work;
        ScanResult out;
        double feedMs = 0, finishMs = 0;
        const double total = Median(runs, [&] {
            work = table;   // fixups patch in place, so feed a fresh copy
            mft::Assembler a;
            a.Begin(records);
            Timer t;
            for (size_t off = 0; off < work.size(); off += 8u << 20) {
                const size_t n = std::min<size_t>(8u << 20, work.size() - off);
                a.FeedChunk(off / 1024, work.data() + off, static_cast<uint32_t>(n), 1024, 512);
            }
            feedMs = t.Ms();
            Timer t2;
            out = ScanResult();
            a.Finish(L"D:\\", out, nullptr);
            RollUp(out.root);
            finishMs = t2.Ms();
        });
        Row("mft: parse records", feedMs, Thousands(records) + " records");
        Row("mft: build tree", finishMs);
        Row("mft: total", total, Thousands(out.root.files) + " files in the tree");
    }
    std::printf("\n");
    return 0;
}
