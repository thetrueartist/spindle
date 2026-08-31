// Spindle - platform-independent core.
// Squarified treemap layout, file categorisation and display formatting.
// Deliberately free of Windows headers so it can be unit-tested on any host.

#include "spindle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <limits>
#include <type_traits>
#include <unordered_map>

namespace spindle {

// ---------------------------------------------------------------- categories

const wchar_t* CatName(Cat c) {
    switch (c) {
        case Cat::Directory:   return L"Folders";
        case Cat::Media:       return L"Media";
        case Cat::Game:        return L"Game data";
        case Cat::Archive:     return L"Archives";
        case Cat::Code:        return L"Source";
        case Cat::Document:    return L"Documents";
        case Cat::Binary:      return L"Programs";
        case Cat::VirtualDisk: return L"Disk images";
        case Cat::Database:    return L"Databases";
        case Cat::System:      return L"System";
        default:               return L"Other";
    }
}

namespace {

struct ExtRule {
    const wchar_t* ext;
    Cat            cat;
};

// Ordered roughly by how often each shows up on a developer's machine.
constexpr ExtRule kExtRules[] = {
    // Media
    {L"mp4", Cat::Media}, {L"mkv", Cat::Media}, {L"avi", Cat::Media},
    {L"mov", Cat::Media}, {L"webm", Cat::Media}, {L"wmv", Cat::Media},
    {L"flv", Cat::Media}, {L"m4v", Cat::Media},
    {L"mp3", Cat::Media}, {L"flac", Cat::Media}, {L"wav", Cat::Media},
    {L"aac", Cat::Media}, {L"ogg", Cat::Media}, {L"m4a", Cat::Media},
    {L"opus", Cat::Media}, {L"aiff", Cat::Media},
    {L"jpg", Cat::Media}, {L"jpeg", Cat::Media}, {L"png", Cat::Media},
    {L"gif", Cat::Media}, {L"bmp", Cat::Media}, {L"webp", Cat::Media},
    {L"tiff", Cat::Media}, {L"heic", Cat::Media}, {L"raw", Cat::Media},
    {L"psd", Cat::Media}, {L"svg", Cat::Media}, {L"ico", Cat::Media},

    // Game / engine asset packages
    {L"pak", Cat::Game}, {L"vpk", Cat::Game}, {L"bsa", Cat::Game},
    {L"ba2", Cat::Game}, {L"asset", Cat::Game}, {L"assets", Cat::Game},
    {L"resource", Cat::Game}, {L"resS", Cat::Game}, {L"uasset", Cat::Game},
    {L"umap", Cat::Game}, {L"sb", Cat::Game}, {L"toc", Cat::Game},
    {L"cas", Cat::Game}, {L"forge", Cat::Game}, {L"bundle", Cat::Game},

    // Archives and installers
    {L"zip", Cat::Archive}, {L"7z", Cat::Archive}, {L"rar", Cat::Archive},
    {L"tar", Cat::Archive}, {L"gz", Cat::Archive}, {L"bz2", Cat::Archive},
    {L"xz", Cat::Archive}, {L"zst", Cat::Archive}, {L"cab", Cat::Archive},
    {L"msi", Cat::Archive}, {L"msix", Cat::Archive}, {L"appx", Cat::Archive},
    {L"nupkg", Cat::Archive}, {L"whl", Cat::Archive}, {L"jar", Cat::Archive},

    // Source and scripts
    {L"c", Cat::Code}, {L"h", Cat::Code}, {L"cpp", Cat::Code},
    {L"hpp", Cat::Code}, {L"cc", Cat::Code}, {L"cs", Cat::Code},
    {L"py", Cat::Code}, {L"js", Cat::Code}, {L"ts", Cat::Code},
    {L"jsx", Cat::Code}, {L"tsx", Cat::Code}, {L"rs", Cat::Code},
    {L"go", Cat::Code}, {L"java", Cat::Code}, {L"kt", Cat::Code},
    {L"rb", Cat::Code}, {L"php", Cat::Code}, {L"lua", Cat::Code},
    {L"sh", Cat::Code}, {L"ps1", Cat::Code}, {L"psm1", Cat::Code},
    {L"bat", Cat::Code}, {L"cmd", Cat::Code}, {L"asm", Cat::Code},
    {L"s", Cat::Code}, {L"json", Cat::Code}, {L"xml", Cat::Code},
    {L"yaml", Cat::Code}, {L"yml", Cat::Code}, {L"toml", Cat::Code},
    {L"html", Cat::Code}, {L"css", Cat::Code}, {L"sql", Cat::Code},

    // Documents
    {L"txt", Cat::Document}, {L"md", Cat::Document}, {L"pdf", Cat::Document},
    {L"doc", Cat::Document}, {L"docx", Cat::Document}, {L"xls", Cat::Document},
    {L"xlsx", Cat::Document}, {L"ppt", Cat::Document}, {L"pptx", Cat::Document},
    {L"csv", Cat::Document}, {L"rtf", Cat::Document}, {L"epub", Cat::Document},

    // Executable / linkable
    {L"exe", Cat::Binary}, {L"dll", Cat::Binary}, {L"sys", Cat::Binary},
    {L"ocx", Cat::Binary}, {L"drv", Cat::Binary}, {L"obj", Cat::Binary},
    {L"lib", Cat::Binary}, {L"o", Cat::Binary}, {L"a", Cat::Binary},
    {L"so", Cat::Binary}, {L"pdb", Cat::Binary}, {L"winmd", Cat::Binary},
    {L"efi", Cat::Binary}, {L"scr", Cat::Binary}, {L"cpl", Cat::Binary},

    // Virtual disks, images, memory dumps
    {L"vmdk", Cat::VirtualDisk}, {L"vhd", Cat::VirtualDisk},
    {L"vhdx", Cat::VirtualDisk}, {L"avhdx", Cat::VirtualDisk},
    {L"vdi", Cat::VirtualDisk}, {L"qcow2", Cat::VirtualDisk},
    {L"iso", Cat::VirtualDisk}, {L"img", Cat::VirtualDisk},
    {L"ova", Cat::VirtualDisk}, {L"vmem", Cat::VirtualDisk},
    {L"dmp", Cat::VirtualDisk}, {L"core", Cat::VirtualDisk},
    {L"hds", Cat::VirtualDisk}, {L"wim", Cat::VirtualDisk},

    // Databases and stores
    {L"db", Cat::Database}, {L"sqlite", Cat::Database},
    {L"sqlite3", Cat::Database}, {L"mdb", Cat::Database},
    {L"accdb", Cat::Database}, {L"ldb", Cat::Database},
    {L"edb", Cat::Database}, {L"pst", Cat::Database},
    {L"ost", Cat::Database}, {L"dat", Cat::Database},
    {L"bin", Cat::Database}, {L"idx", Cat::Database},
};

inline wchar_t LowerAscii(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

}  // namespace

Cat CategoryForFile(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= name.size()) return Cat::Other;

    const size_t extLen = name.size() - dot - 1;
    if (extLen > 8) return Cat::Other;   // no real extension is this long

    wchar_t ext[9] = {};
    for (size_t i = 0; i < extLen; ++i) {
        ext[i] = LowerAscii(name[dot + 1 + i]);
    }

    for (const ExtRule& r : kExtRules) {
        const wchar_t* a = ext;
        const wchar_t* b = r.ext;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (*a == 0 && *b == 0) return r.cat;
    }
    return Cat::Other;
}

// ------------------------------------------------------------------ treemap

namespace {

// Label strip geometry. A cell shorter than 2.6x the strip would be almost
// entirely header, so below that the parent simply yields its label to the
// children drawn over it.
constexpr float kHeaderH    = 16.0f;
constexpr float kHeaderMinW = 58.0f;

struct Item {
    const Node* node = nullptr;
    double      area = 0.0;
};

// Worst aspect ratio produced by laying `row` along a side of length `side`.
// Bruls, Huizing & van Wijk, "Squarified Treemaps" (2000), section 3.
double WorstRatio(double sum, double rmax, double rmin, double side) {
    if (sum <= 0.0 || side <= 0.0 || rmin <= 0.0) {
        return std::numeric_limits<double>::max();
    }
    const double s2 = sum * sum;
    const double d2 = side * side;
    return std::max((d2 * rmax) / s2, s2 / (d2 * rmin));
}

void PlaceRow(const Item* first, size_t count, double rowSum,
              Rect& remaining, bool vertical,
              std::vector<std::pair<const Node*, Rect>>& out) {
    if (count == 0 || rowSum <= 0.0) return;

    const double side = vertical ? remaining.h : remaining.w;
    if (side <= 0.0) return;

    const double thickness = rowSum / side;
    double cursor = vertical ? remaining.y : remaining.x;

    for (size_t i = 0; i < count; ++i) {
        const double extent = first[i].area / thickness;
        Rect r;
        if (vertical) {
            r.x = remaining.x;
            r.y = static_cast<float>(cursor);
            r.w = static_cast<float>(thickness);
            r.h = static_cast<float>(extent);
        } else {
            r.x = static_cast<float>(cursor);
            r.y = remaining.y;
            r.w = static_cast<float>(extent);
            r.h = static_cast<float>(thickness);
        }
        out.emplace_back(first[i].node, r);
        cursor += extent;
    }

    if (vertical) {
        remaining.x += static_cast<float>(thickness);
        remaining.w -= static_cast<float>(thickness);
    } else {
        remaining.y += static_cast<float>(thickness);
        remaining.h -= static_cast<float>(thickness);
    }
}

// Lay `items` (sorted descending, areas already scaled to fill `bounds`) out
// as a squarified treemap.
void Squarify(std::vector<Item>& items, Rect bounds,
              std::vector<std::pair<const Node*, Rect>>& out) {
    Rect remaining = bounds;
    size_t i = 0;

    while (i < items.size()) {
        if (remaining.w <= 0.5f || remaining.h <= 0.5f) break;

        const bool vertical = remaining.w >= remaining.h;
        const double side = vertical ? remaining.h : remaining.w;

        const size_t rowStart = i;
        double rowSum = 0.0;
        double rmin = std::numeric_limits<double>::max();
        double rmax = 0.0;
        double bestWorst = std::numeric_limits<double>::max();

        while (i < items.size()) {
            const double v = items[i].area;
            if (v <= 0.0) { ++i; continue; }

            const double nSum = rowSum + v;
            const double nMin = std::min(rmin, v);
            const double nMax = std::max(rmax, v);
            const double w = WorstRatio(nSum, nMax, nMin, side);

            // Always take at least one item, else a single huge item that
            // cannot improve the ratio would stall the loop forever.
            if (i == rowStart || w <= bestWorst) {
                rowSum = nSum;
                rmin = nMin;
                rmax = nMax;
                bestWorst = w;
                ++i;
            } else {
                break;
            }
        }

        if (i == rowStart) break;   // defensive: no progress, bail out
        PlaceRow(items.data() + rowStart, i - rowStart, rowSum, remaining,
                 vertical, out);
    }
}

// Recursive, but bounded twice over: by maxDepth (the caller passes 5) and
// by the inner-rect size test below, so the frame count is a small constant
// regardless of how deeply the scanned tree actually nests.
void BuildRecursive(const Node& node, Rect bounds, int depth, int maxDepth,
                    float minArea, int parentIndex, std::vector<Cell>& out) {
    if (depth > maxDepth) return;
    if (bounds.w <= 1.0f || bounds.h <= 1.0f) return;
    if (node.children.empty()) return;

    const double boundsArea =
        static_cast<double>(bounds.w) * static_cast<double>(bounds.h);
    if (boundsArea <= 0.0) return;

    // Total of children rather than node.size: a directory's own size should
    // equal the sum of its children, but a partially-denied scan can leave
    // them inconsistent and the layout must use what it is actually drawing.
    double total = 0.0;
    for (const Node& c : node.children) total += static_cast<double>(c.size);
    if (total <= 0.0) return;

    std::vector<Item> items;
    items.reserve(node.children.size());
    const double scale = boundsArea / total;

    for (const Node& c : node.children) {
        const double a = static_cast<double>(c.size) * scale;
        if (a < static_cast<double>(minArea)) continue;   // too small to see
        items.push_back(Item{&c, a});
    }
    if (items.empty()) return;

    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.area > b.area; });

    std::vector<std::pair<const Node*, Rect>> placed;
    placed.reserve(items.size());
    Squarify(items, bounds, placed);

    for (const auto& [child, rect] : placed) {
        const size_t cellIndex = out.size();
        out.push_back(Cell{child, rect, depth, 0.0f, false, parentIndex});

        if (!child->dir || child->children.empty()) continue;

        // Inset so the parent's edge stays visible as a frame. Nested padding
        // is what makes the hierarchy legible at a glance.
        const float pad = (depth == 0) ? 3.0f : 1.0f;

        // Reserve a label strip when the cell is big enough to carry one.
        // Without this the parent's label and its first child's label render
        // within a pixel or two of each other and overlap illegibly.
        const bool roomForHeader =
            rect.h > kHeaderH * 2.6f && rect.w > kHeaderMinW;
        const float header = roomForHeader ? kHeaderH : 0.0f;

        const Rect inner{rect.x + pad,
                         rect.y + pad + header,
                         rect.w - pad * 2.0f,
                         rect.h - pad * 2.0f - header};

        if (inner.w > 3.0f && inner.h > 3.0f) {
            const size_t before = out.size();
            BuildRecursive(*child, inner, depth + 1, maxDepth, minArea,
                           static_cast<int>(cellIndex), out);
            if (out.size() > before) {
                out[cellIndex].header   = header;
                out[cellIndex].expanded = true;
            }
        }
    }
}

}  // namespace

void BuildTreemap(const Node& node, Rect bounds, int maxDepth, float minArea,
                  std::vector<Cell>& out) {
    out.clear();
    if (minArea < 1.0f) minArea = 1.0f;
    BuildRecursive(node, bounds, 0, maxDepth, minArea, -1, out);
}

int HitTestIndex(const std::vector<Cell>& cells, float x, float y) {
    // Cells are emitted parent-before-child, so the deepest match wins.
    int best = -1;
    int bestDepth = -1;
    for (size_t i = 0; i < cells.size(); ++i) {
        if (!cells[i].rect.contains(x, y)) continue;
        if (cells[i].depth >= bestDepth) {
            bestDepth = cells[i].depth;
            best = static_cast<int>(i);
        }
    }
    return best;
}

const Cell* HitTest(const std::vector<Cell>& cells, float x, float y) {
    const int i = HitTestIndex(cells, x, y);
    return (i < 0) ? nullptr : &cells[static_cast<size_t>(i)];
}

std::vector<const Node*> CellChain(const std::vector<Cell>& cells, int index) {
    std::vector<const Node*> chain;
    if (index < 0 || static_cast<size_t>(index) >= cells.size()) return chain;

    // Bounded by the cell count: a corrupted parent link cannot spin here.
    size_t guard = 0;
    int i = index;
    while (i >= 0 && static_cast<size_t>(i) < cells.size() &&
           guard++ <= cells.size()) {
        chain.push_back(cells[static_cast<size_t>(i)].node);
        i = cells[static_cast<size_t>(i)].parent;
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}

// ------------------------------------------------------------------ display

// swprintf returns a negative value on encoding failure and does NOT
// guarantee a terminator in that case. Constructing a std::wstring from the
// raw buffer would then read past its end, so every caller here builds the
// string from the reported length instead of trusting a terminator.
static std::wstring FromBuffer(const wchar_t* buf, int written, size_t cap) {
    if (written < 0 || static_cast<size_t>(written) >= cap) return L"--";
    return std::wstring(buf, static_cast<size_t>(written));
}

std::wstring FormatSize(uint64_t bytes) {
    constexpr double kKB = 1024.0;
    constexpr double kMB = kKB * 1024.0;
    constexpr double kGB = kMB * 1024.0;
    constexpr double kTB = kGB * 1024.0;
    constexpr size_t kCap = 48;

    wchar_t buf[kCap];
    const double b = static_cast<double>(bytes);
    int n = 0;

    if (b >= kTB)      n = std::swprintf(buf, kCap, L"%.2f TB", b / kTB);
    else if (b >= kGB) n = std::swprintf(buf, kCap, L"%.2f GB", b / kGB);
    else if (b >= kMB) n = std::swprintf(buf, kCap, L"%.1f MB", b / kMB);
    else if (b >= kKB) n = std::swprintf(buf, kCap, L"%.1f KB", b / kKB);
    else               n = std::swprintf(buf, kCap, L"%llu B",
                                     static_cast<unsigned long long>(bytes));

    return FromBuffer(buf, n, kCap);
}

std::wstring FormatCount(uint64_t n) {
    constexpr size_t kCap = 32;
    wchar_t buf[kCap];
    const int written =
        std::swprintf(buf, kCap, L"%llu", static_cast<unsigned long long>(n));

    const std::wstring digits = FromBuffer(buf, written, kCap);
    if (digits.empty() || digits == L"--") return digits;
    std::wstring out;
    out.reserve(digits.size() + digits.size() / 3 + 1);

    // Built right-to-left then reversed. Grouping from the left needs an
    // (i - lead) offset, and with size_t operands that underflows to SIZE_MAX
    // whenever lead == 2 and i == 1 -- and SIZE_MAX % 3 == 0, so it emits a
    // separator in the wrong place. Counting from the right has no such edge.
    size_t placed = 0;
    for (size_t i = digits.size(); i-- > 0; ) {
        if (placed > 0 && placed % 3 == 0) out.push_back(L',');
        out.push_back(digits[i]);
        ++placed;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::wstring SanitizeForDisplay(const std::wstring& in, bool* modified) {
    if (modified) *modified = false;

    std::wstring out;
    out.reserve(in.size());

    for (const wchar_t c : in) {
        // Widen through the unsigned counterpart first. wchar_t is unsigned
        // 16-bit on Windows but signed 32-bit elsewhere, and sign-extending a
        // negative value straight to uint32_t produces something above every
        // range test below -- which would let the character through
        // unsanitised on exactly the platforms where wchar_t is signed.
        const uint32_t u =
            static_cast<uint32_t>(static_cast<std::make_unsigned<wchar_t>::type>(c));

        const bool isC0     = (u < 0x20);
        const bool isDel    = (u == 0x7F);
        const bool isC1     = (u >= 0x80 && u <= 0x9F);
        // Bidi embedding/override and isolate controls.
        const bool isBidi   = (u >= 0x202A && u <= 0x202E) ||
                              (u >= 0x2066 && u <= 0x2069);
        // Zero-width and other invisible formatting characters.
        const bool isInvis  = (u == 0x200B) || (u == 0x200C) ||
                              (u == 0x200D) || (u == 0xFEFF) ||
                              (u == 0x00AD);

        if (isC0 || isDel || isC1 || isBidi || isInvis) {
            out.push_back(L'\x2423');   // OPEN BOX, visibly marks the removal
            if (modified) *modified = true;
        } else {
            out.push_back(c);
        }
    }
    return out;
}


// ---------------------------------------------------------------- reporting

namespace {

// Iterative walk shared by the report builders. Recursion here would be
// bounded by directory nesting, which comes off the disk and is therefore
// not ours to trust.
template <typename Fn>
void ForEachNode(const Node& root, Fn&& fn) {
    struct Frame { const Node* node; size_t next; };
    std::vector<Frame> stack;
    stack.push_back(Frame{&root, 0});

    while (!stack.empty()) {
        Frame& f = stack.back();
        if (f.next < f.node->children.size()) {
            const Node* child = &f.node->children[f.next];
            ++f.next;
            fn(*child);
            if (child->dir && !child->children.empty()) {
                stack.push_back(Frame{child, 0});
            }
            continue;
        }
        stack.pop_back();
    }
}

// As above, but supplying the enclosing directory's path so a callback can
// build a full path when it needs one.
//
// The prefix is only extended for directories. Appending each leaf name and
// truncating again costs a copy per file, and on a volume with two million
// files almost all of that work is thrown away: the callers that want a path
// want it for a few dozen rows, not for everything they visit.
template <typename Fn>
void ForEachNodeWithPath(const Node& root, Fn&& fn) {
    struct Frame { const Node* node; size_t next; size_t prefixLen; };
    std::vector<Frame> stack;
    std::wstring prefix;

    stack.push_back(Frame{&root, 0, 0});
    while (!stack.empty()) {
        Frame& f = stack.back();
        if (f.next < f.node->children.size()) {
            const Node* child = &f.node->children[f.next];
            ++f.next;

            fn(*child, prefix);

            if (child->dir && !child->children.empty()) {
                const size_t before = prefix.size();
                if (!prefix.empty()) prefix += L'\\';
                prefix += child->name;
                stack.push_back(Frame{child, 0, before});
            }
            continue;
        }
        prefix.resize(f.prefixLen);
        stack.pop_back();
    }
}

// Joins a directory prefix and a leaf name.
inline std::wstring JoinRel(const std::wstring& prefix,
                            const std::wstring& leaf) {
    if (prefix.empty()) return leaf;
    std::wstring out;
    out.reserve(prefix.size() + 1 + leaf.size());
    out += prefix;
    out += L'\\';
    out += leaf;
    return out;
}

// Locates the extension inside a name and hashes it lower-cased, without
// building a string. Called once per file, so an allocation here is an
// allocation per file on the volume.
struct ExtRef {
    size_t   start = 0;    // index of the first character after the dot
    size_t   len   = 0;
    uint64_t hash  = 0;
};

inline ExtRef ExtRefOf(const std::wstring& name) {
    ExtRef ref;
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot + 1 < name.size() &&
        name.size() - dot - 1 <= 12) {
        ref.start = dot + 1;
        ref.len   = name.size() - dot - 1;
    }

    // FNV-1a over the lower-cased characters. A zero-length extension hashes
    // to the basis, which is a fine key for "no extension".
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < ref.len; ++i) {
        h ^= static_cast<uint64_t>(LowerAscii(name[ref.start + i]));
        h *= 1099511628211ull;
    }
    ref.hash = h;
    return ref;
}

inline std::wstring MaterialiseExt(const std::wstring& name,
                                   const ExtRef& ref) {
    std::wstring ext;
    ext.reserve(ref.len);
    for (size_t i = 0; i < ref.len; ++i) {
        ext.push_back(LowerAscii(name[ref.start + i]));
    }
    return ext;
}

inline bool ExtEquals(const std::wstring& name, const ExtRef& ref,
                      const std::wstring& candidate) {
    if (candidate.size() != ref.len) return false;
    for (size_t i = 0; i < ref.len; ++i) {
        if (LowerAscii(name[ref.start + i]) != candidate[i]) return false;
    }
    return true;
}

}  // namespace

std::vector<ExtStat> ExtensionBreakdown(const Node& root, size_t limit) {
    std::vector<ExtStat> stats;
    stats.reserve(64);

    // Indexed rather than linearly probed. A volume has a few hundred distinct
    // extensions and a million-plus files, so a scan of the table per file is
    // hundreds of millions of string comparisons -- seconds of stall on
    // exactly the volumes where this view is most wanted.
    // Keyed on a hash of the extension rather than on the extension itself,
    // so the common case -- a file whose extension has been seen before --
    // costs no allocation at all. Collisions fall through to a comparison
    // against the stored string, so the result stays exact.
    std::unordered_map<uint64_t, std::vector<size_t>> index;
    index.reserve(256);

    ForEachNode(root, [&](const Node& n) {
        if (n.dir) return;
        const ExtRef ref = ExtRefOf(n.name);

        std::vector<size_t>& bucket = index[ref.hash];
        for (size_t idx : bucket) {
            if (ExtEquals(n.name, ref, stats[idx].ext)) {
                stats[idx].bytes = SatAdd(stats[idx].bytes, n.size);
                ++stats[idx].count;
                return;
            }
        }
        if (stats.size() < 4096) {   // bound what a hostile tree can allocate
            bucket.push_back(stats.size());
            ExtStat e;
            e.ext   = MaterialiseExt(n.name, ref);
            e.bytes = n.size;
            e.count = 1;
            e.cat   = n.cat;
            stats.push_back(std::move(e));
        }
    });

    std::sort(stats.begin(), stats.end(),
              [](const ExtStat& a, const ExtStat& b) {
                  return a.bytes > b.bytes;
              });

    if (limit > 0 && stats.size() > limit) {
        // Fold the tail into one row so the column still sums to the subtree.
        ExtStat rest;
        rest.ext = L"\u2026";
        rest.cat = Cat::Other;
        for (size_t i = limit; i < stats.size(); ++i) {
            rest.bytes = SatAdd(rest.bytes, stats[i].bytes);
            rest.count += stats[i].count;
        }
        stats.resize(limit);
        if (rest.count > 0) stats.push_back(std::move(rest));
    }
    return stats;
}

std::vector<FileHit> LargestFiles(const Node& root, size_t limit) {
    if (limit == 0) return {};

    // Keeps only the current top N rather than sorting every file on the
    // volume: on a million-file tree the difference is decisive.
    std::vector<FileHit> best;
    best.reserve(limit + 1);
    uint64_t floorSize = 0;

    ForEachNodeWithPath(root, [&](const Node& n, const std::wstring& prefix) {
        if (n.dir) return;
        // Cheap rejection before any string work: on a large volume almost
        // every file fails this test, and building its path first would be
        // the dominant cost of the whole walk.
        if (best.size() == limit && n.size <= floorSize) return;

        FileHit hit;
        hit.node = &n;
        hit.path = JoinRel(prefix, n.name);
        hit.size = n.size;

        const auto pos = std::lower_bound(
            best.begin(), best.end(), hit,
            [](const FileHit& a, const FileHit& b) { return a.size > b.size; });
        best.insert(pos, std::move(hit));

        if (best.size() > limit) best.pop_back();
        if (best.size() == limit) floorSize = best.back().size;
    });
    return best;
}

std::vector<FileHit> FindByName(const Node& root, const std::wstring& needle,
                                size_t limit) {
    Query q;
    if (!needle.empty()) {
        std::wstring lowered;
        lowered.reserve(needle.size());
        for (wchar_t c : needle) lowered.push_back(LowerAscii(c));
        q.include.push_back(std::move(lowered));
    }
    return FindMatching(root, q, limit);
}

// CSV export. Quoting follows RFC 4180: fields containing a comma, quote or
// newline are wrapped in quotes and embedded quotes are doubled.
//
// The name is also run through SanitizeForDisplay first. Filenames reach this
// from a scanned volume, and a name containing a bidi override or a control
// character would otherwise be written verbatim into a file that someone opens
// in a spreadsheet -- carrying the spoof out of this program and into theirs.
namespace {

void CsvField(std::wstring& out, const std::wstring& raw) {
    const std::wstring v = SanitizeForDisplay(raw);
    const bool needsQuote =
        v.find(L',') != std::wstring::npos ||
        v.find(L'"') != std::wstring::npos ||
        v.find(L'\n') != std::wstring::npos ||
        v.find(L'\r') != std::wstring::npos;

    if (!needsQuote) { out += v; return; }

    out += L'"';
    for (wchar_t c : v) {
        if (c == L'"') out += L'"';
        out += c;
    }
    out += L'"';
}

}  // namespace

bool ExportCsv(const Node& root, const std::wstring& rootPath,
               const std::wstring& outPath) {
    std::wstring text;
    text.reserve(1u << 20);
    text += L"path,bytes,kind,files\r\n";

    ForEachNodeWithPath(root, [&](const Node& n, const std::wstring& prefix) {
        const std::wstring rel = JoinRel(prefix, n.name);
        std::wstring full = rootPath;
        if (!full.empty() && full.back() != L'\\' && !rel.empty()) {
            full += L'\\';
        }
        full += rel;

        CsvField(text, full);
        text += L',';
        text += std::to_wstring(n.size);
        text += L',';
        CsvField(text, n.dir ? std::wstring(L"folder")
                             : std::wstring(CatName(n.cat)));
        text += L',';
        text += std::to_wstring(n.files);
        text += L"\r\n";
    });

    return WriteTextFileUtf8(outPath, text);
}


// ------------------------------------------------------------------ search

bool CatFromToken(const std::wstring& token, Cat& out) {
    struct Alias { const wchar_t* word; Cat cat; };
    // Several spellings per category: people type what they see in the panel,
    // and also what they would have typed anywhere else.
    static constexpr Alias kAliases[] = {
        {L"folder", Cat::Directory},  {L"folders", Cat::Directory},
        {L"dir", Cat::Directory},
        {L"media", Cat::Media},       {L"video", Cat::Media},
        {L"audio", Cat::Media},       {L"image", Cat::Media},
        {L"images", Cat::Media},      {L"pictures", Cat::Media},
        {L"game", Cat::Game},         {L"games", Cat::Game},
        {L"archive", Cat::Archive},   {L"archives", Cat::Archive},
        {L"zip", Cat::Archive},
        {L"source", Cat::Code},       {L"code", Cat::Code},
        {L"src", Cat::Code},
        {L"document", Cat::Document}, {L"documents", Cat::Document},
        {L"doc", Cat::Document},      {L"docs", Cat::Document},
        {L"program", Cat::Binary},    {L"programs", Cat::Binary},
        {L"exe", Cat::Binary},        {L"binary", Cat::Binary},
        {L"disk", Cat::VirtualDisk},  {L"disks", Cat::VirtualDisk},
        {L"vm", Cat::VirtualDisk},    {L"vms", Cat::VirtualDisk},
        {L"iso", Cat::VirtualDisk},   {L"image-disk", Cat::VirtualDisk},
        {L"database", Cat::Database}, {L"databases", Cat::Database},
        {L"db", Cat::Database},
        {L"system", Cat::System},
        {L"other", Cat::Other},
    };
    for (const Alias& a : kAliases) {
        size_t i = 0;
        while (a.word[i] && i < token.size() &&
               LowerAscii(token[i]) == a.word[i]) {
            ++i;
        }
        if (a.word[i] == 0 && i == token.size()) {
            out = a.cat;
            return true;
        }
    }
    return false;
}

namespace {

std::wstring Lower(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) out.push_back(LowerAscii(c));
    return out;
}

// "500mb", "2 gb", "1024" -> bytes. Returns false on anything else.
bool ParseSize(const std::wstring& text, uint64_t& out) {
    size_t i = 0;
    while (i < text.size() && text[i] == L' ') ++i;

    uint64_t value = 0;
    bool sawDigit = false;
    // Fractions are accepted because "1.5gb" is what people type.
    uint64_t frac = 0, fracScale = 1;

    while (i < text.size() && text[i] >= L'0' && text[i] <= L'9') {
        if (value > (UINT64_MAX - 9) / 10) return false;   // overflow
        value = value * 10 + static_cast<uint64_t>(text[i] - L'0');
        sawDigit = true;
        ++i;
    }
    if (i < text.size() && (text[i] == L'.' || text[i] == L',')) {
        ++i;
        while (i < text.size() && text[i] >= L'0' && text[i] <= L'9') {
            if (fracScale > UINT64_MAX / 10) break;
            frac = frac * 10 + static_cast<uint64_t>(text[i] - L'0');
            fracScale *= 10;
            sawDigit = true;
            ++i;
        }
    }
    if (!sawDigit) return false;

    while (i < text.size() && text[i] == L' ') ++i;

    std::wstring unit;
    while (i < text.size() && unit.size() < 3) {
        unit.push_back(LowerAscii(text[i]));
        ++i;
    }
    while (i < text.size() && text[i] == L' ') ++i;
    if (i != text.size()) return false;   // trailing junk

    uint64_t mult = 1;
    if (unit.empty() || unit == L"b")                    mult = 1;
    else if (unit == L"k" || unit == L"kb")              mult = 1024ull;
    else if (unit == L"m" || unit == L"mb")              mult = 1024ull * 1024;
    else if (unit == L"g" || unit == L"gb")              mult = 1024ull * 1024 * 1024;
    else if (unit == L"t" || unit == L"tb")              mult = 1024ull * 1024 * 1024 * 1024;
    else return false;

    if (value > UINT64_MAX / mult) return false;
    out = value * mult;
    if (fracScale > 1) {
        out = SatAdd(out, (frac * mult) / fracScale);
    }
    return true;
}

// Splits on whitespace, honouring double quotes so a phrase stays one term.
std::vector<std::wstring> Tokenise(const std::wstring& text) {
    std::vector<std::wstring> tokens;
    std::wstring cur;
    bool inQuotes = false;

    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c == L'"') {
            inQuotes = !inQuotes;
            continue;
        }
        if (!inQuotes && (c == L' ' || c == L'\t')) {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
            continue;
        }
        cur.push_back(c);
        if (tokens.size() > 64) break;   // bound a pathological query
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

bool StartsWith(const std::wstring& s, const wchar_t* prefix, size_t& after) {
    size_t i = 0;
    while (prefix[i]) {
        if (i >= s.size() || LowerAscii(s[i]) != prefix[i]) return false;
        ++i;
    }
    after = i;
    return true;
}

}  // namespace

Query ParseQuery(const std::wstring& text) {
    Query q;

    for (const std::wstring& raw : Tokenise(text)) {
        if (raw.empty()) continue;

        std::wstring token = raw;
        bool negate = false;
        if (token[0] == L'-' && token.size() > 1) {
            negate = true;
            token.erase(0, 1);
        }

        size_t after = 0;

        if (StartsWith(token, L"kind:", after)) {
            Cat cat;
            if (CatFromToken(token.substr(after), cat)) {
                q.kinds.push_back(cat);
                continue;
            }
            // Unrecognised kind falls through to a name term rather than
            // silently matching nothing.
        } else if (StartsWith(token, L"ext:", after)) {
            std::wstring ext = Lower(token.substr(after));
            if (!ext.empty() && ext[0] == L'.') ext.erase(0, 1);
            if (!ext.empty()) { q.exts.push_back(ext); continue; }
        } else if (StartsWith(token, L"is:", after)) {
            const std::wstring what = Lower(token.substr(after));
            if (what == L"file" || what == L"files") {
                q.only = Query::Only::Files;
                continue;
            }
            if (what == L"folder" || what == L"folders" || what == L"dir") {
                q.only = Query::Only::Folders;
                continue;
            }
        } else if (StartsWith(token, L"size:", after)) {
            token = token.substr(after);
        }

        // Size comparisons, either bare (">500mb") or after "size:".
        if (!token.empty() && (token[0] == L'>' || token[0] == L'<')) {
            const bool greater = token[0] == L'>';
            size_t start = 1;
            if (start < token.size() && token[start] == L'=') ++start;

            uint64_t bytes = 0;
            if (ParseSize(token.substr(start), bytes)) {
                if (greater) {
                    q.minSize = std::max(q.minSize, bytes);
                } else {
                    q.maxSize = std::min(q.maxSize, bytes);
                }
                continue;
            }
        }

        const std::wstring lowered = Lower(token);
        if (lowered.empty()) continue;
        if (negate) q.exclude.push_back(lowered);
        else        q.include.push_back(lowered);
    }
    return q;
}

bool QueryMatches(const Query& q, const Node& n) {
    if (q.only == Query::Only::Files && n.dir) return false;
    if (q.only == Query::Only::Folders && !n.dir) return false;

    // Size bounds apply to a directory's rolled-up total, which is what makes
    // ">10gb is:folder" a useful thing to type.
    if (n.size < q.minSize || n.size > q.maxSize) return false;

    if (!q.kinds.empty()) {
        bool hit = false;
        for (Cat c : q.kinds) if (n.cat == c) { hit = true; break; }
        if (!hit) return false;
    }

    if (!q.exts.empty()) {
        const ExtRef ref = ExtRefOf(n.name);
        bool hit = false;
        for (const std::wstring& e : q.exts) {
            if (ExtEquals(n.name, ref, e)) { hit = true; break; }
        }
        if (!hit) return false;
    }

    // Name matching done in place: lower-casing into a temporary would
    // allocate once per node examined, and this runs over the whole subtree.
    const auto contains = [&n](const std::wstring& needle) {
        const size_t nameLen = n.name.size();
        const size_t needleLen = needle.size();
        if (needleLen == 0 || needleLen > nameLen) return false;
        for (size_t i = 0; i + needleLen <= nameLen; ++i) {
            size_t k = 0;
            while (k < needleLen && LowerAscii(n.name[i + k]) == needle[k]) ++k;
            if (k == needleLen) return true;
        }
        return false;
    };

    for (const std::wstring& term : q.include) {
        if (!contains(term)) return false;
    }
    for (const std::wstring& term : q.exclude) {
        if (contains(term)) return false;
    }
    return true;
}

std::vector<FileHit> FindMatching(const Node& root, const Query& q,
                                  size_t limit) {
    std::vector<FileHit> hits;
    if (limit == 0 || q.Empty()) return hits;

    ForEachNodeWithPath(root, [&](const Node& n, const std::wstring& prefix) {
        if (hits.size() >= limit) return;
        if (!QueryMatches(q, n)) return;

        FileHit hit;
        hit.node = &n;
        hit.path = JoinRel(prefix, n.name);
        hit.size = n.size;
        hits.push_back(std::move(hit));
    });

    std::sort(hits.begin(), hits.end(),
              [](const FileHit& a, const FileHit& b) {
                  return a.size > b.size;
              });
    return hits;
}


// ------------------------------------------------------------------ easing

namespace ease {

namespace {
inline float Clamp01(float t) {
    return (t < 0.0f) ? 0.0f : (t > 1.0f ? 1.0f : t);
}
}  // namespace

float OutQuint(float t) {
    const float u = 1.0f - Clamp01(t);
    return 1.0f - u * u * u * u * u;
}

float OutCubic(float t) {
    const float u = 1.0f - Clamp01(t);
    return 1.0f - u * u * u;
}

float OutBack(float t) {
    // Overshoot tuned down from the textbook 1.70158: at a UI's distances a
    // full-strength overshoot reads as a wobble rather than as arrival.
    constexpr float c1 = 1.10f;
    constexpr float c3 = c1 + 1.0f;
    const float u = Clamp01(t) - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

float InOutCubic(float t) {
    const float x = Clamp01(t);
    if (x < 0.5f) return 4.0f * x * x * x;
    const float u = -2.0f * x + 2.0f;
    return 1.0f - (u * u * u) / 2.0f;
}

}  // namespace ease

}  // namespace spindle
