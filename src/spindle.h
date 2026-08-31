// Spindle - disk space analyser
// Core types shared between the scanner, treemap layout and UI.
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <atomic>

namespace spindle {

// ---------------------------------------------------------------- categories

// Files are coloured by what they are, not at random. The category is decided
// once at scan time from the extension and never recomputed during rendering.
enum class Cat : uint8_t {
    Directory = 0,
    Media,        // video, audio, images
    Game,         // game package/asset formats
    Archive,      // zip, 7z, tar, installers
    Code,         // source, headers, scripts
    Document,     // text, pdf, office
    Binary,       // exe, dll, sys, obj
    VirtualDisk,  // vmdk, vhdx, iso, dumps
    Database,     // sqlite, db, mdb, log-structured stores
    System,       // anything under a known OS-owned path
    Other,
    COUNT
};

const wchar_t* CatName(Cat c);

// Decide a category from a filename. Extension match is case-insensitive.
Cat CategoryForFile(const std::wstring& name);

// ---------------------------------------------------------------------- tree

struct Node {
    std::wstring name;
    uint64_t     size  = 0;   // bytes, inclusive of children for directories
    uint32_t     files = 0;   // file count in subtree
    bool         dir   = false;
    Cat          cat   = Cat::Other;
    std::vector<Node> children;

    Node() = default;
    Node(std::wstring n, bool d) : name(std::move(n)), dir(d) {}
};

// Saturating add. File sizes come from the filesystem and a corrupt or hostile
// volume can report values that would otherwise wrap a 64-bit accumulator.
inline uint64_t SatAdd(uint64_t a, uint64_t b) {
    return (a > UINT64_MAX - b) ? UINT64_MAX : a + b;
}

// --------------------------------------------------------------- scan result

struct ScanStats {
    uint64_t bytes       = 0;
    uint64_t fileCount   = 0;
    uint64_t dirCount    = 0;
    uint64_t deniedCount = 0;
    double   seconds     = 0.0;
    // A worker aborted on an exception. The tree is usable but incomplete,
    // and the interface says so rather than presenting partial totals as
    // though they were the whole volume.
    bool     faulted     = false;
    // The scan read the Master File Table rather than walking directories.
    bool     usedMft     = false;
};

struct ScanResult {
    Node      root;
    ScanStats stats;
    std::vector<std::wstring> denied;   // capped; see kMaxDeniedRecorded
};

inline constexpr size_t kMaxDeniedRecorded = 512;

// ----------------------------------------------------------------- scan cache
//
// A finished scan serialised for reuse: clicking a drive shows the cached
// map immediately while a fresh scan revalidates behind it. The file is read
// back and its tree dereferenced everywhere, so deserialisation trusts
// nothing - every length, count and enum is validated before use, the same
// posture as ntfs.cpp. The encoding is explicit little-endian UTF-16 so the
// host-side tests exercise the identical bytes the Windows build writes.

struct CacheMeta {
    uint64_t savedUnixMs  = 0;   // when the cached scan finished
    uint32_t volumeSerial = 0;   // rejects a cache from a re-lettered volume
};

// Refusal bounds. A cache is a convenience; anything implausible is discarded
// and the ordinary scan runs instead.
inline constexpr size_t   kMaxCacheBytes   = size_t{1} << 30;
inline constexpr size_t   kMaxCacheNameLen = 4096;          // UTF-16 units
inline constexpr uint64_t kMaxCacheNodes   = uint64_t{1} << 26;

void SerializeScan(const ScanResult& in, const CacheMeta& meta,
                   std::vector<uint8_t>& out);
bool DeserializeScan(const uint8_t* data, size_t len, ScanResult& out,
                     CacheMeta& meta);

// Cache file location and I/O. Windows-only, implemented in scan.cpp; the
// serialised bytes above are what travels through them.
std::wstring CachePathForVolume(const std::wstring& volumePath);
bool LoadScanCache(const std::wstring& volumePath, ScanResult& out,
                   CacheMeta& meta);
bool SaveScanCache(const std::wstring& volumePath, const ScanResult& res);
std::wstring CacheDirPath();
void ClearScanCaches();

// ------------------------------------------------------------------ settings
//
// Two booleans. Persisted as key=value lines beside the caches - the
// registry stays untouched, per the security posture - and parsed with the
// usual suspicion even though the file is normally our own.

struct Settings {
    bool keepCaches     = true;   // write and load .spincache files
    bool resumeOnLaunch = true;   // open the freshest cached drive at start
};

inline constexpr size_t kMaxSettingsBytes = 4096;

Settings ParseSettings(const uint8_t* data, size_t len);
void SerializeSettings(const Settings& s, std::vector<uint8_t>& out);

// Windows-only, implemented in scan.cpp.
Settings LoadSettings();
bool SaveSettings(const Settings& s);

// Progress is polled by the UI thread while a scan runs. Relaxed ordering is
// fine: these are display counters, nothing branches on them.
struct Progress {
    std::atomic<uint64_t> files{0};
    std::atomic<uint64_t> dirs{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<bool>     cancel{false};
    std::atomic<bool>     done{false};
};

// Walk `root` and return the aggregated tree. Reparse points are never
// followed, so junctions and symlinks cannot loop or double-count.
//
// Prefers the NTFS Master File Table when it is available -- local NTFS
// volume, elevated process -- and falls back to a parallel directory walk
// otherwise. Both paths produce the same tree; the MFT path is far faster.
ScanResult Scan(const std::wstring& root, unsigned threads, Progress* progress);

// Reads the tree from the volume's Master File Table. Returns false, having
// changed nothing observable, whenever the fast path is unavailable.
bool ScanMft(const std::wstring& root, Progress* progress, ScanResult& out);

// True when the last completed scan used the MFT rather than the walker.
// Purely informational; both paths agree on the result.

// ------------------------------------------------------------------- volumes

struct Volume {
    std::wstring path;       // "C:\"
    std::wstring label;      // volume name, may be empty
    std::wstring fs;         // "NTFS"
    uint64_t     capacity = 0;
    uint64_t     free     = 0;
    bool         ready    = false;
};

std::vector<Volume> EnumerateVolumes();

// ------------------------------------------------------------------ treemap

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float right()  const { return x + w; }
    float bottom() const { return y + h; }
    bool  contains(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

struct Cell {
    const Node* node   = nullptr;
    Rect        rect;
    int         depth  = 0;

    // Height of the label strip reserved at the top of this cell. Children are
    // laid out below it, so the parent's own label never collides with theirs.
    // Zero means no strip was reserved.
    float       header = 0.0f;

    // True when child cells were laid out inside this one. An expanded cell
    // with no header must not draw a label: its children cover the area.
    bool        expanded = false;

    // Index of the enclosing cell in the same vector, or -1 at the top level.
    // Cells nest several levels below the directory being viewed, so the
    // breadcrumb alone does not identify a cell: without this chain a nested
    // cell's path is missing every intermediate directory.
    int         parent = -1;
};

// ------------------------------------------------------------------ easing

// Easing curves. All take and return a normalised 0..1.
//
// The default choice everywhere is OutQuint: it covers most of the distance in
// the first third of the duration, which is what makes a transition read as
// instant while still showing what moved. A symmetric curve at the same
// duration feels sluggish even though it takes exactly as long.
namespace ease {

float OutQuint(float t);   // very fast start, long gentle settle
float OutCubic(float t);   // gentler; for things that should not snap
float OutBack(float t);    // slight overshoot, for arrival emphasis
float InOutCubic(float t); // symmetric, for reversible movement

}  // namespace ease

// Squarified treemap (Bruls, Huizing & van Wijk 2000).
//
// Lays out `node`'s subtree into `bounds`, descending only while a cell is
// large enough to be worth drawing. `minArea` is in square pixels and bounds
// the output size: without it a 1.6M-file volume would emit 1.6M cells.
void BuildTreemap(const Node& node, Rect bounds, int maxDepth, float minArea,
                  std::vector<Cell>& out);

// Index of the topmost (deepest) cell containing the point, or -1.
int HitTestIndex(const std::vector<Cell>& cells, float x, float y);

// As above, returning the cell itself. Prefer HitTestIndex where the full
// path is needed: the index is what lets the parent chain be walked.
const Cell* HitTest(const std::vector<Cell>& cells, float x, float y);

// Names from the outermost cell down to `index`, inclusive. Empty if the
// index is out of range. Does not include the directory being viewed.
std::vector<const Node*> CellChain(const std::vector<Cell>& cells, int index);

// ------------------------------------------------------------------ display

// --------------------------------------------------------------- reporting

// Aggregate for one file extension, the breakdown every comparable tool
// shows: which kinds of file are actually consuming the volume.
struct ExtStat {
    std::wstring ext;      // lower case, without the dot; empty = no extension
    uint64_t     bytes = 0;
    uint32_t     count = 0;
    Cat          cat   = Cat::Other;
};

// Extension totals across a subtree, largest first. `limit` caps the result;
// the remainder is folded into a trailing "other" row so the totals still add
// up to the subtree.
std::vector<ExtStat> ExtensionBreakdown(const Node& root, size_t limit);

// Largest files in a subtree, largest first. Directories are excluded: this
// answers "what single files should I look at", which a treemap makes you
// hunt for.
struct FileHit {
    const Node*  node = nullptr;
    std::wstring path;   // relative to the subtree root
    uint64_t     size = 0;
};
std::vector<FileHit> LargestFiles(const Node& root, size_t limit);

// ------------------------------------------------------------------ search

// A parsed search query.
//
// Bare words match the name. Prefixed terms filter by kind, extension, size
// or type, and every term must match -- so `kind:media >500mb` is "media
// files over 500 MB", which is the question people actually have when a drive
// fills up. The alternative, a row of dropdowns, takes longer to operate than
// typing it.
//
//   pak                 name contains "pak"
//   "two words"         name contains the quoted phrase
//   kind:media          one of the categories in the Kinds panel
//   ext:vmdk            exact extension
//   >500mb  <2gb        size bounds; b/kb/mb/gb/tb, or bare bytes
//   size:>1gb           same, spelled out
//   is:file is:folder   restrict to one or the other
//   -temp               name must NOT contain "temp"
struct Query {
    std::vector<std::wstring> include;   // name substrings, all must match
    std::vector<std::wstring> exclude;   // name substrings, none may match
    std::vector<Cat>          kinds;     // any match; empty means any kind
    std::vector<std::wstring> exts;      // any match; empty means any
    uint64_t minSize = 0;
    uint64_t maxSize = UINT64_MAX;

    enum class Only { Any, Files, Folders };
    Only only = Only::Any;

    // True when nothing was specified, so the caller can show everything or
    // nothing rather than treating an empty query as "match all".
    bool Empty() const {
        return include.empty() && exclude.empty() && kinds.empty() &&
               exts.empty() && minSize == 0 && maxSize == UINT64_MAX &&
               only == Only::Any;
    }
};

// Parses query text. Never fails: anything unrecognised is treated as a name
// term, because a search box that rejects input is worse than one that
// guesses.
Query ParseQuery(const std::wstring& text);

// Maps a kind: token to a category. Returns false if it names nothing.
bool CatFromToken(const std::wstring& token, Cat& out);

bool QueryMatches(const Query& q, const Node& n);

// Nodes matching `q`, largest first.
std::vector<FileHit> FindMatching(const Node& root, const Query& q,
                                  size_t limit);

// Convenience wrapper: a plain name substring search.
std::vector<FileHit> FindByName(const Node& root, const std::wstring& needle,
                                size_t limit);

// Writes the subtree as CSV: path, size in bytes, kind. Returns false if the
// file could not be opened.
bool ExportCsv(const Node& root, const std::wstring& rootPath,
               const std::wstring& outPath);

// Writes UTF-16 text to a file as UTF-8 with a BOM. Platform-specific; the
// host test build supplies its own so the CSV writer stays testable.
bool WriteTextFileUtf8(const std::wstring& path, const std::wstring& text);

// Human-readable byte count, e.g. "1.44 GB".
std::wstring FormatSize(uint64_t bytes);

// Thousands-separated integer.
std::wstring FormatCount(uint64_t n);

// Strip characters that must never reach the renderer: C0/C1 controls, and the
// Unicode bidi overrides (U+202A..U+202E, U+2066..U+2069).
//
// This matters because filenames are attacker-controlled input. A sample named
// "invoice\u202Egpj.exe" renders as "invoicexe.jpg" in any bidi-aware text
// stack, which is a live technique for disguising executables. Spindle is
// meant to be pointed at directories full of hostile files, so it shows the
// real name and marks the substitution rather than rendering the spoof.
std::wstring SanitizeForDisplay(const std::wstring& in, bool* modified = nullptr);

}  // namespace spindle
