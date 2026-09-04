// Spindle - disk space analyser
// Core types shared between the scanner, treemap layout and UI.
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_set>
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

    // The file has more than one directory entry pointing at it, so it also
    // exists elsewhere on the volume and deleting this one frees nothing
    // until the last link goes. Only the MFT path can see this; the
    // directory walker would have to open a handle per file to find out,
    // which is exactly what the scanner refuses to do.
    bool         hardlink  = false;
    // A cloud placeholder (OneDrive and friends): it has a size on paper but
    // occupies little or nothing locally, and touching its contents would
    // make Windows download it.
    bool         cloudOnly = false;

    std::vector<Node> children;

    Node() = default;
    Node(std::wstring n, bool d) : name(std::move(n)), dir(d) {}
};

// Saturating add. File sizes come from the filesystem and a corrupt or hostile
// volume can report values that would otherwise wrap a 64-bit accumulator.
inline uint64_t SatAdd(uint64_t a, uint64_t b) {
    return (a > UINT64_MAX - b) ? UINT64_MAX : a + b;
}

// Saturating multiply, for "this many copies of this size". Same reason:
// the count comes from a report built out of what a disk claimed.
inline uint64_t SatMul(uint64_t a, uint64_t b) {
    if (a == 0 || b == 0) return 0;
    return (a > UINT64_MAX / b) ? UINT64_MAX : a * b;
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

    // Bytes that look like free space on the map but are not. Hardlinked
    // bytes exist once however many names point at them; cloud-only bytes
    // are not on this disk at all. Reporting a total without these is how a
    // tool promises 8 GB back from WinSxS and delivers nothing.
    uint64_t hardlinkBytes = 0;
    uint64_t hardlinkFiles = 0;
    uint64_t cloudBytes    = 0;
    uint64_t cloudFiles    = 0;
};

struct ScanResult {
    Node      root;
    ScanStats stats;
    std::vector<std::wstring> denied;   // capped; see kMaxDeniedRecorded
};

inline constexpr size_t kMaxDeniedRecorded = 512;

// Post-order size roll-up of a tree whose leaves carry their own sizes and
// counts, done iteratively: nesting depth comes off the disk.
void RollUp(Node& root);

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
// Bounded by memory, not by how large a file someone can write. The
// reader reserves children up front, so a count derived from the file
// size alone let a one gigabyte cache demand nearly four gigabytes of
// address space in a single call, on a thread with nothing to catch the
// failure. Eight million nodes is roughly 600 MB of Node, which a tree
// that size needs in memory anyway, and more files than a desktop volume
// holds; the allocation is caught if it fails, so the cost of a hostile
// file is a bounded reserve and no cache, never a dead process.
inline constexpr uint64_t kMaxCacheNodes   = uint64_t{1} << 23;

// Maximum directory nesting any tree may reach, whatever produced it. The
// directory walker already stops here; the cache reader and the MFT builder
// must too, because `Node` owns its children by value and so the compiler's
// destructor recurses once per level. A tree deeper than the stack can
// unwind cannot be freed without crashing, and nothing can catch that.
inline constexpr size_t   kMaxTreeDepth    = 512;

void SerializeScan(const ScanResult& in, const CacheMeta& meta,
                   std::vector<uint8_t>& out);
// cancel, when given, is polled during the parse so a caller on a worker
// can abandon a large cache load promptly (a drive switched away from
// before its cache finished loading). A cancelled parse returns false and
// leaves out unusable, exactly like a malformed file.
bool DeserializeScan(const uint8_t* data, size_t len, ScanResult& out,
                     CacheMeta& meta,
                     const std::atomic<bool>* cancel = nullptr);

// Cache file location and I/O. Windows-only, implemented in scan.cpp; the
// serialised bytes above are what travels through them.
std::wstring CachePathForVolume(const std::wstring& volumePath);
bool LoadScanCache(const std::wstring& volumePath, ScanResult& out,
                   CacheMeta& meta,
                   const std::atomic<bool>* cancel = nullptr);
bool SaveScanCache(const std::wstring& volumePath, const ScanResult& res);
std::wstring CacheDirPath();
void ClearScanCaches();

// Explorer's "Scan with Spindle" on a folder's right-click menu. This is
// the only thing in the program that writes to the registry, it is off by
// default, and it writes under HKCU only - no elevation, nothing
// machine-wide, and Unregister removes every key it created. Kept honest in
// the security notes rather than quietly added.
bool RegisterShellVerb();
bool UnregisterShellVerb();
bool ShellVerbRegistered();

// -------------------------------------------------------------- duplicates
//
// Finding duplicates means reading file contents, which is the one thing the
// scanner never does. So it is a separate, explicit operation, and it is
// bounded hard: only files that share an exact size with another file are
// ever opened, because two files of different lengths cannot be identical.
// On a normal volume that eliminates almost everything before a byte is read.

// A 128-bit content digest, used to group files of three or more that share
// a size. Not a cryptographic hash and not presented as one: for a group of
// that many, reading each once and grouping by digest is O(n) where an
// exact all-pairs comparison would be O(n^2), and coincidental collision is
// the only thing being defended against. A group of exactly two - the
// common case - is instead confirmed by VerifyIdentical, an exact
// byte-for-byte comparison, so no crafted collision can make two different
// files look the same. Any future deletion will go through that same exact
// check before it touches a file.
struct Digest {
    uint64_t a = 0;
    uint64_t b = 0;
    bool operator==(const Digest& o) const { return a == o.a && b == o.b; }
    bool operator!=(const Digest& o) const { return !(*this == o); }
    bool operator<(const Digest& o) const {
        return (a != o.a) ? (a < o.a) : (b < o.b);
    }
};

// Streaming digest, so a 40 GB disk image is hashed without being held in
// memory. Feed it chunks in order; Finish() is stable for identical input
// regardless of how the input was divided into chunks.
class Hasher {
public:
    void Update(const uint8_t* data, size_t len);
    Digest Finish() const;

private:
    void Block(uint64_t v);

    uint64_t h1_ = 0x9E3779B97F4A7C15ull;
    uint64_t h2_ = 0xBF58476D1CE4E5B9ull;
    uint64_t len_ = 0;
    // Bytes left over from the previous chunk. Without this the digest would
    // depend on where the reads happened to split, so the same file hashed
    // in 1 MB chunks and in one go would disagree.
    uint8_t  tail_[8] = {};
    size_t   tailLen_ = 0;
};

// One file that is a candidate for, or a confirmed member of, a duplicate
// set. `path` is relative to the scanned subtree, as in FileHit.
struct DupFile {
    const Node*  node = nullptr;
    // The volume or folder `path` is relative to. Empty means "the root the
    // hunt was given", which is the single-tree case; a hunt spanning
    // several drives fills it in so every file still knows where it lives.
    std::wstring root;
    std::wstring path;
    uint64_t     size = 0;
    Digest       digest;

    // Where the file actually is, for opening and for display.
    std::wstring Full() const {
        if (root.empty()) return path;
        std::wstring out = root;
        if (!out.empty() && out.back() != L'\\') out += L'\\';
        return out + path;
    }
};

// A set of files with identical content. `wasted` is what deleting all but
// one of them would return: (count - 1) * size.
struct DupGroup {
    uint64_t             size = 0;
    uint64_t             wasted = 0;
    std::vector<DupFile> files;
};

// Everything a duplicate hunt needs to report, including what it refused to
// look at and why - a report that silently skipped half a drive would be
// worse than no report.
struct DupReport {
    std::vector<DupGroup> groups;
    uint64_t totalWasted   = 0;
    uint64_t filesHashed   = 0;
    uint64_t bytesHashed   = 0;
    uint64_t skippedCloud  = 0;   // would have been downloaded to read
    uint64_t skippedUnread = 0;   // permission denied, locked, vanished
    bool     cancelled     = false;
};

// Which files could possibly be duplicates: everything whose size is shared
// with at least one other file, above `minSize`, excluding directories,
// cloud placeholders and hardlinks (a hardlink is not a duplicate - it is
// already the same bytes, and "deduplicating" it frees nothing).
//
// Portable and host-tested: the caller supplies the reading.
std::vector<DupFile> DuplicateCandidates(const Node& root, uint64_t minSize);

// As above, but stamping each candidate with the volume it came from, so
// results from several drives can be pooled and still say where they are.
std::vector<DupFile> DuplicateCandidatesIn(const Node& tree,
                                           const std::wstring& rootPath,
                                           uint64_t minSize);

// The two halves, for a hunt spanning several trees: collect everything
// eligible from each, then filter the pooled result once. Filtering per
// tree would discard the file whose only twin lives on another drive -
// which is the whole point of comparing drives against each other.
std::vector<DupFile> CollectDupFiles(const Node& tree,
                                     const std::wstring& rootPath,
                                     uint64_t minSize);
std::vector<DupFile> FilterBySharedSize(std::vector<DupFile> files);

// Group candidates that have been given digests into confirmed sets.
// Candidates with no digest (unreadable, skipped) are dropped.
std::vector<DupGroup> GroupByDigest(const std::vector<DupFile>& hashed);

inline constexpr uint64_t kDefaultDupMinSize = 1u << 20;   // 1 MB

// Defined below, with the scanner it belongs to; only a pointer is needed
// here and the duplicate hunt reads the same cancel flag a scan does.
struct Progress;

// Hash an already-chosen candidate list. This is the slow half - it opens
// and reads files - and it touches nothing but its own arguments, so it is
// safe to run on a worker thread while the interface stays live. Candidates
// carry owned paths and no Node pointers, so a rescan replacing the tree
// underneath cannot invalidate anything here.
// Optional: called with the full path of each file as its bytes are about
// to be read, so an interface can say what a long verify is chewing on.
// Called from the hunt's own thread; the callee owns any hand-off.
using DupFileNote = void (*)(void* ctx, const std::wstring& full);

DupReport HashCandidates(std::vector<DupFile> candidates,
                         const std::wstring& rootPath, Progress* progress,
                         DupFileNote onFile = nullptr,
                         void* onFileCtx = nullptr);

// The pooled variant for candidates spanning volumes (each file carries
// its own root). Size classes wholly on one volume are hashed by one
// worker per volume, in parallel, so three drives read at three drives'
// speed; a size class spanning volumes is hashed in a shared batch so
// grouping stays global. Progress counts additively across workers.
DupReport HashCandidatesAcrossVolumes(std::vector<DupFile> candidates,
                                      Progress* progress,
                                      DupFileNote onFile = nullptr,
                                      void* onFileCtx = nullptr);

// True only if the two files are byte-for-byte identical. This is the exact
// check a deletion must pass before it touches anything: two files with the
// same 128-bit digest are near-certainly the same, but "near-certain" is not
// good enough to delete one believing the other is a copy. Opens both with
// the same safety as the hunt (no cloud fetch, no reparse follow); a file it
// cannot read is reported as not-identical, so the deletion is refused.
bool VerifyFilesIdentical(const std::wstring& a, const std::wstring& b,
                          Progress* progress);

// Move one file to the Recycle Bin. Reversible, refuses a protected system
// path or a drive root, and never touches more than the single file named.
// quiet: no shell progress window. Interactive callers pass their window
// and false, so the shell shows its own progress for a large folder, with
// its own Cancel; the result then reports the abort as a refusal.
bool RecycleToBin(const std::wstring& path, void* owner = nullptr,
                  bool quiet = true);

// Run a full duplicate hunt under `root`, whose files live under
// `rootPath`. Blocks until finished, so it belongs on a worker thread or in
// the headless command-line path. Windows-only: it opens and reads files.
// Opens read-only, sharing everything, never follows a cloud placeholder,
// and stops promptly when `progress->cancel` is set.
DupReport FindDuplicates(const Node& root, const std::wstring& rootPath,
                         uint64_t minSize, Progress* progress);

// ----------------------------------------------------------- command line
//
// A command line turns Spindle into something Task Scheduler can run, which
// is a better answer to "does it have scheduling" than shipping a service
// that has to be kept alive and secured. It is also how a shell verb and a
// UNC path get in.

struct CommandLine {
    enum class Mode { Gui, Help, Version, Export, GenUpdateKey,
                      SignRelease, VerifyManifest };
    Mode         mode = Mode::Gui;
    std::wstring path;      // volume, folder or UNC path; empty means ask
    std::wstring csvOut;    // --csv destination, for Mode::Export
    uint64_t     minDup = 0;   // --duplicates threshold, 0 = not requested
    bool         wantDuplicates = false;
    // Release signing and verification (see the auto-update notes).
    std::wstring signExe;   // --sign-release: the built exe
    std::wstring signKey;   // file holding the offline private key
    std::wstring signTag;   // the release tag the manifest promises
    std::wstring verifyManifest;   // --verify-update-manifest inputs
    std::wstring verifySig;
    std::wstring verifyPub;
    // --allow-network: headless mode may read a network location. Nothing
    // can ask there, so without this only a share remembered in the window
    // is read.
    bool         allowNetwork = false;
    bool         valid = true;
    std::wstring error;
};

// Parse already-split arguments, excluding argv[0]. Pure and host-tested:
// the Windows side only supplies the splitting.
CommandLine ParseCommandLine(const std::vector<std::wstring>& args);

// The text shown by --help, also used in the About/Controls box.
const wchar_t* CommandLineHelp();

// ------------------------------------------------------------- comparison
//
// What changed between two scans of the same volume. The cache already
// stores finished scans, so this answers "what ate my disk this week"
// without needing anything new on disk.

enum class ChangeKind : uint8_t { Added, Removed, Grown, Shrunk };

struct Change {
    ChangeKind   kind = ChangeKind::Added;
    std::wstring path;      // relative to the compared root
    bool         dir = false;
    uint64_t     before = 0;
    uint64_t     after  = 0;
    // Signed delta, widened so a full-size shrink cannot wrap. Positive for
    // growth, negative for loss.
    int64_t      delta = 0;
};

struct DiffReport {
    std::vector<Change> changes;   // largest absolute delta first
    uint64_t grewBy    = 0;
    uint64_t shrankBy  = 0;
    int64_t  netDelta  = 0;
};

// Compare two trees, reporting only entries whose size moved by at least
// `minDelta`. A directory that changed only because its children did is
// reported once, at the directory, and its children are not walked - the
// answer to "what grew" is a place to look, not ten thousand rows.
DiffReport DiffTrees(const Node& before, const Node& after,
                     uint64_t minDelta);

const wchar_t* ChangeKindName(ChangeKind k);

// ------------------------------------------------------------ force removal
//
// Force removal permanently deletes a file or folder, taking ownership and
// terminating the processes holding it open if that is what it takes. It is
// the one genuinely destructive thing in the program, so the decisions about
// what it may touch live here, in portable code the tests can reach: the
// Windows side must not be the only thing standing between a mis-click and
// an unbootable machine.
//
// "Force" applies to locks and permissions, never to the confirmations.

// True for anything force removal must refuse outright: volume roots, the
// Windows directory and its contents, the Program Files and ProgramData
// roots, the Users root, System Volume Information, and the boot files. The
// check is deliberately blunt - refusing a path that would have been
// survivable costs the user a manual delete; allowing one that is not costs
// them the machine.
bool IsProtectedSystemPath(const std::wstring& path);

// True when `name` is usable as a single path component. Names arrive from
// the MFT and from the cache file, and both are attacker-controlled: NTFS's
// POSIX namespace allows a backslash or a colon, and the cache is writable
// without elevation while being read with it. A name carrying a separator,
// a colon or a NUL would let a later join aim a path somewhere it was never
// displayed, so it is refused where it enters rather than where it bites.
bool IsSafeNodeName(const std::wstring& name);

// True for processes that must never be terminated to break a file lock.
// Killing any of these is an instant bugcheck or a broken session.
bool IsCriticalProcess(const std::wstring& exeName, uint32_t pid);

// A process holding a file open, as reported by the Restart Manager.
struct Locker {
    uint32_t     pid = 0;
    std::wstring name;      // display name, e.g. "Steam Client Bootstrapper"
    std::wstring image;     // image file name, e.g. "steam.exe"
    bool         critical = false;   // never terminate
    // Creation time, as the Restart Manager reported it. A PID is reused
    // the moment a process exits, so terminating by PID alone can hit
    // whatever inherited the number between listing and killing.
    uint64_t     startTime = 0;
};

// Which processes have the file or folder open. Empty when nothing does, or
// when the Restart Manager is unavailable - in which case the caller simply
// proceeds and lets the delete fail on its own.
std::vector<Locker> FindLockers(const std::wstring& path);

struct ForceRemoveResult {
    bool     ok            = false;
    bool     blocked       = false;   // refused by IsProtectedSystemPath
    uint64_t filesDeleted  = 0;
    uint32_t lastError     = 0;
    std::vector<Locker> remaining;    // still holding it when we gave up
};

// Permanently delete a file or folder: no Recycle Bin, no undo. Clears
// read-only/hidden/system attributes and takes ownership when access is
// denied. When `terminateLockers` is set, non-critical processes holding
// the target open are terminated first - that is what makes it "force".
//
// Refuses outright if IsProtectedSystemPath says so, whatever the caller
// asks for: the confirmations live in the interface, but the refusal lives
// here so no future caller can route around it.
// `approved` is the exact set of lockers the user was shown and consented
// to. Only members of it are ever terminated: re-sampling here would let
// the program end a process that appeared after the dialog, which nobody
// agreed to.
ForceRemoveResult ForceRemove(const std::wstring& path,
                              bool terminateLockers,
                              const std::vector<Locker>& approved =
                                  std::vector<Locker>());

// ------------------------------------------------------------------ settings
//
// Two booleans. Persisted as key=value lines beside the caches - the
// registry stays untouched, per the security posture - and parsed with the
// usual suspicion even though the file is normally our own.

struct Settings {
    bool keepCaches     = true;   // write and load .spincache files
    bool resumeOnLaunch = true;   // open the freshest cached drive at start
    bool prefetchAll    = true;   // read the other fixed drives at launch
    bool checkUpdates   = true;   // ask GitHub for a newer release at launch
    // Reopen the exact place last left: drive, folder, map-or-list and
    // panel. Off by default, because some people want a clean start and
    // some want to carry on; the menu toggles it.
    bool rememberView   = false;
    std::string lastPath;         // UTF-8 folder path to reopen
    bool lastBrowse     = false;  // list view rather than the map
    int  lastPanel      = 0;      // which side panel was open
    // Network shares the person has agreed to read, remembered by their
    // own identity (\\server\share, normalised) rather than by drive
    // letter, so a letter mapped somewhere else later asks afresh. A
    // mapping with no name has no identity and is asked every time.
    std::vector<std::string> trustedShares;   // UTF-8, normalised
    // An update was staged: the previous binary sits beside the running
    // one as spindle.exe.old until the next launch removes it. Only then
    // is the executable's folder touched at launch at all.
    bool cleanupOld = false;
    // Highest signed manifest serial ever accepted. Anti-replay: a
    // genuine but superseded release carries a lower serial and is
    // refused, so nobody can be pinned on an old signed version.
    uint64_t updateSerial = 0;
};

// Room for the trusted share list at its cap and a long remembered path;
// the writer stays within this by construction and the reader refuses a
// larger file outright rather than parsing a prefix of it.
inline constexpr size_t kMaxSettingsBytes  = 65536;
inline constexpr size_t kMaxTrustedShares  = 32;
inline constexpr size_t kMaxShareKeyChars  = 260;

// Portable share identity handling, host-tested. A key is a UNC path,
// \\server\share[\deeper], and nothing else: the long-path spelling folds
// to it, every other \\?\ or \\.\ spelling is refused, as are dot
// components, control characters and characters no server or share name
// may contain. NormalizeShareKey returns the canonical lowercase form, or
// empty when the input is not a share identity at all; a scan of a
// location with no identity asks every time and is never remembered.
// UTF-8 both ways, portable (wchar_t is 16-bit on Windows and 32-bit on
// the host, so this works in code points and handles surrogate pairs).
// Malformed input is replaced with U+FFFD rather than dropped or trusted.
std::string  WideToUtf8(const std::wstring& w);
std::wstring Utf8ToWide(const std::string& u);
std::wstring NormalizeShareKey(std::wstring s);
// The share a normalised key belongs to: \\server\share\deep\er folds to
// \\server\share, since permission is granted per share, not per folder.
// A lettered fallback key is returned as it is; anything else, empty.
std::wstring ShareRootOf(const std::wstring& key);
// Whether a location may be read without asking: not a network location,
// or a share whose identity is remembered in the settings or was agreed
// to this run. A network location with no identity is never allowed
// silently. Portable, host-tested; the interface wraps it.
struct NetPlace;
bool ShareAllowedFor(const Settings& s,
                     const std::unordered_set<std::wstring>& session,
                     const NetPlace& np);
bool ShareTrusted(const Settings& s, const std::wstring& key);
// Adds a normalised key; false when invalid or the list is at its cap.
bool TrustShare(Settings& s, const std::wstring& key);

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
    // What the worker is doing when the counters cannot say: the file
    // table is read whole before a single file is counted, and a cache is
    // unsealed and parsed before anything is shown. Without this the
    // window said "Scanning" over both, which read as a stuck rescan.
    // 0 walking or idle, 1 reading the file table, 2 building the tree
    // from it, 3 loading the cached map.
    std::atomic<uint32_t> phase{0};
    std::atomic<uint64_t> tableBytes{0};   // of the file table read so far
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
    bool         fixed    = false;  // DRIVE_FIXED: safe to read unprompted
    bool         remote   = false;  // DRIVE_REMOTE: read only with permission
    // An internal fixed disk. Only these are cached or read unprompted:
    // removable media, disks on a USB, FireWire or card-reader bus, and
    // network shares come and go, and a listing must not outlive them.
    bool         cacheable = false;
};

std::vector<Volume> EnumerateVolumes();

// Windows-only, scan.cpp. Where a path physically lives. A UNC path is a
// network location by spelling; a lettered path is one when the letter is
// a network drive, a SUBST of a UNC path, or (with `resolve`) a folder
// that is a symbolic link or junction into a share, found by opening it
// once and asking where the handle ended up. `resolve` is false at launch,
// where nothing may be opened unasked, and true at the moment of a click.
// `key` is the share identity to remember permission under, empty when
// the location has no identity worth remembering.
struct NetPlace {
    bool         network = false;
    std::wstring key;
};
NetPlace ClassifyPath(const std::wstring& path, bool resolve);
bool RootIsNetwork(const std::wstring& root);              // ClassifyPath(root, true)
std::wstring ShareIdentityForRoot(const std::wstring& root);

// A cache file on disk is a small frame around sealed bytes: this magic,
// a version, then the body. Version 2 is an envelope: a fresh random key
// protected by Windows data protection (DPAPI), which costs milliseconds
// for 32 bytes, and the tree itself under AES-256-GCM with that key,
// in-process and fast. Version 1 sealed the whole tree with DPAPI, which
// hands every byte to the security subsystem and took seconds on a large
// drive; it is still read, and rewritten as version 2 at the next save.
// The key is the account's own, managed by Windows and never stored by
// the program, so a copied or imaged cache is unreadable anywhere but
// under that account. A plain cache from an older build fails the frame
// and is deleted rather than read.
inline constexpr uint32_t kCacheSealMagic         = 0x454E5053;   // "SPNE"
inline constexpr uint32_t kCacheSealVersion       = 2;
inline constexpr uint32_t kCacheSealVersionLegacy = 1;
inline constexpr size_t   kCacheSealHeader        = 8;
inline constexpr size_t   kCacheSealKeyMax        = 4096;   // a DPAPI blob of 32 bytes is ~300
inline constexpr size_t   kCacheSealNonce         = 12;
inline constexpr size_t   kCacheSealTag           = 16;
// The frame check: magic, a known version, and at least one byte of body.
// `offset` is where the version-specific body starts.
bool SealedCachePayload(const uint8_t* data, size_t len, size_t& offset);
// The version 2 layout, bounds-checked: offsets into `data` of the
// protected key, the nonce, the tag and the ciphertext. False for any
// other version or a frame that does not fit.
struct SealedFrame {
    size_t keyOff = 0, keyLen = 0, nonceOff = 0, tagOff = 0, cipherOff = 0;
};
bool SealedCacheFrame(const uint8_t* data, size_t len, SealedFrame& f);

// Windows-only, scan.cpp. Whether a lettered root may be cached at all
// (see Volume::cacheable), and the launch sweep that removes any cache
// file whose drive is gone or should never have had one. Returns how
// many were removed.
bool   VolumeCacheable(const std::wstring& root);
size_t PruneStaleCaches();

// Portable decision behind PruneStaleCaches, host-tested. `present` lists
// every drive letter the system currently has, with 1 for cacheable, 0
// for never-cacheable and -1 for unknowable (a locked encrypted volume
// keeps its letter but answers nothing, and must keep its cache).
// `cached` lists the letters that have a cache file. Returns the letters
// whose cache should be deleted: absent from the system, or present and
// never-cacheable.
std::vector<wchar_t> CachesToDrop(
    const std::vector<std::pair<wchar_t, int>>& present,
    const std::vector<wchar_t>& cached);

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
    // Path substrings, matched against the full path rather than the name.
    // A term lands here when it contains a separator, or when the whole
    // query is a path: a pasted path is a request to see what lives there,
    // and could never match a name.
    std::vector<std::wstring> pathInclude;
    std::vector<std::wstring> pathExclude;
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
               pathInclude.empty() && pathExclude.empty() &&
               exts.empty() && minSize == 0 && maxSize == UINT64_MAX &&
               only == Only::Any;
    }
    bool HasPathTerms() const {
        return !pathInclude.empty() || !pathExclude.empty();
    }
};

// Parses query text. Never fails: anything unrecognised is treated as a name
// term, because a search box that rejects input is worse than one that
// guesses.
Query ParseQuery(const std::wstring& text);

// Path completion, split so the pure logic is testable off Windows. Split
// normalises slashes and the drive-relative form, then divides into the
// parent directory (with its trailing separator) and the partial leaf.
struct PathPrefix {
    std::wstring dir;       // e.g. "E:\" or "E:\Games\"
    std::wstring partial;   // the leaf being typed, may be empty
    bool         ok = false;
};
PathPrefix SplitPathForCompletion(std::wstring text);

// Given the parent's directory and file names that matched the partial,
// return the completed full path: a lone match completed whole (a folder
// gets a trailing separator so the next Tab goes deeper), otherwise the
// longest shared prefix. Empty when there is nothing to add. Folders are
// preferred over files, since this drives navigation.
std::wstring ApplyPathCompletion(const std::wstring& dir,
                                 const std::wstring& partial,
                                 const std::vector<std::wstring>& dirs,
                                 const std::vector<std::wstring>& files);

// Maps a kind: token to a category. Returns false if it names nothing.
bool CatFromToken(const std::wstring& token, Cat& out);

bool QueryMatches(const Query& q, const Node& n);

// Nodes matching `q`, largest first.
// rootPath is the real path of `root`, used when the query carries path
// terms: a scoped search starts at a node whose name is one component,
// and a full path could never match against that alone. Empty means the
// root's own name is its path, which is true of a volume root.
std::vector<FileHit> FindMatching(const Node& root, const Query& q,
                                  size_t limit,
                                  const std::wstring& rootPath = std::wstring());

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

// The duplicate report as CSV rows (set, copies, sizes, path), and the
// writer that puts it in a file. The text builder is pure and host-tested.
std::wstring DuplicatesCsvText(const DupReport& rep);
bool ExportDuplicatesCsv(const DupReport& rep, const std::wstring& outPath);

// Bounded extractor for a string field in untrusted JSON (the GitHub
// release API). Finds "key": "value" from `from`, unescaping only what a
// URL or tag needs. Returns false rather than guessing. foundAt (optional)
// receives where the key was, so a caller can walk repeated keys.
bool JsonFindString(const std::string& json, const std::string& key,
                    size_t from, std::string& out,
                    size_t* foundAt = nullptr);

// Auto-update (src/update.cpp, Windows only). Dormant until the embedded
// public key is set; every failure is the fail-closed path. See HACKING.
bool UpdateFeatureEnabled();
bool CheckForUpdate(const wchar_t* currentVersion, uint64_t minSerial,
                    std::wstring& tagOut);
std::wstring ApplyUpdate(const wchar_t* currentVersion,
                         const std::wstring& expectedTag,
                         uint64_t minSerial, uint64_t& serialOut);
void CleanupOldUpdate();
bool GenerateUpdateKeypair(std::wstring& outText);
bool SignReleaseFile(const std::wstring& exePath,
                     const std::wstring& privKeyB64,
                     const std::wstring& tag, std::wstring& err);
bool VerifyManifestFile(const std::wstring& manifestPath,
                        const std::wstring& sigPath,
                        const std::wstring& pubB64);

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
