// Tests for Spindle's platform-independent core.
// Builds and runs on the host; no Windows headers involved.

#include "../src/spindle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <cstring>
#include <string>
#include <vector>

using namespace spindle;

// core.cpp's CSV writer defers the actual file write to the platform. The
// Windows build supplies a UTF-8 writer; the host build writes UTF-8 here so
// the CSV formatting itself stays under test.
namespace spindle {
bool WriteTextFileUtf8(const std::wstring& path, const std::wstring& text) {
    FILE* f = std::fopen(std::string(path.begin(), path.end()).c_str(), "wb");
    if (!f) return false;
    // Same contract as the Windows implementation: UTF-8 with a BOM.
    std::string out("\xEF\xBB\xBF");
    for (wchar_t c : text) {
        const uint32_t u = static_cast<uint32_t>(c);
        if (u < 0x80) {
            out.push_back(static_cast<char>(u));
        } else if (u < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (u >> 6)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (u >> 12)));
            out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }
    const size_t n = std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return n == out.size();
}
}  // namespace spindle

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { ++g_pass; }                                             \
        else { ++g_fail;                                                    \
               std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); }\
    } while (0)

static std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) {
        s += (c < 128 && c > 31) ? static_cast<char>(c) : '?';
    }
    return s;
}

// ---------------------------------------------------------------- tree build

static Node MakeFile(const std::wstring& name, uint64_t size) {
    Node n(name, false);
    n.size = size;
    n.files = 1;
    n.cat = CategoryForFile(name);
    return n;
}

static Node MakeDir(const wchar_t* name, std::vector<Node> kids) {
    Node n(name, true);
    n.cat = Cat::Directory;
    for (auto& k : kids) {
        n.size = SatAdd(n.size, k.size);
        n.files += k.files;
        n.children.push_back(std::move(k));
    }
    return n;
}

// ------------------------------------------------------------------- checks

static void TestFormatSize() {
    std::printf("FormatSize\n");
    CHECK(Narrow(FormatSize(0)) == "0 B", "zero");
    CHECK(Narrow(FormatSize(1023)) == "1023 B", "sub-KB");
    CHECK(Narrow(FormatSize(1024)) == "1.0 KB", "exactly 1KB");
    CHECK(Narrow(FormatSize(1536)) == "1.5 KB", "1.5KB");
    CHECK(Narrow(FormatSize(1024ULL * 1024)) == "1.0 MB", "1MB");
    CHECK(Narrow(FormatSize(1024ULL * 1024 * 1024)) == "1.00 GB", "1GB");
    CHECK(Narrow(FormatSize(1024ULL * 1024 * 1024 * 1024)) == "1.00 TB", "1TB");
    // 191.61 GB, the Steam figure from the real scan
    const uint64_t steam = 205740000000ULL;
    const std::string s = Narrow(FormatSize(steam));
    CHECK(s.find("GB") != std::string::npos, "large value uses GB");
    std::printf("    191GB case -> %s\n", s.c_str());
}

static void TestFormatCount() {
    std::printf("FormatCount\n");
    CHECK(Narrow(FormatCount(0)) == "0", "zero");
    CHECK(Narrow(FormatCount(7)) == "7", "single digit");
    CHECK(Narrow(FormatCount(999)) == "999", "three digits");
    CHECK(Narrow(FormatCount(1000)) == "1,000", "four digits");
    CHECK(Narrow(FormatCount(12345)) == "12,345", "five digits");
    CHECK(Narrow(FormatCount(1612865)) == "1,612,865", "real file count");
    CHECK(Narrow(FormatCount(1000000000)) == "1,000,000,000", "billion");

    // Every digit-length from 1..20. The original left-to-right grouping
    // underflowed size_t at lengths 2, 5, 8, ... so walk all of them.
    CHECK(Narrow(FormatCount(12)) == "12", "two digits");
    CHECK(Narrow(FormatCount(12345678)) == "12,345,678", "eight digits");
    CHECK(Narrow(FormatCount(12345678901ULL)) == "12,345,678,901",
          "eleven digits");
    CHECK(Narrow(FormatCount(UINT64_MAX)) == "18,446,744,073,709,551,615",
          "uint64 max");

    for (int len = 1; len <= 19; ++len) {
        uint64_t v = 1;
        for (int k = 1; k < len; ++k) v *= 10;
        const std::string s = Narrow(FormatCount(v));
        size_t digitCount = 0, commaCount = 0;
        for (char ch : s) {
            if (ch == ',') ++commaCount; else ++digitCount;
        }
        const size_t expectCommas = (len - 1) / 3;
        if (digitCount != static_cast<size_t>(len) ||
            commaCount != expectCommas) {
            std::printf("    len %d -> %s (%zu digits, %zu commas, "
                        "expected %d/%zu)\n",
                        len, s.c_str(), digitCount, commaCount,
                        len, expectCommas);
        }
        CHECK(digitCount == static_cast<size_t>(len), "digit count preserved");
        CHECK(commaCount == expectCommas, "separator count correct");
    }
}

static void TestCategories() {
    std::printf("CategoryForFile\n");
    CHECK(CategoryForFile(L"movie.mp4") == Cat::Media, "mp4 media");
    CHECK(CategoryForFile(L"MOVIE.MP4") == Cat::Media, "case insensitive");
    CHECK(CategoryForFile(L"disk.vmdk") == Cat::VirtualDisk, "vmdk");
    CHECK(CategoryForFile(L"WATCHDOG-20260831.dmp") == Cat::VirtualDisk, "dmp");
    CHECK(CategoryForFile(L"linuxmint-21.3-xfce-64bit.iso") == Cat::VirtualDisk,
          "iso with dots in stem");
    CHECK(CategoryForFile(L"main.cpp") == Cat::Code, "cpp");
    CHECK(CategoryForFile(L"Get-DiskHogs.ps1") == Cat::Code, "ps1");
    CHECK(CategoryForFile(L"kernel32.dll") == Cat::Binary, "dll");
    CHECK(CategoryForFile(L"CZFEUPBRNF.sqlite") == Cat::Database, "sqlite");
    CHECK(CategoryForFile(L"Textures-part0.pak") == Cat::Game, "pak");
    CHECK(CategoryForFile(L"noextension") == Cat::Other, "no extension");
    CHECK(CategoryForFile(L"trailingdot.") == Cat::Other, "trailing dot");
    CHECK(CategoryForFile(L"") == Cat::Other, "empty name");
    CHECK(CategoryForFile(L".gitignore") == Cat::Other, "dotfile");
    CHECK(CategoryForFile(L"x.verylongextension") == Cat::Other,
          "over-long extension rejected");
}

static void TestSanitize() {
    std::printf("SanitizeForDisplay\n");
    bool mod = false;

    std::wstring clean = SanitizeForDisplay(L"normal_file.txt", &mod);
    CHECK(!mod, "clean name untouched");
    CHECK(clean == L"normal_file.txt", "clean name preserved");

    // U+202E RIGHT-TO-LEFT OVERRIDE: the classic extension-spoofing trick.
    std::wstring spoof = L"invoice";
    spoof.push_back(static_cast<wchar_t>(0x202E));
    spoof += L"gpj.exe";
    std::wstring safe = SanitizeForDisplay(spoof, &mod);
    CHECK(mod, "RTL override flagged");
    CHECK(safe.find(static_cast<wchar_t>(0x202E)) == std::wstring::npos,
          "RTL override removed");
    CHECK(safe.size() == spoof.size(), "length preserved by substitution");

    std::wstring nul = L"ab";
    nul.push_back(L'\0');
    nul += L"cd";
    std::wstring safeNul = SanitizeForDisplay(nul, &mod);
    CHECK(mod, "embedded NUL flagged");
    CHECK(safeNul.find(L'\0') == std::wstring::npos, "NUL removed");

    std::wstring zw = L"a";
    zw.push_back(static_cast<wchar_t>(0x200B));
    zw += L"b";
    SanitizeForDisplay(zw, &mod);
    CHECK(mod, "zero-width space flagged");

    std::wstring nl = L"line1\nline2\ttab";
    SanitizeForDisplay(nl, &mod);
    CHECK(mod, "control chars flagged");
}

static void TestSatAdd() {
    std::printf("SatAdd\n");
    CHECK(SatAdd(1, 2) == 3, "normal add");
    CHECK(SatAdd(0, 0) == 0, "zero");
    CHECK(SatAdd(UINT64_MAX, 1) == UINT64_MAX, "saturates at max");
    CHECK(SatAdd(UINT64_MAX, UINT64_MAX) == UINT64_MAX, "double max saturates");
    CHECK(SatAdd(UINT64_MAX - 5, 5) == UINT64_MAX, "exact boundary");
    CHECK(SatAdd(UINT64_MAX - 5, 6) == UINT64_MAX, "one past boundary");
}

// Cells must stay inside the bounds they were given.
static void TestTreemapBounds() {
    std::printf("Treemap containment\n");

    Node root = MakeDir(L"root", {
        MakeFile(L"a.bin", 500), MakeFile(L"b.bin", 300),
        MakeFile(L"c.bin", 150), MakeFile(L"d.bin",  40),
        MakeFile(L"e.bin",  10),
    });

    Rect bounds{0, 0, 800, 600};
    std::vector<Cell> cells;
    BuildTreemap(root, bounds, 6, 1.0f, cells);

    CHECK(!cells.empty(), "produced cells");

    bool inside = true;
    for (const Cell& c : cells) {
        const float eps = 0.01f;
        if (c.rect.x < bounds.x - eps || c.rect.y < bounds.y - eps ||
            c.rect.right()  > bounds.right()  + eps ||
            c.rect.bottom() > bounds.bottom() + eps) {
            inside = false;
            std::printf("    escaped: %.2f,%.2f %.2fx%.2f\n",
                        c.rect.x, c.rect.y, c.rect.w, c.rect.h);
        }
    }
    CHECK(inside, "all cells within bounds");

    bool positive = true;
    for (const Cell& c : cells) {
        if (c.rect.w < 0.0f || c.rect.h < 0.0f) positive = false;
    }
    CHECK(positive, "no negative extents");
}

// Total cell area at depth 0 should approximate the bounds area, and each
// cell's area should be proportional to its node's size.
static void TestTreemapProportionality() {
    std::printf("Treemap proportionality\n");

    Node root = MakeDir(L"root", {
        MakeFile(L"big.bin",   6000),
        MakeFile(L"mid.bin",   3000),
        MakeFile(L"small.bin", 1000),
    });

    Rect bounds{0, 0, 1000, 1000};
    std::vector<Cell> cells;
    BuildTreemap(root, bounds, 1, 1.0f, cells);

    double totalArea = 0.0;
    for (const Cell& c : cells) {
        if (c.depth == 0) totalArea += double(c.rect.w) * double(c.rect.h);
    }
    const double boundsArea = 1000.0 * 1000.0;
    const double ratio = totalArea / boundsArea;
    std::printf("    area coverage: %.4f\n", ratio);
    CHECK(std::fabs(ratio - 1.0) < 0.01, "depth-0 cells fill the bounds");

    for (const Cell& c : cells) {
        if (c.depth != 0) continue;
        const double area = double(c.rect.w) * double(c.rect.h);
        const double expect = double(c.node->size) / 10000.0 * boundsArea;
        const double err = std::fabs(area - expect) / expect;
        if (err > 0.02) {
            std::printf("    %s: area %.0f expected %.0f (%.1f%% off)\n",
                        Narrow(c.node->name).c_str(), area, expect, err * 100);
        }
        CHECK(err < 0.02, "cell area proportional to size");
    }
}

// Sibling cells at the same depth must not overlap.
static void TestTreemapNoOverlap() {
    std::printf("Treemap overlap\n");

    std::mt19937 rng(20260831);
    std::uniform_int_distribution<uint64_t> dist(1, 100000);

    std::vector<Node> kids;
    for (int i = 0; i < 40; ++i) {
        wchar_t nm[32];
        std::swprintf(nm, 32, L"f%03d.bin", i);
        kids.push_back(MakeFile(nm, dist(rng)));
    }
    Node root = MakeDir(L"root", std::move(kids));

    Rect bounds{0, 0, 1280, 720};
    std::vector<Cell> cells;
    BuildTreemap(root, bounds, 1, 1.0f, cells);

    int overlaps = 0;
    for (size_t i = 0; i < cells.size(); ++i) {
        for (size_t j = i + 1; j < cells.size(); ++j) {
            if (cells[i].depth != cells[j].depth) continue;
            const Rect& a = cells[i].rect;
            const Rect& b = cells[j].rect;
            const float eps = 0.01f;
            const bool sep = a.right()  <= b.x + eps || b.right()  <= a.x + eps ||
                             a.bottom() <= b.y + eps || b.bottom() <= a.y + eps;
            if (!sep) ++overlaps;
        }
    }
    std::printf("    %zu cells, %d overlapping pairs\n", cells.size(), overlaps);
    CHECK(overlaps == 0, "no sibling overlap");
}

// The squarified algorithm exists to keep cells near-square; verify it does.
static void TestTreemapAspectRatio() {
    std::printf("Treemap aspect ratio\n");

    std::mt19937 rng(1234);
    std::uniform_int_distribution<uint64_t> dist(1000, 500000);

    std::vector<Node> kids;
    for (int i = 0; i < 60; ++i) {
        wchar_t nm[32];
        std::swprintf(nm, 32, L"f%03d.bin", i);
        kids.push_back(MakeFile(nm, dist(rng)));
    }
    Node root = MakeDir(L"root", std::move(kids));

    std::vector<Cell> cells;
    BuildTreemap(root, Rect{0, 0, 1000, 700}, 1, 4.0f, cells);

    double worst = 0.0;
    double sum = 0.0;
    int n = 0;
    for (const Cell& c : cells) {
        if (c.rect.w < 1.0f || c.rect.h < 1.0f) continue;
        const double ar = std::max(c.rect.w / c.rect.h, c.rect.h / c.rect.w);
        worst = std::max(worst, ar);
        sum += ar;
        ++n;
    }
    const double mean = n ? sum / n : 0.0;
    std::printf("    mean aspect %.2f, worst %.2f, over %d cells\n",
                mean, worst, n);
    CHECK(mean < 2.5, "mean aspect ratio stays near square");
    CHECK(worst < 12.0, "no extreme slivers");
}

// Deep nesting must not blow the stack or emit runaway cell counts.
static void TestTreemapDeepNesting() {
    std::printf("Treemap deep nesting\n");

    Node leaf = MakeFile(L"leaf.bin", 1024 * 1024);
    Node cur = MakeDir(L"d40", {std::move(leaf)});
    for (int i = 39; i >= 0; --i) {
        wchar_t nm[16];
        std::swprintf(nm, 16, L"d%02d", i);
        std::vector<Node> one;
        one.push_back(std::move(cur));
        cur = MakeDir(nm, std::move(one));
    }

    std::vector<Cell> cells;
    BuildTreemap(cur, Rect{0, 0, 900, 900}, 8, 1.0f, cells);

    int maxDepth = 0;
    for (const Cell& c : cells) maxDepth = std::max(maxDepth, c.depth);
    std::printf("    %zu cells, max depth %d\n", cells.size(), maxDepth);
    CHECK(maxDepth <= 8, "maxDepth respected");
    CHECK(!cells.empty(), "produced cells");
}

// minArea must actually cap output on a large synthetic tree.
static void TestTreemapCellCap() {
    std::printf("Treemap cell cap\n");

    std::mt19937 rng(99);
    std::uniform_int_distribution<uint64_t> dist(1, 10000);

    std::vector<Node> dirs;
    for (int d = 0; d < 50; ++d) {
        std::vector<Node> kids;
        for (int f = 0; f < 400; ++f) {
            wchar_t nm[32];
            std::swprintf(nm, 32, L"f%04d.dat", f);
            kids.push_back(MakeFile(nm, dist(rng)));
        }
        wchar_t dn[16];
        std::swprintf(dn, 16, L"dir%02d", d);
        dirs.push_back(MakeDir(dn, std::move(kids)));
    }
    Node root = MakeDir(L"root", std::move(dirs));
    std::printf("    tree holds %u files\n", root.files);

    std::vector<Cell> cells;
    BuildTreemap(root, Rect{0, 0, 1600, 900}, 6, 16.0f, cells);
    std::printf("    emitted %zu cells at minArea=16\n", cells.size());
    CHECK(cells.size() < 20000, "cell count bounded well below file count");
    CHECK(!cells.empty(), "still produced cells");
}

static void TestZeroAndDegenerate() {
    std::printf("Degenerate inputs\n");

    std::vector<Cell> cells;

    Node empty(L"empty", true);
    BuildTreemap(empty, Rect{0, 0, 100, 100}, 4, 1.0f, cells);
    CHECK(cells.empty(), "empty dir yields no cells");

    Node zero = MakeDir(L"zero", {MakeFile(L"a", 0), MakeFile(L"b", 0)});
    BuildTreemap(zero, Rect{0, 0, 100, 100}, 4, 1.0f, cells);
    CHECK(cells.empty(), "all-zero sizes yield no cells");

    Node ok = MakeDir(L"ok", {MakeFile(L"a", 100)});
    BuildTreemap(ok, Rect{0, 0, 0, 0}, 4, 1.0f, cells);
    CHECK(cells.empty(), "zero-area bounds yields no cells");

    BuildTreemap(ok, Rect{0, 0, -50, -50}, 4, 1.0f, cells);
    CHECK(cells.empty(), "negative bounds yields no cells");

    Node huge = MakeDir(L"huge", {MakeFile(L"a", UINT64_MAX),
                                  MakeFile(L"b", UINT64_MAX)});
    BuildTreemap(huge, Rect{0, 0, 500, 500}, 4, 1.0f, cells);
    CHECK(huge.size == UINT64_MAX, "parent size saturated, did not wrap");
    std::printf("    saturated tree produced %zu cells\n", cells.size());
}

static void TestHitTest() {
    std::printf("HitTest\n");

    Node root = MakeDir(L"root", {
        MakeDir(L"sub", {MakeFile(L"inner.bin", 400)}),
        MakeFile(L"outer.bin", 600),
    });

    std::vector<Cell> cells;
    BuildTreemap(root, Rect{0, 0, 400, 400}, 4, 1.0f, cells);
    CHECK(!cells.empty(), "cells built");

    const Cell* miss = HitTest(cells, -10.0f, -10.0f);
    CHECK(miss == nullptr, "point outside returns nullptr");

    int hits = 0;
    for (const Cell& c : cells) {
        const float cx = c.rect.x + c.rect.w * 0.5f;
        const float cy = c.rect.y + c.rect.h * 0.5f;
        const Cell* h = HitTest(cells, cx, cy);
        if (h) ++hits;
    }
    CHECK(hits == static_cast<int>(cells.size()),
          "every cell centre hits something");

    // The deepest cell must win where a child sits inside its parent.
    for (const Cell& c : cells) {
        if (c.depth == 0) continue;
        const float cx = c.rect.x + c.rect.w * 0.5f;
        const float cy = c.rect.y + c.rect.h * 0.5f;
        const Cell* h = HitTest(cells, cx, cy);
        CHECK(h && h->depth >= c.depth, "hit resolves to deepest cell");
        break;
    }
}


// ---------------------------------------------------------------- fuzzing

// Filenames are attacker-controlled: this tool is meant to be pointed at
// directories full of hostile samples. Throw randomised names and tree shapes
// at the two functions that touch them and confirm nothing escapes.
static void FuzzSanitize() {
    std::printf("Fuzz: SanitizeForDisplay\n");

    std::mt19937 rng(0xBADC0DE);
    std::uniform_int_distribution<int> lenDist(0, 300);
    std::uniform_int_distribution<uint32_t> chDist(0, 0x10FFFF);

    int escaped = 0;
    for (int iter = 0; iter < 20000; ++iter) {
        std::wstring name;
        const int len = lenDist(rng);
        name.reserve(static_cast<size_t>(len));
        for (int i = 0; i < len; ++i) {
            name.push_back(static_cast<wchar_t>(chDist(rng)));
        }

        bool mod = false;
        const std::wstring out = SanitizeForDisplay(name, &mod);

        if (out.size() != name.size()) { ++escaped; continue; }

        for (wchar_t c : out) {
            const uint32_t u =
                static_cast<uint32_t>(
                    static_cast<std::make_unsigned<wchar_t>::type>(c));
            const bool bad = (u < 0x20) || (u == 0x7F) ||
                             (u >= 0x80 && u <= 0x9F) ||
                             (u >= 0x202A && u <= 0x202E) ||
                             (u >= 0x2066 && u <= 0x2069) ||
                             (u == 0x200B) || (u == 0x200C) ||
                             (u == 0x200D) || (u == 0xFEFF) || (u == 0x00AD);
            if (bad) { ++escaped; break; }
        }
    }
    std::printf("    20000 random names, %d leaked a filtered codepoint\n",
                escaped);
    CHECK(escaped == 0, "no filtered codepoint survives sanitisation");
}

static void FuzzTreemap() {
    std::printf("Fuzz: BuildTreemap\n");

    std::mt19937 rng(0x5EED);
    std::uniform_int_distribution<int> kidsDist(0, 25);
    std::uniform_int_distribution<int> depthDist(1, 5);
    std::uniform_int_distribution<uint64_t> sizeDist(0, 1ULL << 40);
    std::uniform_real_distribution<float> dimDist(-20.0f, 2200.0f);

    int violations = 0;
    int rounds = 0;

    for (int iter = 0; iter < 900; ++iter) {
        // Random tree
        std::vector<Node> level;
        const int nkids = kidsDist(rng);
        for (int i = 0; i < nkids; ++i) {
            wchar_t nm[24];
            std::swprintf(nm, 24, L"n%d.bin", i);
            if (depthDist(rng) > 3) {
                std::vector<Node> sub;
                const int m = kidsDist(rng);
                for (int j = 0; j < m; ++j) {
                    wchar_t sn[24];
                    std::swprintf(sn, 24, L"s%d.dat", j);
                    sub.push_back(MakeFile(sn, sizeDist(rng)));
                }
                level.push_back(MakeDir(nm, std::move(sub)));
            } else {
                level.push_back(MakeFile(nm, sizeDist(rng)));
            }
        }
        Node root = MakeDir(L"fuzzroot", std::move(level));

        Rect bounds{dimDist(rng), dimDist(rng), dimDist(rng), dimDist(rng)};
        std::vector<Cell> cells;
        BuildTreemap(root, bounds, depthDist(rng), 1.0f, cells);
        ++rounds;

        for (const Cell& c : cells) {
            if (!std::isfinite(c.rect.x) || !std::isfinite(c.rect.y) ||
                !std::isfinite(c.rect.w) || !std::isfinite(c.rect.h)) {
                ++violations; break;
            }
            if (c.rect.w < -0.01f || c.rect.h < -0.01f) { ++violations; break; }
            if (bounds.w > 0.0f && bounds.h > 0.0f) {
                const float eps = 0.5f;
                if (c.rect.x < bounds.x - eps || c.rect.y < bounds.y - eps ||
                    c.rect.right() > bounds.right() + eps ||
                    c.rect.bottom() > bounds.bottom() + eps) {
                    ++violations; break;
                }
            }
            // Hit-testing every produced cell must not fault.
            HitTest(cells, c.rect.x + c.rect.w * 0.5f,
                    c.rect.y + c.rect.h * 0.5f);
        }
    }
    std::printf("    %d random trees incl. negative/zero bounds, "
                "%d violations\n", rounds, violations);
    CHECK(violations == 0, "no NaN, negative or escaping cell under fuzz");
}


// A parent that draws a label reserves a strip for it. If any child overlaps
// that strip the two labels render on top of each other -- which is exactly
// the bug this invariant exists to catch.
static void TestLabelStripClear() {
    std::printf("Label strip exclusivity\n");

    std::mt19937 rng(0xABE1);
    std::uniform_int_distribution<uint64_t> sizeDist(1000, 900000);

    std::vector<Node> top;
    for (int d = 0; d < 12; ++d) {
        std::vector<Node> kids;
        for (int f = 0; f < 9; ++f) {
            wchar_t nm[40];
            std::swprintf(nm, 40, L"Counter-Strike Global Offensive %d.pak", f);
            kids.push_back(MakeFile(nm, sizeDist(rng)));
        }
        wchar_t dn[40];
        std::swprintf(dn, 40, L"Cyberpunk 2077 dir %d", d);
        top.push_back(MakeDir(dn, std::move(kids)));
    }
    Node root = MakeDir(L"E:\\", std::move(top));

    std::vector<Cell> cells;
    BuildTreemap(root, Rect{0, 0, 1000, 700}, 5, 4.0f, cells);

    int intrusions = 0;
    for (const Cell& parent : cells) {
        if (!parent.expanded || parent.header <= 0.0f) continue;
        const Rect strip{parent.rect.x, parent.rect.y, parent.rect.w,
                         parent.header};

        for (const Cell& other : cells) {
            if (&other == &parent) continue;
            if (other.depth <= parent.depth) continue;
            const float eps = 0.01f;
            const bool sep = other.rect.right()  <= strip.x + eps ||
                             strip.right()       <= other.rect.x + eps ||
                             other.rect.bottom() <= strip.y + eps ||
                             strip.bottom()      <= other.rect.y + eps;
            if (!sep) {
                ++intrusions;
                if (intrusions <= 3) {
                    std::printf("    child at %.1f,%.1f %.1fx%.1f intrudes "
                                "into strip %.1f,%.1f %.1fx%.1f\n",
                                other.rect.x, other.rect.y, other.rect.w,
                                other.rect.h, strip.x, strip.y, strip.w,
                                strip.h);
                }
            }
        }
    }

    int headed = 0, expanded = 0;
    for (const Cell& c : cells) {
        if (c.expanded) ++expanded;
        if (c.expanded && c.header > 0.0f) ++headed;
    }
    std::printf("    %zu cells, %d expanded, %d carrying a label strip, "
                "%d intrusions\n", cells.size(), expanded, headed, intrusions);

    CHECK(intrusions == 0, "no child intrudes into a parent's label strip");
    CHECK(headed > 0, "some parents did reserve a strip");
}

// An expanded cell with no strip must not be labelled at all -- its children
// are drawn over it. Verify the flags the renderer branches on are consistent.
static void TestExpandedFlagConsistency() {
    std::printf("Expanded/header flag consistency\n");

    std::mt19937 rng(4242);
    std::uniform_int_distribution<uint64_t> sizeDist(1, 5000000);

    std::vector<Node> top;
    for (int d = 0; d < 20; ++d) {
        std::vector<Node> kids;
        for (int f = 0; f < 14; ++f) {
            wchar_t nm[24];
            std::swprintf(nm, 24, L"a%d.dat", f);
            kids.push_back(MakeFile(nm, sizeDist(rng)));
        }
        wchar_t dn[24];
        std::swprintf(dn, 24, L"dir%d", d);
        top.push_back(MakeDir(dn, std::move(kids)));
    }
    Node root = MakeDir(L"root", std::move(top));

    std::vector<Cell> cells;
    BuildTreemap(root, Rect{0, 0, 1280, 800}, 5, 6.0f, cells);

    int bad = 0;
    for (const Cell& c : cells) {
        // A header without expansion would reserve dead space for nothing.
        if (c.header > 0.0f && !c.expanded) ++bad;
        // Only directories can be expanded.
        if (c.expanded && !c.node->dir) ++bad;
        // A strip must fit inside its own cell.
        if (c.header > c.rect.h) ++bad;
    }
    std::printf("    %zu cells, %d inconsistent\n", cells.size(), bad);
    CHECK(bad == 0, "header/expanded flags are self-consistent");
}


// Reconstructing a nested cell's path is what "Show in Explorer" and, more
// importantly, "Move to Recycle Bin" act on. If the parent chain is wrong the
// destructive action targets a different path than the one clicked.
static void TestCellChainPaths() {
    std::printf("Cell parent chain\n");

    // Deliberately deep and wide enough that cells nest several levels.
    std::vector<Node> lvl3;
    for (int i = 0; i < 4; ++i) {
        wchar_t nm[32];
        std::swprintf(nm, 32, L"leaf%d.pak", i);
        lvl3.push_back(MakeFile(nm, 4ULL << 30));
    }
    Node deep = MakeDir(L"Paks", std::move(lvl3));

    std::vector<Node> lvl2;
    lvl2.push_back(std::move(deep));
    lvl2.push_back(MakeFile(L"sibling.bin", 2ULL << 30));
    Node content = MakeDir(L"Content", std::move(lvl2));

    std::vector<Node> lvl1;
    lvl1.push_back(std::move(content));
    Node game = MakeDir(L"Cyberpunk 2077", std::move(lvl1));

    std::vector<Node> top;
    top.push_back(std::move(game));
    Node root = MakeDir(L"E:\\", std::move(top));

    std::vector<Cell> cells;
    BuildTreemap(root, Rect{0, 0, 1400, 900}, 5, 4.0f, cells);
    std::printf("    %zu cells\n", cells.size());

    int checked = 0, wrong = 0, deepest = 0;
    for (size_t i = 0; i < cells.size(); ++i) {
        const std::vector<const Node*> chain =
            CellChain(cells, static_cast<int>(i));

        if (chain.empty()) { ++wrong; continue; }
        // Chain must end at this cell and be one longer per level of depth.
        if (chain.back() != cells[i].node) ++wrong;
        if (chain.size() != static_cast<size_t>(cells[i].depth) + 1) ++wrong;
        deepest = std::max(deepest, cells[i].depth);

        // Every link must be a real parent/child relationship in the tree.
        for (size_t k = 0; k + 1 < chain.size(); ++k) {
            bool found = false;
            for (const Node& child : chain[k]->children) {
                if (&child == chain[k + 1]) { found = true; break; }
            }
            if (!found) ++wrong;
        }
        ++checked;
    }
    std::printf("    %d chains checked, deepest depth %d, %d wrong\n",
                checked, deepest, wrong);
    CHECK(wrong == 0, "every cell chain is a real root-to-cell path");
    CHECK(deepest >= 2, "test actually produced nested cells");

    // Out-of-range indices must be handled, not indexed blindly.
    CHECK(CellChain(cells, -1).empty(), "negative index yields empty chain");
    CHECK(CellChain(cells, 999999).empty(), "over-range index yields empty");

    // A corrupted parent link must terminate rather than spin.
    std::vector<Cell> broken = cells;
    if (broken.size() > 2) {
        broken[1].parent = static_cast<int>(broken.size()) - 1;
        broken[broken.size() - 1].parent = 1;   // cycle
        const std::vector<const Node*> c = CellChain(broken, 1);
        std::printf("    cyclic parent link terminated at %zu entries\n",
                    c.size());
        CHECK(c.size() <= broken.size() + 1, "cyclic chain is bounded");
    }
}

// HitTestIndex must agree with HitTest and resolve to the deepest cell.
static void TestHitTestIndexAgreement() {
    std::printf("HitTestIndex agreement\n");

    std::mt19937 rng(777);
    std::uniform_int_distribution<uint64_t> sz(1000, 800000);

    std::vector<Node> kids;
    for (int i = 0; i < 8; ++i) {
        std::vector<Node> sub;
        for (int j = 0; j < 6; ++j) {
            wchar_t nm[24];
            std::swprintf(nm, 24, L"f%d.dat", j);
            sub.push_back(MakeFile(nm, sz(rng)));
        }
        wchar_t dn[24];
        std::swprintf(dn, 24, L"d%d", i);
        kids.push_back(MakeDir(dn, std::move(sub)));
    }
    Node root = MakeDir(L"root", std::move(kids));

    std::vector<Cell> cells;
    BuildTreemap(root, Rect{0, 0, 1200, 800}, 5, 4.0f, cells);

    int mismatch = 0;
    for (const Cell& c : cells) {
        const float cx = c.rect.x + c.rect.w * 0.5f;
        const float cy = c.rect.y + c.rect.h * 0.5f;
        const int idx = HitTestIndex(cells, cx, cy);
        const Cell* ptr = HitTest(cells, cx, cy);
        if (idx < 0 || !ptr) { ++mismatch; continue; }
        if (&cells[static_cast<size_t>(idx)] != ptr) ++mismatch;
    }
    std::printf("    %zu probes, %d mismatches\n", cells.size(), mismatch);
    CHECK(mismatch == 0, "index and pointer hit tests agree");
    CHECK(HitTestIndex(cells, -5.0f, -5.0f) == -1, "outside returns -1");
}


// ---------------------------------------------------------------- reporting

static Node BuildReportTree() {
    return MakeDir(L"root", {
        MakeDir(L"games", {
            MakeFile(L"Textures-part0.pak", 2000000000ull),
            MakeFile(L"IPL_Objects.pak",    1900000000ull),
            MakeFile(L"readme.txt",         1024),
        }),
        MakeDir(L"vms", {
            MakeFile(L"win10.vmdk", 4000000000ull),
            MakeFile(L"kali.vmdk",  3000000000ull),
        }),
        MakeDir(L"src", {
            MakeFile(L"main.cpp", 40000),
            MakeFile(L"util.cpp", 30000),
            MakeFile(L"NOEXT",    5000),
        }),
    });
}

static void TestExtensionBreakdown() {
    std::printf("ExtensionBreakdown\n");

    const Node root = BuildReportTree();
    const std::vector<ExtStat> stats = ExtensionBreakdown(root, 20);

    CHECK(!stats.empty(), "produced rows");

    // Sorted descending, and the totals must account for every file byte.
    uint64_t total = 0;
    uint32_t count = 0;
    for (size_t i = 0; i < stats.size(); ++i) {
        total = SatAdd(total, stats[i].bytes);
        count += stats[i].count;
        if (i > 0) {
            CHECK(stats[i - 1].bytes >= stats[i].bytes, "sorted descending");
        }
    }
    std::printf("    %zu extensions, %u files, %s\n", stats.size(), count,
                Narrow(FormatSize(total)).c_str());
    CHECK(total == root.size, "extension totals sum to the tree size");
    CHECK(count == root.files, "extension counts sum to the file count");

    CHECK(Narrow(stats[0].ext) == "vmdk", "largest extension is vmdk");
    CHECK(stats[0].bytes == 7000000000ull, "vmdk total");

    bool sawEmpty = false;
    for (const ExtStat& e : stats) if (e.ext.empty()) sawEmpty = true;
    CHECK(sawEmpty, "files without an extension are counted");

    // The limit must fold the tail rather than dropping it.
    const std::vector<ExtStat> capped = ExtensionBreakdown(root, 2);
    uint64_t cappedTotal = 0;
    uint32_t cappedCount = 0;
    for (const ExtStat& e : capped) {
        cappedTotal = SatAdd(cappedTotal, e.bytes);
        cappedCount += e.count;
    }
    std::printf("    capped to 2: %zu rows, still %s\n", capped.size(),
                Narrow(FormatSize(cappedTotal)).c_str());
    CHECK(cappedTotal == root.size, "capped totals still sum to the tree");
    CHECK(cappedCount == root.files, "capped counts still sum");

    const Node empty(L"empty", true);
    CHECK(ExtensionBreakdown(empty, 10).empty(), "empty tree yields no rows");
}

static void TestLargestFiles() {
    std::printf("LargestFiles\n");

    const Node root = BuildReportTree();
    const std::vector<FileHit> top = LargestFiles(root, 3);

    CHECK(top.size() == 3, "honours the limit");
    if (top.size() == 3) {
        CHECK(top[0].size == 4000000000ull, "largest first");
        CHECK(top[1].size == 3000000000ull, "second");
        CHECK(top[2].size == 2000000000ull, "third");
        CHECK(Narrow(top[0].path) == "vms\\win10.vmdk", "path is relative");
        std::printf("    top: %s (%s)\n", Narrow(top[0].path).c_str(),
                    Narrow(FormatSize(top[0].size)).c_str());
    }

    for (const FileHit& h : top) {
        CHECK(h.node && !h.node->dir, "directories excluded");
    }

    const std::vector<FileHit> all = LargestFiles(root, 1000);
    CHECK(all.size() == root.files, "limit above the count returns everything");
    CHECK(LargestFiles(root, 0).empty(), "zero limit returns nothing");

    const Node empty(L"empty", true);
    CHECK(LargestFiles(empty, 10).empty(), "empty tree yields nothing");

    // Ordering must hold on a large random tree, where the top-N insertion
    // path rather than a full sort is what actually runs.
    std::mt19937 rng(555);
    std::uniform_int_distribution<uint64_t> sz(1, 1ULL << 40);
    std::vector<Node> kids;
    for (int i = 0; i < 4000; ++i) {
        wchar_t nm[24];
        std::swprintf(nm, 24, L"f%04d.bin", i);
        kids.push_back(MakeFile(nm, sz(rng)));
    }
    const Node big = MakeDir(L"big", std::move(kids));
    const std::vector<FileHit> top20 = LargestFiles(big, 20);
    CHECK(top20.size() == 20, "top 20 of 4000");
    bool ordered = true;
    for (size_t i = 1; i < top20.size(); ++i) {
        if (top20[i - 1].size < top20[i].size) ordered = false;
    }
    CHECK(ordered, "top-N insertion preserves ordering");

    // And it must actually be the top 20, not just 20 sorted entries.
    uint64_t twentieth = top20.back().size;
    size_t larger = 0;
    for (const Node& c : big.children) if (c.size > twentieth) ++larger;
    std::printf("    %zu files exceed the 20th largest (expected < 20)\n",
                larger);
    CHECK(larger < 20, "no file outside the list beats the cut-off");
}

static void TestFindByName() {
    std::printf("FindByName\n");

    const Node root = BuildReportTree();

    const std::vector<FileHit> vmdk = FindByName(root, L"vmdk", 100);
    CHECK(vmdk.size() == 2, "matched both vmdk files");

    const std::vector<FileHit> upper = FindByName(root, L"VMDK", 100);
    CHECK(upper.size() == 2, "search is case-insensitive");

    const std::vector<FileHit> dirs = FindByName(root, L"games", 100);
    CHECK(dirs.size() == 1, "directories are searchable too");

    CHECK(FindByName(root, L"", 100).empty(), "empty needle matches nothing");
    CHECK(FindByName(root, L"zzzznope", 100).empty(), "no match yields none");
    CHECK(FindByName(root, L"vmdk", 1).size() == 1, "limit honoured");

    bool ordered = true;
    for (size_t i = 1; i < vmdk.size(); ++i) {
        if (vmdk[i - 1].size < vmdk[i].size) ordered = false;
    }
    CHECK(ordered, "results sorted by size");
    std::printf("    'vmdk' matched %zu, largest %s\n", vmdk.size(),
                Narrow(FormatSize(vmdk[0].size)).c_str());
}

static void TestCsvExport() {
    std::printf("CSV export\n");

    // Names chosen to exercise the quoting rules and the sanitiser: a comma,
    // an embedded quote, and a bidi override.
    std::wstring spoof = L"invoice";
    spoof.push_back(static_cast<wchar_t>(0x202E));
    spoof += L"gpj.exe";

    const Node root = MakeDir(L"root", {
        MakeFile(L"plain.txt", 100),
        MakeFile(L"has,comma.bin", 200),
        MakeFile(L"has\"quote.bin", 300),
        MakeFile(spoof, 400),
    });

    const char* path = "/tmp/spindle_test_export.csv";
    const std::wstring wpath(path, path + std::strlen(path));
    CHECK(ExportCsv(root, L"C:\\test", wpath), "export succeeds");

    FILE* f = std::fopen(path, "rb");
    CHECK(f != nullptr, "file was written");
    if (!f) return;

    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);

    CHECK(text.size() > 3 &&
          static_cast<unsigned char>(text[0]) == 0xEF &&
          static_cast<unsigned char>(text[1]) == 0xBB &&
          static_cast<unsigned char>(text[2]) == 0xBF, "UTF-8 BOM present");
    CHECK(text.find("path,bytes,kind,files") != std::string::npos,
          "header row");
    CHECK(text.find("\"C:\\test\\has,comma.bin\"") != std::string::npos,
          "comma-bearing field is quoted");
    CHECK(text.find("\"\"quote") != std::string::npos,
          "embedded quote is doubled");

    // The bidi override must not survive into the exported file.
    CHECK(text.find("\xE2\x80\xAE") == std::string::npos,
          "U+202E stripped before export");
    std::printf("    %zu bytes written, quoting and sanitisation verified\n",
                text.size());

    std::remove(path);

    const Node empty(L"empty", true);
    CHECK(ExportCsv(empty, L"C:\\", L"/tmp/spindle_empty.csv"),
          "empty tree still writes a header");
    std::remove("/tmp/spindle_empty.csv");
    CHECK(!ExportCsv(root, L"C:\\", L"/nonexistent-dir/x.csv"),
          "unwritable path reports failure");
}

// The report walkers must not blow up on the tree shapes a hostile or merely
// unusual volume produces.
static void TestReportsOnHostileTrees() {
    std::printf("Reports on degenerate trees\n");

    // Very deep chain: recursion in these walkers would be a stack overflow.
    Node cur = MakeFile(L"leaf.bin", 1024);
    for (int i = 0; i < 4000; ++i) {
        std::vector<Node> one;
        one.push_back(std::move(cur));
        cur = MakeDir(L"d", std::move(one));
    }
    const std::vector<ExtStat> deepExt = ExtensionBreakdown(cur, 10);
    const std::vector<FileHit> deepTop = LargestFiles(cur, 10);
    const std::vector<FileHit> deepFind = FindByName(cur, L"leaf", 10);
    std::printf("    4000-deep chain: %zu ext rows, %zu files, %zu matches\n",
                deepExt.size(), deepTop.size(), deepFind.size());
    CHECK(deepTop.size() == 1, "found the single leaf at depth 4000");
    CHECK(deepFind.size() == 1, "search reached depth 4000");
    CHECK(deepTop[0].path.size() > 4000, "path reflects the full depth");

    // Saturated sizes must not wrap the aggregate.
    const Node sat = MakeDir(L"sat", {
        MakeFile(L"a.bin", UINT64_MAX),
        MakeFile(L"b.bin", UINT64_MAX),
    });
    const std::vector<ExtStat> satExt = ExtensionBreakdown(sat, 10);
    CHECK(!satExt.empty() && satExt[0].bytes == UINT64_MAX,
          "aggregate saturates rather than wrapping");

    // Thousands of distinct extensions must be bounded.
    std::vector<Node> many;
    for (int i = 0; i < 9000; ++i) {
        wchar_t nm[32];
        std::swprintf(nm, 32, L"f.e%04d", i);
        many.push_back(MakeFile(nm, 100));
    }
    const Node wide = MakeDir(L"wide", std::move(many));
    const std::vector<ExtStat> wideExt = ExtensionBreakdown(wide, 12);
    std::printf("    9000 distinct extensions -> %zu rows\n", wideExt.size());
    CHECK(wideExt.size() <= 13, "row count bounded by the limit plus tail");
}


// ------------------------------------------------------------------ search

static Node BuildSearchTree() {
    return MakeDir(L"root", {
        MakeDir(L"Steam", {
            MakeFile(L"Textures-part0.pak", 2000000000ull),
            MakeFile(L"IPL_Objects.pak",    1900000000ull),
            MakeFile(L"temp_cache.pak",       50000000ull),
        }),
        MakeDir(L"VMware", {
            MakeFile(L"win10.vmdk",  4000000000ull),
            MakeFile(L"kali.vmdk",   3000000000ull),
            MakeFile(L"notes.txt",         2048),
        }),
        MakeDir(L"temp files", {
            MakeFile(L"holiday photo.jpg", 5000000ull),
            MakeFile(L"song.mp3",          8000000ull),
        }),
    });
}

static void TestQueryParsing() {
    std::printf("ParseQuery\n");

    Query q = ParseQuery(L"pak");
    CHECK(q.include.size() == 1 && q.include[0] == L"pak", "bare name term");
    CHECK(q.Empty() == false, "non-empty query");

    CHECK(ParseQuery(L"").Empty(), "empty text yields an empty query");
    CHECK(ParseQuery(L"   ").Empty(), "whitespace only yields empty");

    q = ParseQuery(L"PAK");
    CHECK(q.include[0] == L"pak", "name terms are lower-cased");

    q = ParseQuery(L"\"two words\"");
    CHECK(q.include.size() == 1 && q.include[0] == L"two words",
          "quoted phrase stays one term");

    q = ParseQuery(L"kind:media");
    CHECK(q.kinds.size() == 1 && q.kinds[0] == Cat::Media, "kind:media");

    q = ParseQuery(L"kind:vm");
    CHECK(q.kinds.size() == 1 && q.kinds[0] == Cat::VirtualDisk,
          "kind aliases resolve");

    q = ParseQuery(L"kind:nonsense");
    CHECK(q.kinds.empty() && q.include.size() == 1,
          "unknown kind falls back to a name term");

    q = ParseQuery(L"ext:vmdk");
    CHECK(q.exts.size() == 1 && q.exts[0] == L"vmdk", "ext:");
    q = ParseQuery(L"ext:.VMDK");
    CHECK(q.exts.size() == 1 && q.exts[0] == L"vmdk",
          "leading dot and case handled");

    q = ParseQuery(L">500mb");
    CHECK(q.minSize == 500ull * 1024 * 1024, "greater-than size");
    q = ParseQuery(L"<2gb");
    CHECK(q.maxSize == 2ull * 1024 * 1024 * 1024, "less-than size");
    q = ParseQuery(L"size:>1gb");
    CHECK(q.minSize == 1024ull * 1024 * 1024, "size: prefix");
    q = ParseQuery(L">=1kb");
    CHECK(q.minSize == 1024, ">= accepted");
    q = ParseQuery(L">1.5gb");
    CHECK(q.minSize == 1610612736ull, "fractional size");
    q = ParseQuery(L">1024");
    CHECK(q.minSize == 1024, "bare bytes");

    q = ParseQuery(L"is:file");
    CHECK(q.only == Query::Only::Files, "is:file");
    q = ParseQuery(L"is:folder");
    CHECK(q.only == Query::Only::Folders, "is:folder");

    q = ParseQuery(L"-temp");
    CHECK(q.exclude.size() == 1 && q.exclude[0] == L"temp", "negation");

    q = ParseQuery(L"kind:media >100mb -temp pak");
    CHECK(q.kinds.size() == 1, "combined: kind");
    CHECK(q.minSize == 100ull * 1024 * 1024, "combined: size");
    CHECK(q.exclude.size() == 1, "combined: exclusion");
    CHECK(q.include.size() == 1, "combined: name");
    std::printf("    'kind:media >100mb -temp pak' parsed into 4 constraints\n");

    // Malformed sizes must not become silent filters.
    q = ParseQuery(L">notasize");
    CHECK(q.minSize == 0 && q.include.size() == 1,
          "bad size becomes a name term");
    q = ParseQuery(L">999999999999999999999999gb");
    CHECK(q.minSize == 0, "overflowing size rejected");
}

static void TestQueryMatching() {
    std::printf("FindMatching\n");

    const Node root = BuildSearchTree();

    auto count = [&](const wchar_t* text) {
        return FindMatching(root, ParseQuery(text), 500).size();
    };

    CHECK(count(L"pak") == 3, "name term matches three paks");
    CHECK(count(L"ext:pak") == 3, "ext: matches the same three");
    CHECK(count(L"ext:vmdk") == 2, "ext:vmdk");
    CHECK(count(L"kind:vm") == 2, "kind:vm matches the disk images");

    CHECK(count(L"pak -temp") == 2, "negation removes the temp pak");
    // Folders match on their rolled-up total, so this is 4 files plus the
    // two folders holding them.
    CHECK(count(L">1gb") == 6, "size filter spans files and folders");
    CHECK(count(L"is:file >1gb") == 4, "size filter restricted to files");
    CHECK(count(L"ext:pak >1gb") == 2, "extension and size combined");
    CHECK(count(L"is:folder") == 3, "folders only");
    CHECK(count(L"is:file kind:media") == 2, "media files");

    // Size on a folder uses its rolled-up total.
    CHECK(count(L"is:folder >5gb") == 1, "folder size uses the subtree total");

    CHECK(count(L"\"holiday photo\"") == 1, "quoted phrase with a space");
    CHECK(count(L"zzznothing") == 0, "no match");
    CHECK(FindMatching(root, ParseQuery(L""), 500).empty(),
          "empty query matches nothing, not everything");
    CHECK(FindMatching(root, ParseQuery(L"pak"), 1).size() == 1,
          "limit honoured");

    const std::vector<FileHit> big = FindMatching(root, ParseQuery(L">1gb"), 10);
    bool ordered = true;
    for (size_t i = 1; i < big.size(); ++i) {
        if (big[i - 1].size < big[i].size) ordered = false;
    }
    CHECK(ordered, "results sorted largest first");
    std::printf("    '>1gb' matched %zu, largest %s at %s\n", big.size(),
                Narrow(big[0].node->name).c_str(),
                Narrow(big[0].path).c_str());
}

// A search box takes arbitrary text, so it is an input surface like any other.
static void FuzzQueries() {
    std::printf("Fuzz: query parsing\n");

    const Node root = BuildSearchTree();
    std::mt19937 rng(0x0DDBA11);

    const wchar_t* fragments[] = {
        L"kind:", L"ext:", L"is:", L"size:", L">", L"<", L">=", L"-", L"\"",
        L"media", L"999999999999999999999999", L"gb", L"tb", L".", L",",
        L"pak", L"", L" ", L"\t", L"0", L"-0", L"><", L"::", L"kind:kind:",
        L"\"\"\"", L"----", L">>>>", L"size:size:>", L"ext:.....",
    };
    std::uniform_int_distribution<size_t> pick(0, std::size(fragments) - 1);
    std::uniform_int_distribution<int> count(0, 12);

    for (int iter = 0; iter < 40000; ++iter) {
        std::wstring text;
        const int n = count(rng);
        for (int i = 0; i < n; ++i) {
            text += fragments[pick(rng)];
            if (rng() % 3 == 0) text += L' ';
        }
        const Query q = ParseQuery(text);
        FindMatching(root, q, 20);
    }
    std::printf("    40000 generated queries parsed and executed\n");

    // Random unicode, including the characters the sanitiser filters.
    std::uniform_int_distribution<uint32_t> ch(1, 0xFFFF);
    std::uniform_int_distribution<int> len(0, 80);
    for (int iter = 0; iter < 20000; ++iter) {
        std::wstring text;
        const int n = len(rng);
        for (int i = 0; i < n; ++i) {
            text.push_back(static_cast<wchar_t>(ch(rng)));
        }
        FindMatching(root, ParseQuery(text), 20);
    }
    std::printf("    20000 random unicode queries\n");
    CHECK(true, "query fuzzing completed without a fault");
}


static void TestEasing() {
    std::printf("Easing curves\n");

    struct Curve { const char* name; float (*fn)(float); bool overshoots; };
    const Curve curves[] = {
        {"OutQuint",   ease::OutQuint,   false},
        {"OutCubic",   ease::OutCubic,   false},
        {"OutBack",    ease::OutBack,    true},
        {"InOutCubic", ease::InOutCubic, false},
    };

    for (const Curve& c : curves) {
        // Endpoints must be exact or an animation snaps at its boundaries.
        CHECK(std::fabs(c.fn(0.0f)) < 1e-5f, "starts at 0");
        CHECK(std::fabs(c.fn(1.0f) - 1.0f) < 1e-5f, "ends at 1");

        // Out-of-range input must clamp, not extrapolate: a dropped frame can
        // easily hand these a t past 1.
        CHECK(std::fabs(c.fn(-5.0f)) < 1e-5f, "clamps below zero");
        CHECK(std::fabs(c.fn(9.0f) - 1.0f) < 1e-5f, "clamps above one");

        // Monotonic, except where overshoot is the point.
        float prev = c.fn(0.0f);
        bool monotonic = true;
        float peak = 0.0f;
        for (int i = 1; i <= 200; ++i) {
            const float v = c.fn(static_cast<float>(i) / 200.0f);
            if (v < prev - 1e-4f) monotonic = false;
            peak = std::max(peak, v);
            prev = v;
        }
        if (!c.overshoots) {
            CHECK(monotonic, "monotonic");
            CHECK(peak <= 1.0f + 1e-4f, "never exceeds 1");
        } else {
            CHECK(peak > 1.0f, "OutBack does overshoot");
            CHECK(peak < 1.15f, "overshoot stays subtle");
        }

        // No NaN anywhere across the domain.
        bool finite = true;
        for (int i = -50; i <= 250; ++i) {
            if (!std::isfinite(c.fn(static_cast<float>(i) / 200.0f))) {
                finite = false;
            }
        }
        CHECK(finite, "finite across the domain");
    }

    // The reason OutQuint is the default: most of the distance early.
    const float quintAtThird = ease::OutQuint(0.33f);
    const float cubicAtThird = ease::OutCubic(0.33f);
    std::printf("    at t=0.33  OutQuint %.3f  OutCubic %.3f  InOutCubic %.3f\n",
                quintAtThird, cubicAtThird, ease::InOutCubic(0.33f));
    CHECK(quintAtThird > 0.85f, "OutQuint is most of the way by a third");
    CHECK(quintAtThird > cubicAtThird, "OutQuint leads OutCubic early");
}

// ------------------------------------------------------------- scan cache

static bool TreesEqual(const Node& a, const Node& b) {
    struct Pair { const Node* x; const Node* y; };
    std::vector<Pair> stack{{&a, &b}};
    while (!stack.empty()) {
        const Pair p = stack.back();
        stack.pop_back();
        if (p.x->name != p.y->name || p.x->size != p.y->size ||
            p.x->files != p.y->files || p.x->dir != p.y->dir ||
            p.x->cat != p.y->cat ||
            p.x->hardlink != p.y->hardlink ||
            p.x->cloudOnly != p.y->cloudOnly ||
            p.x->children.size() != p.y->children.size()) {
            return false;
        }
        for (size_t i = 0; i < p.x->children.size(); ++i) {
            stack.push_back({&p.x->children[i], &p.y->children[i]});
        }
    }
    return true;
}

static void TestScanCache() {
    std::printf("Scan cache serialisation\n");

    ScanResult in;
    in.root = MakeDir(L"", {
        MakeDir(L"Games", {
            MakeFile(L"a.pak", 100),
            MakeFile(L"bißchen ü.bin", 5),
        }),
        MakeDir(L"empty", {}),
        MakeDir(L"deep", { MakeDir(L"er", { MakeDir(L"est", {
            MakeFile(L"leaf.txt", 1) }) }) }),
        MakeFile(L"root.iso", 12345678901ULL),
    });
    in.stats.bytes     = in.root.size;
    in.stats.fileCount = 4;
    in.stats.dirCount  = 5;
    in.stats.hardlinkFiles = 2;
    in.stats.hardlinkBytes = 4096;
    in.stats.cloudFiles    = 1;
    in.stats.cloudBytes    = 900;

    // The flags have to survive the round trip too, or a cached scan quietly
    // forgets that half of WinSxS is hardlinked.
    in.root.children[0].children[0].hardlink  = true;
    in.root.children[3].cloudOnly             = true;

    CacheMeta m;
    m.savedUnixMs  = 1725100000000ULL;
    m.volumeSerial = 0xDEADBEEFu;

    std::vector<uint8_t> buf;
    SerializeScan(in, m, buf);
    CHECK(!buf.empty(), "serialises to something");

    ScanResult out;
    CacheMeta om;
    CHECK(DeserializeScan(buf.data(), buf.size(), out, om), "round-trips");
    CHECK(om.savedUnixMs == m.savedUnixMs, "timestamp survives");
    CHECK(om.volumeSerial == m.volumeSerial, "serial survives");
    CHECK(out.stats.bytes == in.stats.bytes, "byte total survives");
    CHECK(out.stats.fileCount == in.stats.fileCount, "file count survives");
    CHECK(out.stats.dirCount == in.stats.dirCount, "dir count survives");
    CHECK(out.stats.hardlinkBytes == in.stats.hardlinkBytes,
          "hardlink bytes survive");
    CHECK(out.stats.cloudBytes == in.stats.cloudBytes,
          "cloud bytes survive");
    CHECK(TreesEqual(in.root, out.root), "tree survives byte-identically");
    CHECK(out.root.children[0].children[0].hardlink,
          "hardlink flag survives");
    CHECK(out.root.children[3].cloudOnly, "cloud flag survives");

    // Every strict prefix must be refused: a torn write may leave one behind.
    bool anyPrefixParsed = false;
    for (size_t cut = 0; cut < buf.size(); ++cut) {
        ScanResult r;
        CacheMeta cm;
        if (DeserializeScan(buf.data(), cut, r, cm)) anyPrefixParsed = true;
    }
    CHECK(!anyPrefixParsed, "no truncation parses");

    {   // Trailing bytes mean it is not our file.
        std::vector<uint8_t> t = buf;
        t.push_back(0);
        ScanResult r;
        CacheMeta cm;
        CHECK(!DeserializeScan(t.data(), t.size(), r, cm),
              "trailing garbage refused");
    }
    {   // Wrong magic and wrong version are both someone else's file.
        std::vector<uint8_t> t = buf;
        t[0] ^= 0xFF;
        ScanResult r;
        CacheMeta cm;
        CHECK(!DeserializeScan(t.data(), t.size(), r, cm), "magic checked");
        t = buf;
        t[4] ^= 0xFF;
        CHECK(!DeserializeScan(t.data(), t.size(), r, cm), "version checked");
    }
    {   // A root that claims to be a file is structurally meaningless.
        // 88-byte header (magic, version, meta, four totals, four
        // hardlink/cloud counters, node count), then the root record's
        // 2-byte nameLen, then its flags byte.
        constexpr size_t kFlagsByte = 88 + 2;
        std::vector<uint8_t> t = buf;
        t[kFlagsByte] = 0;   // clear the directory bit
        ScanResult r;
        CacheMeta cm;
        CHECK(!DeserializeScan(t.data(), t.size(), r, cm),
              "file-as-root refused");

        // And a flags byte carrying bits the format does not define.
        t = buf;
        t[kFlagsByte] = 0xF8;
        CHECK(!DeserializeScan(t.data(), t.size(), r, cm),
              "undefined flag bits refused");
    }

    // Flip every byte in turn: any answer is fine, crashing is not. The
    // sanitisers turn a stray read into a failure here.
    for (size_t i = 0; i < buf.size(); ++i) {
        std::vector<uint8_t> t = buf;
        t[i] ^= 0xFF;
        ScanResult r;
        CacheMeta cm;
        static_cast<void>(DeserializeScan(t.data(), t.size(), r, cm));
    }
    CHECK(true, "byte-flip sweep completed without incident");

    ScanResult r;
    CacheMeta cm;
    CHECK(!DeserializeScan(nullptr, 0, r, cm), "null input refused");

    // A real cache's root is named for the volume - "D:\\" - which is a
    // path, not a path component. Validating it as a component rejected
    // every cache the program wrote, and because an unreadable cache is
    // deleted and rescanned the only symptom was that caching silently
    // stopped working. The fixtures above use an empty root name, so they
    // could never have caught it.
    {
        ScanResult vol;
        vol.root = MakeDir(L"D:\\", {
            MakeDir(L"Games", {MakeFile(L"a.pak", 4096)}),
            MakeFile(L"top.bin", 2048),
        });
        vol.stats.bytes = vol.root.size;

        std::vector<uint8_t> vb;
        SerializeScan(vol, m, vb);
        ScanResult back;
        CacheMeta bm;
        CHECK(DeserializeScan(vb.data(), vb.size(), back, bm),
              "a cache rooted at a volume path round-trips");
        CHECK(Narrow(back.root.name) == "D:\\", "root keeps its name");
        CHECK(back.root.children.size() == 2, "and its children");

        // The exemption is the root only: a child may not be a path.
        ScanResult evil;
        evil.root = MakeDir(L"D:\\", {MakeFile(L"..\\..\\Windows", 10)});
        SerializeScan(evil, m, vb);
        CHECK(!DeserializeScan(vb.data(), vb.size(), back, bm),
              "a child carrying a separator is still refused");
    }

    // A hand-built cache, which is what an attacker writes: the file lives
    // in the user's profile and is read by an elevated process. The
    // byte-flip sweep above starts from a valid file and structurally
    // cannot express either of these shapes.
    {
        // Header, then a chain of directories each owning one child. Node
        // owns its children by value, so the compiler-generated destructor
        // recurses - a tree deeper than the stack cannot be freed, and that
        // crash is not catchable. Depth must be refused on the way in.
        auto Put = [](std::vector<uint8_t>& v, uint64_t x, int bytes) {
            for (int i = 0; i < bytes; ++i) {
                v.push_back(static_cast<uint8_t>(x >> (i * 8)));
            }
        };
        const uint64_t depth = 100000;
        std::vector<uint8_t> deep;
        Put(deep, 0x434E5053, 4);   // magic
        Put(deep, 2, 4);            // version
        Put(deep, 0, 8);            // savedUnixMs
        Put(deep, 0, 4);            // volumeSerial
        Put(deep, 0, 4);            // reserved
        Put(deep, 0, 8);            // bytes
        Put(deep, 0, 8);            // fileCount
        Put(deep, 0, 8);            // dirCount
        Put(deep, 0, 8);            // hardlinkFiles
        Put(deep, 0, 8);            // hardlinkBytes
        Put(deep, 0, 8);            // cloudFiles
        Put(deep, 0, 8);            // cloudBytes
        Put(deep, depth, 8);        // nodeCount
        for (uint64_t i = 0; i < depth; ++i) {
            Put(deep, 0, 2);                        // nameLen
            deep.push_back(1);                      // flags: directory
            deep.push_back(0);                      // cat
            Put(deep, 0, 8);                        // size
            Put(deep, 0, 4);                        // files
            Put(deep, (i + 1 < depth) ? 1 : 0, 4);  // childCount
        }
        ScanResult deepOut;
        CacheMeta deepMeta;
        CHECK(!DeserializeScan(deep.data(), deep.size(), deepOut, deepMeta),
              "a 100000-deep chain is refused, not built and then freed");
    }
    {
        // Every directory in a chain claiming an enormous child count. Each
        // live frame holds its parent's reserved vector, so without a
        // running total this reserves gigabytes from a small file and the
        // bad_alloc lands on a thread with no handler.
        auto Put = [](std::vector<uint8_t>& v, uint64_t x, int bytes) {
            for (int i = 0; i < bytes; ++i) {
                v.push_back(static_cast<uint8_t>(x >> (i * 8)));
            }
        };
        std::vector<uint8_t> greedy;
        Put(greedy, 0x434E5053, 4);
        Put(greedy, 2, 4);
        for (int i = 0; i < 10; ++i) Put(greedy, 0, 8);   // meta + counters
        Put(greedy, 0, 4);
        Put(greedy, 4, 8);      // nodeCount says four...
        for (int i = 0; i < 3; ++i) {
            Put(greedy, 0, 2);
            greedy.push_back(1);
            greedy.push_back(0);
            Put(greedy, 0, 8);
            Put(greedy, 0, 4);
            Put(greedy, 1000000000u, 4);   // ...each record claims a billion
        }
        greedy.resize(greedy.size() + 4096, 0);   // room so the per-record
                                                  // byte bound is satisfied
        ScanResult g;
        CacheMeta gm;
        CHECK(!DeserializeScan(greedy.data(), greedy.size(), g, gm),
              "child counts exceeding the declared node total are refused");
    }

    // An empty-but-valid tree: bare root, nothing else.
    ScanResult bare;
    bare.root = MakeDir(L"", {});
    SerializeScan(bare, m, buf);
    CHECK(DeserializeScan(buf.data(), buf.size(), out, om),
          "bare root round-trips");
    CHECK(out.root.children.empty(), "bare root stays bare");
}

// -------------------------------------------------------------- duplicates

static Digest HashOf(const std::string& s) {
    Hasher h;
    h.Update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    return h.Finish();
}

static void TestHasher() {
    std::printf("Content hashing\n");

    CHECK(HashOf("") == HashOf(""), "empty is stable");
    CHECK(HashOf("hello") == HashOf("hello"), "same input, same digest");
    CHECK(HashOf("hello") != HashOf("hellp"), "one bit apart differs");
    CHECK(HashOf("hello") != HashOf("olleh"), "order matters");
    // std::string("\0") is empty - the length has to be given explicitly.
    CHECK(HashOf("") != HashOf(std::string("\0", 1)), "length is folded in");
    CHECK(HashOf(std::string(16, '\0')) != HashOf(std::string(32, '\0')),
          "runs of zeros of different length differ");

    // Chunking must not change the answer: files are read a megabyte at a
    // time and the boundaries fall wherever they fall.
    const std::string data(5000, 'x');
    Hasher whole;
    whole.Update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    Hasher split;
    for (size_t off = 0; off < data.size(); off += 7) {
        const size_t n = std::min<size_t>(7, data.size() - off);
        split.Update(reinterpret_cast<const uint8_t*>(data.data() + off), n);
    }
    CHECK(whole.Finish() == split.Finish(), "chunking is irrelevant");

    Hasher nul;
    nul.Update(nullptr, 100);
    CHECK(nul.Finish() == Hasher().Finish(), "null feed is ignored");

    // A weak mixer collides trivially on near-identical short inputs; sweep
    // a few thousand and insist they stay distinct.
    std::vector<Digest> seen;
    for (int i = 0; i < 4000; ++i) {
        seen.push_back(HashOf("file_" + std::to_string(i) + ".bin"));
    }
    std::sort(seen.begin(), seen.end());
    const size_t before = seen.size();
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    CHECK(seen.size() == before, "4000 similar strings, no collisions");
}

static void TestDuplicates() {
    std::printf("Duplicate detection\n");

    // Three 100-byte files, two 200-byte files, one lonely 50.
    // Three 100s waste 200; the 500 pair wastes 500. Deliberately not a tie,
    // so the "biggest saving first" ordering is actually being tested.
    Node root = MakeDir(L"", {
        MakeDir(L"a", {MakeFile(L"one.bin", 100), MakeFile(L"two.bin", 500)}),
        MakeDir(L"b", {MakeFile(L"three.bin", 100),
                       MakeFile(L"four.bin", 500),
                       MakeFile(L"lonely.bin", 50)}),
        MakeFile(L"five.bin", 100),
    });

    // minSize 0 so the tiny fixtures qualify.
    std::vector<DupFile> cand = DuplicateCandidates(root, 0);
    CHECK(cand.size() == 5, "only files sharing a size are candidates");
    bool sawLonely = false;
    for (const DupFile& f : cand) {
        if (f.node->name == L"lonely.bin") sawLonely = true;
    }
    CHECK(!sawLonely, "a unique size is never read");

    // The size floor keeps small files out entirely.
    CHECK(DuplicateCandidates(root, 150).size() == 2, "minSize respected");
    CHECK(DuplicateCandidates(root, 100000).empty(), "high floor finds none");

    // Paths must be relative to the subtree, as the caller will join them.
    for (const DupFile& f : cand) {
        if (f.node->name == L"three.bin") {
            CHECK(Narrow(f.path) == "b\\three.bin", "relative path built");
        }
    }

    // Hardlinks and cloud placeholders are excluded: neither is a duplicate
    // that deleting would recover anything from.
    Node tricky = MakeDir(L"", {
        MakeFile(L"real.bin", 100), MakeFile(L"copy.bin", 100),
        MakeFile(L"linked.bin", 100), MakeFile(L"cloudy.bin", 100),
    });
    tricky.children[2].hardlink  = true;
    tricky.children[3].cloudOnly = true;
    CHECK(DuplicateCandidates(tricky, 0).size() == 2,
          "hardlink and cloud placeholder excluded");

    // Grouping: give matching digests to the two 100s, distinct to the rest.
    std::vector<DupFile> hashed = DuplicateCandidates(root, 0);
    for (DupFile& f : hashed) {
        if (f.size == 100) {
            f.digest = HashOf("same-100");
        } else {
            f.digest = HashOf(Narrow(f.path));
        }
    }
    std::vector<DupGroup> groups = GroupByDigest(hashed);
    CHECK(groups.size() == 1, "one confirmed group");
    CHECK(groups[0].files.size() == 3, "all three 100-byte files matched");
    CHECK(groups[0].size == 100, "group size recorded");
    CHECK(groups[0].wasted == 200, "keeping one copy, 200 recoverable");

    // Same size but different content must not group.
    for (DupFile& f : hashed) f.digest = HashOf(Narrow(f.path));
    CHECK(GroupByDigest(hashed).empty(),
          "same size, different content is not a duplicate");

    // Groups come back biggest-saving first.
    std::vector<DupFile> mixed = DuplicateCandidates(root, 0);
    for (DupFile& f : mixed) {
        f.digest = (f.size == 100) ? HashOf("g100") : HashOf("g500");
    }
    std::vector<DupGroup> ordered = GroupByDigest(mixed);
    CHECK(ordered.size() == 2, "two groups");
    CHECK(ordered[0].wasted >= ordered[1].wasted, "sorted by saving");
    CHECK(ordered[0].wasted == 500 && ordered[0].size == 500,
          "the 500-byte pair wastes most, and leads");
    CHECK(ordered[1].wasted == 200, "the 100-byte trio follows");

    CHECK(GroupByDigest({}).empty(), "no candidates, no groups");

    // Pooling across drives. Each tree holds one copy, so within its own
    // tree that file's size is unique - filtering per tree would discard
    // exactly the pair worth finding.
    Node driveD = MakeDir(L"D:\\", {MakeFile(L"asset.bin", 5000)});
    Node driveE = MakeDir(L"E:\\", {MakeFile(L"asset.bin", 5000)});

    CHECK(DuplicateCandidatesIn(driveD, L"D:\\", 0).empty(),
          "one copy alone in its own tree is not a candidate");

    std::vector<DupFile> pool = CollectDupFiles(driveD, L"D:\\", 0);
    for (DupFile& f : CollectDupFiles(driveE, L"E:\\", 0)) {
        pool.push_back(f);
    }
    CHECK(pool.size() == 2, "both drives contribute before filtering");

    const std::vector<DupFile> pooled = FilterBySharedSize(pool);
    CHECK(pooled.size() == 2,
          "the cross-drive pair survives the pooled filter");

    // And each still knows which drive it is on, or the result would be
    // two identical-looking rows.
    bool sawD = false, sawE = false;
    for (const DupFile& f : pooled) {
        if (Narrow(f.Full()) == "D:\\asset.bin") sawD = true;
        if (Narrow(f.Full()) == "E:\\asset.bin") sawE = true;
    }
    CHECK(sawD && sawE, "each file reports its own volume");

    // A genuinely unique size is still discarded, pooled or not.
    Node lone = MakeDir(L"F:\\", {MakeFile(L"only.bin", 999)});
    std::vector<DupFile> withLone = pool;
    for (DupFile& f : CollectDupFiles(lone, L"F:\\", 0)) {
        withLone.push_back(f);
    }
    CHECK(FilterBySharedSize(withLone).size() == 2,
          "a unique size is dropped from the pool");
}

// ------------------------------------------------------------ command line

static CommandLine Parse(std::vector<const wchar_t*> argv) {
    std::vector<std::wstring> args;
    for (const wchar_t* a : argv) args.push_back(a);
    return ParseCommandLine(args);
}

static void TestCommandLine() {
    std::printf("Command line\n");

    const CommandLine none = Parse({});
    CHECK(none.valid && none.mode == CommandLine::Mode::Gui,
          "no arguments opens the window");
    CHECK(none.path.empty(), "and picks no drive");

    const CommandLine path = Parse({L"D:\\"});
    CHECK(path.valid && path.mode == CommandLine::Mode::Gui, "bare path");
    CHECK(Narrow(path.path) == "D:\\", "path kept");

    const CommandLine unc = Parse({L"\\\\server\\share"});
    CHECK(unc.valid && Narrow(unc.path) == "\\\\server\\share",
          "UNC path accepted");

    CHECK(Parse({L"--help"}).mode == CommandLine::Mode::Help, "--help");
    CHECK(Parse({L"--version"}).mode == CommandLine::Mode::Version,
          "--version");

    const CommandLine csv = Parse({L"--csv", L"out.csv", L"D:\\"});
    CHECK(csv.valid && csv.mode == CommandLine::Mode::Export, "--csv mode");
    CHECK(Narrow(csv.csvOut) == "out.csv", "csv target");
    CHECK(Narrow(csv.path) == "D:\\", "and the path");

    CHECK(!Parse({L"--csv"}).valid, "--csv with no file is refused");
    CHECK(!Parse({L"--csv", L"out.csv"}).valid,
          "--csv with no path to scan is refused");
    CHECK(!Parse({L"--nonsense"}).valid, "unknown option refused");
    CHECK(!Parse({L"C:\\", L"D:\\"}).valid, "two paths refused");

    // --duplicates takes an optional size, and must not swallow a path.
    const CommandLine d1 = Parse({L"--duplicates", L"D:\\"});
    CHECK(d1.valid && d1.wantDuplicates, "--duplicates alone");
    CHECK(Narrow(d1.path) == "D:\\", "path not eaten by --duplicates");
    CHECK(d1.minDup == kDefaultDupMinSize, "default threshold applied");

    const CommandLine d2 = Parse({L"--duplicates", L"4096", L"D:\\"});
    CHECK(d2.valid && d2.minDup == 4096, "explicit threshold");
    CHECK(Narrow(d2.path) == "D:\\", "path still found after a number");

    // A threshold too large to represent must not wrap into something small.
    const CommandLine huge =
        Parse({L"--duplicates", L"99999999999999999999999", L"D:\\"});
    CHECK(huge.valid && huge.minDup == kDefaultDupMinSize,
          "overflowing threshold falls back to the default");

    // Anything dash-led is a mistyped flag, never a path to scan.
    CHECK(!Parse({L"--"}).valid, "bare -- refused");
    CHECK(!Parse({L"-csv", L"out.csv"}).valid, "single-dash flag refused");
    CHECK(!Parse({L"-"}).valid, "bare - refused");
}

// -------------------------------------------------------------- comparison

static int64_t SignedDeltaExpected(uint64_t before, uint64_t after) {
    return (after >= before) ? static_cast<int64_t>(after - before)
                             : -static_cast<int64_t>(before - after);
}

static const Change* FindChange(const DiffReport& r, const char* path) {
    for (const Change& c : r.changes) {
        if (Narrow(c.path) == path) return &c;
    }
    return nullptr;
}

static void TestDiff() {
    std::printf("Scan comparison\n");

    Node before = MakeDir(L"", {
        MakeDir(L"games", {MakeFile(L"a.pak", 1000)}),
        MakeDir(L"docs",  {MakeFile(L"old.txt", 500)}),
        MakeFile(L"steady.bin", 700),
    });
    Node after = MakeDir(L"", {
        MakeDir(L"games", {MakeFile(L"a.pak", 3000)}),      // grew 2000
        MakeDir(L"media", {MakeFile(L"new.mp4", 4000)}),    // added
        MakeFile(L"steady.bin", 700),                       // unchanged
    });                                                     // docs removed

    const DiffReport r = DiffTrees(before, after, 1);

    const Change* grown = FindChange(r, "games\\a.pak");
    CHECK(grown != nullptr, "growth found");
    if (grown) {
        CHECK(grown->kind == ChangeKind::Grown, "classified as grown");
        CHECK(grown->delta == 2000, "delta is the increase");
        CHECK(grown->before == 1000 && grown->after == 3000, "both sizes");
    }

    const Change* added = FindChange(r, "media");
    CHECK(added != nullptr, "addition found at its root");
    if (added) {
        CHECK(added->kind == ChangeKind::Added, "classified as added");
        CHECK(added->delta == 4000, "whole subtree counted");
        CHECK(added->dir, "recorded as a directory");
    }
    CHECK(FindChange(r, "media\\new.mp4") == nullptr,
          "does not also list what is inside a new folder");

    const Change* removed = FindChange(r, "docs");
    CHECK(removed != nullptr, "removal found");
    if (removed) {
        CHECK(removed->kind == ChangeKind::Removed, "classified as removed");
        CHECK(removed->delta == -500, "loss is negative");
    }

    CHECK(FindChange(r, "steady.bin") == nullptr, "unchanged file omitted");

    // Largest movement first.
    CHECK(!r.changes.empty() && r.changes[0].delta == 4000,
          "biggest change leads");

    CHECK(r.grewBy == 6000, "total growth");
    CHECK(r.shrankBy == 500, "total loss");
    CHECK(r.netDelta == SignedDeltaExpected(before.size, after.size),
          "net matches the roots");

    // The threshold hides noise.
    const DiffReport coarse = DiffTrees(before, after, 3000);
    CHECK(coarse.changes.size() == 1, "minDelta filters small movement");
    CHECK(coarse.changes[0].delta == 4000, "only the big one survives");

    // Identical trees produce nothing at all.
    CHECK(DiffTrees(before, before, 1).changes.empty(),
          "no change between a tree and itself");
    CHECK(DiffTrees(before, before, 1).netDelta == 0, "net zero");

    // A file replaced by a directory of the same name still reports.
    Node fileVer = MakeDir(L"", {MakeFile(L"thing", 100)});
    Node dirVer  = MakeDir(L"", {MakeDir(L"thing", {MakeFile(L"x", 900)})});
    const DiffReport swap = DiffTrees(fileVer, dirVer, 1);
    CHECK(!swap.changes.empty(), "file becoming a directory is reported");

    // Deep nesting must not recurse.
    Node deepA = MakeFile(L"leaf", 1);
    Node deepB = MakeFile(L"leaf", 2);
    for (int i = 0; i < 2000; ++i) {
        deepA = MakeDir(L"d", {std::move(deepA)});
        deepB = MakeDir(L"d", {std::move(deepB)});
    }
    CHECK(!DiffTrees(deepA, deepB, 1).changes.empty(),
          "2000 levels deep without a stack overflow");
}

// ---------------------------------------------------------- force removal

static void TestForceRemovalGuards() {
    std::printf("Force removal guards\n");

    // Volume roots and anything that is not a plain drive-letter path.
    CHECK(IsProtectedSystemPath(L""), "empty refused");
    CHECK(IsProtectedSystemPath(L"C:\\"), "drive root refused");
    CHECK(IsProtectedSystemPath(L"C:"), "bare drive refused");
    CHECK(IsProtectedSystemPath(L"D:\\"), "any drive root refused");
    CHECK(IsProtectedSystemPath(L"\\\\?\\C:\\"), "extended root refused");
    CHECK(IsProtectedSystemPath(L"\\\\server\\share\\x"), "UNC refused");
    CHECK(IsProtectedSystemPath(L"\\\\?\\UNC\\server\\share"),
          "extended UNC refused");

    // The directories whose loss ends the machine.
    CHECK(IsProtectedSystemPath(L"C:\\Windows"), "Windows refused");
    CHECK(IsProtectedSystemPath(L"C:\\Windows\\System32"),
          "inside Windows refused");
    CHECK(IsProtectedSystemPath(L"c:\\wInDoWs\\system32\\drivers"),
          "case-insensitive");
    CHECK(IsProtectedSystemPath(L"C:\\Windows\\"), "trailing slash handled");
    // Container roots: the directory itself is refused, because losing it
    // whole is catastrophic.
    CHECK(IsProtectedSystemPath(L"C:\\Program Files"),
          "Program Files root refused");
    CHECK(IsProtectedSystemPath(L"C:\\Program Files (x86)"),
          "Program Files (x86) root refused");
    CHECK(IsProtectedSystemPath(L"C:\\ProgramData"),
          "ProgramData root refused");
    CHECK(IsProtectedSystemPath(L"C:\\Users"), "Users root refused");
    CHECK(IsProtectedSystemPath(L"c:\\users\\"),
          "Users root refused with trailing slash");

    // ...but their contents are the whole point of the feature: the
    // abandoned application folder is what force removal is for.
    CHECK(!IsProtectedSystemPath(L"C:\\Program Files\\DeadApp"),
          "leftover app folder allowed");
    CHECK(!IsProtectedSystemPath(L"C:\\Program Files (x86)\\DeadApp\\bin"),
          "deep inside Program Files allowed");
    CHECK(!IsProtectedSystemPath(L"C:\\ProgramData\\DeadApp"),
          "leftover ProgramData allowed");
    CHECK(!IsProtectedSystemPath(L"C:\\Users\\sam"),
          "a whole profile is allowed (it is the user's own)");
    CHECK(IsProtectedSystemPath(L"D:\\System Volume Information"),
          "SVI refused on any volume");
    CHECK(IsProtectedSystemPath(L"C:\\pagefile.sys"), "pagefile refused");
    CHECK(IsProtectedSystemPath(L"C:\\hiberfil.sys"), "hiberfil refused");

    // The whole point is that ordinary user data is still removable, and
    // that a prefix match is not a path match.
    CHECK(!IsProtectedSystemPath(L"C:\\Users\\sam\\Downloads\\big.iso"),
          "a file in a profile is allowed");
    CHECK(!IsProtectedSystemPath(L"D:\\Games\\Helldivers 2"),
          "ordinary folder allowed");
    CHECK(!IsProtectedSystemPath(L"C:\\WindowsApps"),
          "WindowsApps is not Windows");
    CHECK(!IsProtectedSystemPath(L"D:\\My Windows Backup"),
          "substring is not a match");
    CHECK(!IsProtectedSystemPath(L"C:\\Program Files Backup"),
          "Program Files Backup allowed");
    CHECK(!IsProtectedSystemPath(L"\\\\?\\D:\\Media\\video.mkv"),
          "extended prefix stripped, then allowed");

    // Spellings that Win32 resolves differently than a string compare reads.
    // Each of these once reached the deletion walk pointing at something the
    // comparisons below never saw.
    CHECK(IsProtectedSystemPath(L"C:\\..."), "dot-run component refused");
    CHECK(IsProtectedSystemPath(L"C:\\Windows "),
          "trailing space refused (Win32 trims it)");
    CHECK(IsProtectedSystemPath(L"D:\\System Volume Information "),
          "trailing space on a decoy refused");
    CHECK(IsProtectedSystemPath(L"C:\\Windows."), "trailing dot refused");
    CHECK(IsProtectedSystemPath(L"C:/Windows/System32"),
          "forward slashes refused");
    CHECK(IsProtectedSystemPath(L"C:\\..\\..\\Windows"),
          "parent traversal refused");
    CHECK(IsProtectedSystemPath(L"C:\\.\\Windows"), "dot component refused");
    CHECK(IsProtectedSystemPath(L"C:\\PROGRA~1"), "8.3 alias refused");
    CHECK(IsProtectedSystemPath(L"D:\\SYSTEM~1"), "8.3 alias on any volume");
    CHECK(IsProtectedSystemPath(L"C:Windows\\System32"),
          "drive-relative refused");
    CHECK(IsProtectedSystemPath(std::wstring(L"C:\\bootmgr\0aaa", 13)),
          "embedded NUL refused");
    CHECK(IsProtectedSystemPath(L"C:\\a\"b"), "embedded quote refused");
    CHECK(IsProtectedSystemPath(L"abc"), "non-drive path refused");

    // ...while ordinary paths with dots in them are still fine.
    CHECK(!IsProtectedSystemPath(L"D:\\Games\\v1.2.3\\data.pak"),
          "dots inside components are fine");
    CHECK(!IsProtectedSystemPath(L"D:\\my.folder\\file.bin"),
          "dotted folder allowed");

    // Node names become path components, so a name that is not one at all
    // must be refused where it enters.
    CHECK(IsSafeNodeName(L"ordinary.txt"), "ordinary name accepted");
    CHECK(IsSafeNodeName(L"with spaces and (punctuation)!"), "punctuation ok");
    CHECK(!IsSafeNodeName(L""), "empty name refused");
    CHECK(!IsSafeNodeName(L"."), "dot refused");
    CHECK(!IsSafeNodeName(L".."), "dotdot refused");
    CHECK(!IsSafeNodeName(L"..\\..\\Windows"), "separator refused");
    CHECK(!IsSafeNodeName(L"a/b"), "forward slash refused");
    CHECK(!IsSafeNodeName(L"name:stream"), "colon refused (ADS)");
    CHECK(!IsSafeNodeName(std::wstring(L"boot\0evil", 9)), "NUL refused");
    CHECK(!IsSafeNodeName(L"trailing "), "trailing space refused");
    CHECK(!IsSafeNodeName(L"trailing."), "trailing dot refused");

    // Processes that must never be killed to break a lock.
    CHECK(IsCriticalProcess(L"anything.exe", 0), "pid 0 critical");
    CHECK(IsCriticalProcess(L"anything.exe", 4), "pid 4 critical");
    CHECK(IsCriticalProcess(L"csrss.exe", 900), "csrss critical");
    CHECK(IsCriticalProcess(L"C:\\Windows\\System32\\lsass.exe", 901),
          "full path stripped to leaf");
    CHECK(IsCriticalProcess(L"WinLogon.EXE", 902), "case-insensitive");
    CHECK(IsCriticalProcess(L"services.exe", 903), "services critical");
    CHECK(IsCriticalProcess(L"spindle.exe", 904), "never kills itself");
    CHECK(IsCriticalProcess(L"", 905), "unnamed treated as critical");

    // ...and the ordinary programs that legitimately hold files open.
    CHECK(!IsCriticalProcess(L"notepad.exe", 1200), "notepad killable");
    CHECK(!IsCriticalProcess(L"steam.exe", 1201), "steam killable");
    CHECK(!IsCriticalProcess(L"C:\\Games\\game.exe", 1202),
          "game killable by full path");
}

// --------------------------------------------------------------- settings

static void TestSettings() {
    std::printf("Settings\n");

    const Settings d;
    CHECK(d.keepCaches && d.resumeOnLaunch && d.prefetchAll,
          "defaults are on");
    CHECK(ParseSettings(nullptr, 0).keepCaches, "null input keeps defaults");

    const char* junk = "\xFF\xFE not a settings file == = \n\n=";
    const Settings j =
        ParseSettings(reinterpret_cast<const uint8_t*>(junk), strlen(junk));
    CHECK(j.keepCaches && j.resumeOnLaunch, "garbage keeps defaults");

    Settings s;
    std::vector<uint8_t> buf;
    s.keepCaches = false;
    s.resumeOnLaunch = true;
    SerializeSettings(s, buf);
    Settings r = ParseSettings(buf.data(), buf.size());
    CHECK(!r.keepCaches && r.resumeOnLaunch, "round-trips");

    s.keepCaches = true;
    s.resumeOnLaunch = false;
    SerializeSettings(s, buf);
    r = ParseSettings(buf.data(), buf.size());
    CHECK(r.keepCaches && !r.resumeOnLaunch, "round-trips the other way");

    s.prefetchAll = false;
    SerializeSettings(s, buf);
    r = ParseSettings(buf.data(), buf.size());
    CHECK(!r.prefetchAll, "prefetch_all round-trips off");
    CHECK(ParseSettings(nullptr, 0).prefetchAll,
          "prefetch_all defaults on for an old settings file");

    const char* mixed = "unknown_key=7\r\nkeep_caches=0\r\nfuture=stuff\n";
    r = ParseSettings(reinterpret_cast<const uint8_t*>(mixed), strlen(mixed));
    CHECK(!r.keepCaches && r.resumeOnLaunch,
          "unknown keys ignored, CRLF handled");

    const std::vector<uint8_t> big(kMaxSettingsBytes + 1, 'x');
    r = ParseSettings(big.data(), big.size());
    CHECK(r.keepCaches, "oversize input keeps defaults");
}

int main() {
    std::printf("\n=== Spindle core tests ===\n\n");

    TestEasing();
    TestFormatSize();
    TestFormatCount();
    TestCategories();
    TestSanitize();
    TestSatAdd();
    TestTreemapBounds();
    TestTreemapProportionality();
    TestTreemapNoOverlap();
    TestTreemapAspectRatio();
    TestTreemapDeepNesting();
    TestTreemapCellCap();
    TestZeroAndDegenerate();
    TestHitTest();
    TestExtensionBreakdown();
    TestLargestFiles();
    TestFindByName();
    TestQueryParsing();
    TestQueryMatching();
    FuzzQueries();
    TestCsvExport();
    TestReportsOnHostileTrees();
    TestCellChainPaths();
    TestHitTestIndexAgreement();
    TestLabelStripClear();
    TestExpandedFlagConsistency();
    FuzzSanitize();
    FuzzTreemap();
    TestScanCache();
    TestSettings();
    TestForceRemovalGuards();
    TestHasher();
    TestDuplicates();
    TestDiff();
    TestCommandLine();

    std::printf("\n=== %d passed, %d failed ===\n\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
