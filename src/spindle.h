// Spindle - shared types and the public interface of every module.
//
// This header is included by the portable core, the Windows-only modules and
// the host-side tests alike, so it must not pull in windows.h. Anything that
// needs a Win32 type belongs in scan.cpp, mft.cpp or ui.cpp.

#ifndef SPINDLE_H_
#define SPINDLE_H_

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace spindle {

// ---------------------------------------------------------------- categories

// What a file *is*, decided once at scan time from its extension. The map is
// coloured by this, which is the one thing a size-only treemap cannot tell
// you. Directory is first so index 0 stays the neutral folder shade, and the
// order must match theme::kCat in ui.cpp.
enum class Cat : uint8_t {
    Directory = 0,
    Media,
    Game,
    Archive,
    Code,
    Document,
    Binary,
    VirtualDisk,
    Database,
    System,
    Other,
    COUNT,
};

const wchar_t* CatName(Cat c);
Cat CategoryForFile(const std::wstring& name);

// -------------------------------------------------------------------- sizes

// Every size accumulation goes through this. A volume reporting absurd sizes
// must clamp at the top of the range, not wrap around to something small and
// quietly vanish from the map.
inline uint64_t SatAdd(uint64_t a, uint64_t b) {
    return (b > UINT64_MAX - a) ? UINT64_MAX : a + b;
}

// --------------------------------------------------------------------- tree

// One scanned file or directory. Sized with intent: the whole tree is held in
// memory so navigation is instant, and at two million nodes every byte here
// is two megabytes there.
struct Node {
    std::wstring      name;
    std::vector<Node> children;
    uint64_t          size  = 0;   // files: logical length; dirs: rolled up
    uint64_t          files = 0;   // files: 1; dirs: files anywhere below
    Cat               cat   = Cat::Other;
    bool              dir   = false;
};

// Fills in directory sizes and file counts from the leaves. Iterative: the
// nesting depth of a scanned tree comes off the disk and is not ours to
// trust, so no walk in this codebase may recurse on it.
inline void RollUp(Node& root) {
    struct Frame {
        Node*  node;
        size_t next;
    };
    std::vector<Frame> stack;
    stack.push_back(Frame{&root, 0});

    while (!stack.empty()) {
        Frame& f = stack.back();
        if (f.next < f.node->children.size()) {
            Node* child = &f.node->children[f.next];
            ++f.next;
            if (child->dir && !child->children.empty()) {
                stack.push_back(Frame{child, 0});
            }
            continue;
        }
        if (f.node->dir) {
            uint64_t bytes = 0;
            uint64_t count = 0;
            for (const Node& c : f.node->children) {
                bytes = SatAdd(bytes, c.size);
                count = SatAdd(count, c.files);
            }
            f.node->size  = bytes;
            f.node->files = count;
        }
        stack.pop_back();
    }
}

// Largest-first within every directory, so identical trees always lay out
// identically. Iterative for the same reason as RollUp.
inline void SortTree(Node& root) {
    std::vector<Node*> stack;
    stack.push_back(&root);
    while (!stack.empty()) {
        Node* n = stack.back();
        stack.pop_back();
        std::sort(n->children.begin(), n->children.end(),
                  [](const Node& a, const Node& b) { return a.size > b.size; });
        for (Node& c : n->children) {
            if (!c.children.empty()) stack.push_back(&c);
        }
    }
}

// ------------------------------------------------------------------ geometry

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool contains(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// ------------------------------------------------------------------- treemap

// One drawn block. `parent` indexes the cell this one nests inside (-1 at the
// top level): cells sit up to five levels below the viewed directory, so the
// on-screen breadcrumb plus a cell's own name is *not* its path, and the full
// chain has to be rebuilt through these links. `header` and `expanded` mark a
// directory that reserved a label strip and drew its children below it.
struct Cell {
    const Node* node     = nullptr;
    Rect        rect;
    int         depth    = 0;
    float       header   = 0.0f;
    bool        expanded = false;
    int         parent   = -1;
};

// Squarified layout (Bruls, Huizing & van Wijk, 2000) of `node`'s subtree
// into `bounds`, emitted parent-before-child. minArea is the smallest cell
// worth drawing, in square DIPs.
void BuildTreemap(const Node& node, Rect bounds, int maxDepth, float minArea,
                  std::vector<Cell>& out);

// Deepest cell under the point, or -1 / nullptr.
int HitTestIndex(const std::vector<Cell>& cells, float x, float y);
const Cell* HitTest(const std::vector<Cell>& cells, float x, float y);

// The node chain from the viewed directory down to `index`, outermost first,
// rebuilt through the parent links. Bounded internally, so a corrupted link
// terminates instead of spinning.
std::vector<const Node*> CellChain(const std::vector<Cell>& cells, int index);

// ------------------------------------------------------------------- display

std::wstring FormatSize(uint64_t bytes);
std::wstring FormatCount(uint64_t n);

// Strips C0/C1 controls, bidi overrides and zero-width characters, replacing
// each with a visible open-box mark. Filenames are attacker-controlled input
// and a bidi override in one reorders everything drawn after it.
std::wstring SanitizeForDisplay(const std::wstring& in,
                                bool* modified = nullptr);

// ----------------------------------------------------------------- reporting

struct ExtStat {
    std::wstring ext;
    uint64_t     bytes = 0;
    uint64_t     count = 0;
    Cat          cat   = Cat::Other;
};

struct FileHit {
    const Node*  node = nullptr;
    std::wstring path;   // relative to the subtree the report ran over
    uint64_t     size = 0;
};

std::vector<ExtStat> ExtensionBreakdown(const Node& root, size_t limit);
std::vector<FileHit> LargestFiles(const Node& root, size_t limit);
std::vector<FileHit> FindByName(const Node& root, const std::wstring& needle,
                                size_t limit);

bool ExportCsv(const Node& root, const std::wstring& rootPath,
               const std::wstring& outPath);

// Implemented in scan.cpp on Windows; the host test harness supplies its own,
// which is what lets ExportCsv run under the sanitizers.
bool WriteTextFileUtf8(const std::wstring& path, const std::wstring& text);

// -------------------------------------------------------------------- search

// A parsed Find query. Every populated term must match, so a query reads as a
// sentence: "kind:media >500mb -temp".
struct Query {
    enum class Only { Any, Files, Folders };

    std::vector<std::wstring> include;   // lower-cased name substrings
    std::vector<std::wstring> exclude;
    std::vector<Cat>          kinds;
    std::vector<std::wstring> exts;      // lower-cased, no leading dot
    uint64_t                  minSize = 0;
    uint64_t                  maxSize = UINT64_MAX;
    Only                      only    = Only::Any;

    bool Empty() const {
        return include.empty() && exclude.empty() && kinds.empty() &&
               exts.empty() && minSize == 0 && maxSize == UINT64_MAX &&
               only == Only::Any;
    }
};

bool CatFromToken(const std::wstring& token, Cat& out);
Query ParseQuery(const std::wstring& text);
bool QueryMatches(const Query& q, const Node& n);
std::vector<FileHit> FindMatching(const Node& root, const Query& q,
                                  size_t limit);

// ------------------------------------------------------------------ scanning

struct Volume {
    std::wstring path;       // "C:\"
    std::wstring label;
    uint64_t     capacity = 0;
    uint64_t     free     = 0;
};

// Shared between the scan thread and the UI. Counters are relaxed atomics:
// they feed a progress line, not a decision.
struct Progress {
    std::atomic<uint64_t> files{0};
    std::atomic<uint64_t> dirs{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<bool>     cancel{false};
    std::atomic<bool>     done{false};
};

struct ScanStats {
    uint64_t fileCount   = 0;
    uint64_t dirCount    = 0;
    uint64_t bytes       = 0;
    uint64_t deniedCount = 0;   // directories the scan could not open
    double   seconds     = 0.0;
    bool     usedMft     = false;
    bool     faulted     = false;   // a worker died; the tree is incomplete
};

struct ScanResult {
    Node      root;
    ScanStats stats;
};

std::vector<Volume> EnumerateVolumes();

// Scans `root` and returns the finished tree, rolled up and sorted. Tries the
// MFT fast path first where it applies, then falls back to the parallel
// directory walk. Cancellation is polled from `progress`.
ScanResult Scan(const std::wstring& root, uint32_t threads,
                Progress* progress);

// The MFT fast path (mft.cpp). Returns false - leaving `outRoot` untouched -
// on anything unexpected at all, from a non-NTFS volume to a record that
// fails validation; the caller falls back to the directory walk.
bool ScanWithMft(const std::wstring& root, Progress* progress, Node& outRoot,
                 ScanStats& outStats);

// -------------------------------------------------------------------- easing

namespace ease {
float OutQuint(float t);
float OutCubic(float t);
float OutBack(float t);
float InOutCubic(float t);
}  // namespace ease

}  // namespace spindle

#endif  // SPINDLE_H_
