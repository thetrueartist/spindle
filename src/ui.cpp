// Spindle - Win32 + Direct2D interface.
//
// Palette is drawn from disk media rather than generic tool chrome: a cool
// blue-graphite base so category colours stay readable against it, and an
// amber accent taken from a drive activity LED.

#include "spindle.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <atomic>
#include <exception>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <unordered_set>
#include <cwctype>
#include <process.h>

using namespace spindle;

// Interface IDs defined locally. MinGW's import libraries carry some of these
// symbols and not others (ID2D1Factory yes, IDWriteFactory no), and __uuidof
// cannot be used where the out-pointer is void**. Spelling them out removes
// the link-time dependency entirely.
static const GUID kIidD2D1Factory = {
    0x06152247, 0x6f50, 0x465a,
    {0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07}};
static const GUID kIidDWriteFactory = {
    0xb859ee5a, 0xd838, 0x4b5b,
    {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};

// --------------------------------------------------------------- small ComPtr

template <typename T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;

    T*  get() const { return p_; }
    T** put() { reset(); return &p_; }
    T*  operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

    void** putVoid() { reset(); return reinterpret_cast<void**>(&p_); }

    void reset() {
        if (p_) { p_->Release(); p_ = nullptr; }
    }

private:
    T* p_ = nullptr;
};

// ------------------------------------------------------------------- palette

namespace theme {

constexpr D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.0f) {
    return D2D1_COLOR_F{
        static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
        static_cast<float>(rgb & 0xFF) / 255.0f, a};
}

constexpr uint32_t kInk    = 0x12161C;  // base, blue-graphite
constexpr uint32_t kSlab   = 0x191F27;  // panel
constexpr uint32_t kSlabHi = 0x1F2732;  // raised panel
constexpr uint32_t kRule   = 0x2A3440;  // hairline
constexpr uint32_t kType   = 0xC9D4E1;  // primary text
constexpr uint32_t kMute   = 0x6E7E92;  // secondary text
constexpr uint32_t kSignal = 0xE8A33D;  // amber, drive-activity accent

// Category colours. Chosen to stay distinguishable at small cell sizes on the
// graphite base, and to avoid two adjacent hues reading as the same block.
constexpr uint32_t kCat[static_cast<int>(Cat::COUNT)] = {
    0x3D4B5C,  // Directory
    0x4F9DD9,  // Media        blue
    0x7B6CD9,  // Game         violet
    0xD9714F,  // Archive      clay
    0x52B788,  // Code         green
    0xD9C24F,  // Document     yellow
    0xC264A8,  // Binary       magenta
    0xE8A33D,  // VirtualDisk  amber
    0x4FC4C4,  // Database     teal
    0x5A6B7D,  // System       slate
    0x47566B,  // Other        dim slate
};

}  // namespace theme

// --------------------------------------------------------------------- layout

namespace layout {
// Line boxes are pinned explicitly rather than left to the font's own
// metrics. Every text rect is then sized from these, so a rect can never be
// shorter than the line it has to hold -- which is what was clipping
// descenders and cropping the larger labels.
constexpr float kLineSmall = 17.0f;   // 12 px
constexpr float kLineBody  = 20.0f;   // 14 px
constexpr float kLineHead  = 26.0f;   // 19 px

constexpr float kSidebar    = 268.0f;
constexpr float kPad        = 16.0f;
constexpr float kCrumbH     = 40.0f;
constexpr float kStatusH    = 34.0f;
constexpr float kDriveCardH = 66.0f;
constexpr float kLegendRowH = 22.0f;
}  // namespace layout

// Must match res/spindle.rc.
constexpr WORD IDI_APPICON = 1;

constexpr UINT WM_SCAN_DONE     = WM_APP + 1;
constexpr UINT WM_DUPES_DONE    = WM_APP + 2;
constexpr UINT WM_PREFETCH_DONE = WM_APP + 3;
constexpr UINT WM_DUPE_FILE     = WM_APP + 4;
constexpr UINT WM_BULK_DONE     = WM_APP + 5;

// A cache this young is served without a revalidating rescan: the launch
// prefetch (or a scan moments ago) already walked the drive, and walking it
// again on every click buys nothing but disk noise. F5 always forces one.
constexpr uint64_t kFreshCacheMs = 5u * 60u * 1000u;
constexpr UINT_PTR kTimerId = 1;

// Durations. Deliberately short: the job of these is to show what moved, not
// to be watched. Anything past about 200 ms starts to feel like waiting.
constexpr DWORD kZoomMs   = 145;   // drilling in or out
constexpr DWORD kRevealMs = 230;   // first paint after a scan, staggered
constexpr DWORD kHoverMs  = 80;    // hover outline
constexpr DWORD kTabMs    = 130;   // panel underline slide
constexpr DWORD kFrameMs  = 8;     // ~120 Hz while animating

// Shown in the About box. The authoritative version lives in the resource
// block; keep the two in step when releasing.
constexpr const wchar_t* kAppVersion = L"1.8.3";

// A running animation. Holding the start time rather than a progress value
// means a dropped frame is skipped over instead of stretching the duration.
struct Anim {
    DWORD start = 0;
    DWORD ms    = 0;
    bool  live  = false;

    void Begin(DWORD durationMs) {
        start = GetTickCount();
        ms    = durationMs;
        live  = durationMs > 0;
    }

    // Raw 0..1 progress. Marks itself finished on reaching the end, so the
    // repaint timer can stop.
    float Raw() {
        if (!live || ms == 0) return 1.0f;
        const DWORD elapsed = GetTickCount() - start;
        if (elapsed >= ms) { live = false; return 1.0f; }
        return static_cast<float>(elapsed) / static_cast<float>(ms);
    }

    bool Running() const { return live; }
};

// ----------------------------------------------------------------- app state

struct DriveHit {
    Rect rect;
    int  index = -1;
};

struct App {
    HWND hwnd = nullptr;

    Com<ID2D1Factory>          d2d;
    Com<ID2D1HwndRenderTarget> rt;
    Com<IDWriteFactory>        dwrite;
    Com<IDWriteTextFormat>     fmtBody;
    Com<IDWriteTextFormat>     fmtSmall;
    Com<IDWriteTextFormat>     fmtHead;
    Com<IDWriteTextFormat>     fmtNum;
    Com<IDWriteTypography>     typoTabular;

    Com<ID2D1SolidColorBrush>     brush;      // recoloured per draw
    Com<ID2D1LinearGradientBrush> sheen;      // one reused cell highlight

    std::vector<Volume> volumes;
    int                 selected = -1;

    std::unique_ptr<ScanResult> result;
    std::vector<const Node*>    trail;   // root .. current directory
    std::vector<Cell>           cells;
    const Node*                 hoverNode = nullptr;
    int                         hoverIndex = -1;
    Rect                        hoverRect;

    std::vector<DriveHit> driveHits;
    std::vector<Rect>     crumbHits;
    Rect                  menuHit;   // the "···" beside the title
    Settings              settings;
    // A path given on the command line or by a shell verb. Scanned at
    // startup instead of the remembered drive, and the way a UNC share is
    // reached at all, since only lettered volumes are enumerated.
    std::wstring          startPath;
    size_t                crumbFirst = 0;   // trail index of crumbHits[0]

    // Side panel. Every comparable tool carries these two views next to the
    // map: which kinds of file are consuming the volume, and which individual
    // files are the biggest. The treemap answers "where", these answer "what".
    enum class Panel { Kinds, Largest, Search, Dupes };
    Panel                 panel = Panel::Kinds;
    std::vector<ExtStat>  extStats;
    std::vector<FileHit>  fileList;
    // Duplicate results. Held separately from fileList because finding them
    // reads every candidate file and must never happen just because a panel
    // was drawn - it runs when asked, and only then.
    DupReport             dupes;
    bool                  dupesRun = false;
    Rect                  dupeButton;
    Rect                  dupeAllButton;
    Rect                  dupeStopHit;   // "click here to stop" during a hunt
    std::wstring          dupeCurrentFile;   // what the hunt is reading now
    // Bulk recycle: one copy of every set kept, every extra verified byte
    // for byte against its kept copy immediately before it goes to the
    // bin. Shares the hunt's generation counter and progress, since the
    // two are mutually exclusive.
    bool                  bulkRunning = false;
    HANDLE                bulkWorker = nullptr;
    Rect                  dupeBulkButton;
    // Pointer position in layout units, kept so panels can highlight the
    // row under the cursor without each one recomputing the DPI scale.
    float                 mouseX = -1.0f;
    float                 mouseY = -1.0f;
    // One rect per drawn duplicate row, and the file it points at, so a
    // click can open the thing the user is looking at.
    std::vector<Rect>         dupeRowHits;
    std::vector<std::wstring> dupeRowPaths;
    // Which group and member each drawn row is, so a recycle can find a
    // verified twin within the same group and never delete the last copy.
    std::vector<std::pair<int, int>> dupeRowRef;
    // In-map reveal: clicking a duplicate navigates the map to the file
    // and flashes its cell, and every duplicate on screen gets a marker
    // while the Dupes tab is up. The set holds case-folded full paths.
    const Node*                      flashNode = nullptr;
    Anim                             dupeFlash;
    std::wstring                     pendingReveal;
    std::unordered_set<std::wstring> dupePaths;
    std::vector<uint8_t>             cellDupe;   // parallel to `cells`
    // Separate from the scanner's, so the two never write each other's
    // counters or cancel one another.
    Progress              dupeProgress;
    // The hunt runs on its own thread: it reads every candidate file, and
    // doing that on the UI thread froze the window and made the documented
    // Esc-to-cancel impossible to deliver.
    HANDLE                dupeWorker = nullptr;
    bool                  dupeRunning = false;
    std::atomic<uint64_t> dupeGen{0};
    // The launch prefetch: the fixed drives not on screen, walked one at a
    // time in the background so every drive has a fresh cache without being
    // clicked. It owns nothing the interface shows - its whole product is
    // the cache file - so cancelling one loses nothing but the walk.
    std::vector<std::wstring> prefetchQueue;
    HANDLE                prefetchWorker = nullptr;
    bool                  prefetching = false;
    std::wstring          prefetchRoot;
    Progress              prefetchProgress;
    std::atomic<uint64_t> prefetchGen{0};
    std::vector<Rect>     panelTabs;
    std::vector<Rect>     rowHits;
    bool                  panelDirty = true;

    std::wstring          query;
    bool                  searchFocus = false;
    // Ctrl+A: the whole query is selected, so the next keystroke or paste
    // replaces it. The only selection state the box has.
    bool                  searchSelectAll = false;
    Rect                  searchBox;

    Progress              progress;
    // Native thread handle rather than std::thread: MinGW's win32 threading
    // model backs the standard library types, and this is the one place the
    // application creates a long-lived thread.
    HANDLE                worker = nullptr;
    bool                  scanning = false;
    // The map on screen came from the cache file and a rescan is running
    // behind it. Cleared when the fresh tree is adopted.
    bool                  showingCache = false;
    uint64_t              cacheSavedMs = 0;
    // Bumped per scan. A result carrying a stale generation is discarded, so
    // a superseded scan can never overwrite the one the user is waiting on.
    std::atomic<uint64_t> scanGen{0};

    Anim  zoom;
    Rect  zoomFrom;
    Anim  reveal;      // staggered entrance after a scan
    Anim  hoverFade;
    Anim  tabSlide;
    Rect  tabFrom;     // underline position the slide started from
    Rect  tabTo;
    const Node* hoverPrev = nullptr;

    // Windows exposes a system-wide preference for reduced motion. Honouring
    // it is not decoration: for some people these transitions are the
    // difference between usable and not.
    bool  motion = true;

    // True while the user is dragging a window edge. A full relayout costs
    // 10-15 ms on a large volume, which is visible as stutter at every frame
    // of a drag, so the existing cells are scaled instead and the real
    // rebuild happens once the drag ends.
    bool  resizing = false;

    Rect  mapBounds;
    float dpiScale = 1.0f;
};

static App g_app;

// ------------------------------------------------------------------- helpers

static D2D1_RECT_F ToD2D(const Rect& r) {
    return D2D1_RECT_F{r.x, r.y, r.x + r.w, r.y + r.h};
}

static Rect Lerp(const Rect& a, const Rect& b, float t) {
    return Rect{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t};
}

static void AppendComponent(std::wstring& p, const std::wstring& name) {
    if (p.empty()) { p = name; return; }
    if (p.back() != L'\\') p += L'\\';
    p += name;
}

// Full path of the currently viewed directory, e.g. "C:\Users\sam".
static std::wstring TrailPath(const std::vector<const Node*>& trail) {
    std::wstring p;
    for (const Node* n : trail) AppendComponent(p, n->name);
    return p;
}

// Full path of a cell. Cells nest up to five levels inside the viewed
// directory, so the breadcrumb has to be extended by the cell's own parent
// chain -- appending just the cell's name yields a path with every
// intermediate directory missing, which would point Explorer (and delete)
// somewhere else entirely.
static std::wstring CellPath(int cellIndex) {
    std::wstring p = TrailPath(g_app.trail);
    for (const Node* n : CellChain(g_app.cells, cellIndex)) {
        AppendComponent(p, n->name);
    }
    return p;
}

// Case-fold for path identity. Both sides of every comparison come out of
// this program (the report on one hand, CellPath on the other), so simple
// towlower is enough; this is display navigation, not a security check.
static std::wstring FoldPath(std::wstring p) {
    for (wchar_t& c : p) c = static_cast<wchar_t>(towlower(c));
    return p;
}

// Rebuild the case-folded set of every path in the duplicate report.
static void RebuildDupePathSet() {
    g_app.dupePaths.clear();
    for (const DupGroup& grp : g_app.dupes.groups) {
        for (const DupFile& f : grp.files) {
            g_app.dupePaths.insert(FoldPath(f.Full()));
        }
    }
}

// Mark the cells whose file is in the duplicate report, so the map can
// outline them while the Dupes tab is up. Runs per treemap rebuild, which
// is navigation-rate, not frame-rate.
static void MarkDupeCells() {
    g_app.cellDupe.assign(g_app.cells.size(), 0);
    if (g_app.dupePaths.empty()) return;
    for (size_t i = 0; i < g_app.cells.size(); ++i) {
        const Cell& c = g_app.cells[i];
        if (!c.node || c.node->dir) continue;
        if (g_app.dupePaths.count(
                FoldPath(CellPath(static_cast<int>(i)))) != 0) {
            g_app.cellDupe[i] = 1;
        }
    }
}

static void SetBrush(uint32_t rgb, float alpha = 1.0f) {
    if (g_app.brush) g_app.brush->SetColor(theme::Hex(rgb, alpha));
}

// Draw text through a layout so tabular figures can be applied. Numeric
// columns need equal-width digits to be comparable down a column; that is a
// property of the figures, not a reason to switch to a monospace family.
static void DrawText(const std::wstring& text, IDWriteTextFormat* fmt,
                     const Rect& r, uint32_t rgb, float alpha = 1.0f,
                     bool tabular = false,
                     DWRITE_TEXT_ALIGNMENT align =
                         DWRITE_TEXT_ALIGNMENT_LEADING) {
    if (text.empty() || !fmt || !g_app.rt || !g_app.brush) return;
    if (r.w <= 0.0f || r.h <= 0.0f) return;

    SetBrush(rgb, alpha);

    // Alignment is a property of the shared format object, so it has to be
    // put back afterwards or it leaks into whatever draws next.
    if (!tabular || !g_app.typoTabular) {
        if (align != DWRITE_TEXT_ALIGNMENT_LEADING) fmt->SetTextAlignment(align);
        g_app.rt->DrawText(text.c_str(), static_cast<UINT32>(text.size()), fmt,
                           ToD2D(r), g_app.brush.get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (align != DWRITE_TEXT_ALIGNMENT_LEADING) {
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        return;
    }

    Com<IDWriteTextLayout> layout;
    if (FAILED(g_app.dwrite->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()), fmt, r.w, r.h,
            layout.put()))) {
        return;
    }
    const DWRITE_TEXT_RANGE all{0, static_cast<UINT32>(text.size())};
    layout->SetTypography(g_app.typoTabular.get(), all);
    layout->SetTextAlignment(align);
    g_app.rt->DrawTextLayout(D2D1_POINT_2F{r.x, r.y}, layout.get(),
                             g_app.brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

static void FillRect(const Rect& r, uint32_t rgb, float alpha = 1.0f) {
    if (!g_app.rt || !g_app.brush) return;
    SetBrush(rgb, alpha);
    g_app.rt->FillRectangle(ToD2D(r), g_app.brush.get());
}

static void StrokeRect(const Rect& r, uint32_t rgb, float alpha = 1.0f,
                       float width = 1.0f) {
    if (!g_app.rt || !g_app.brush) return;
    SetBrush(rgb, alpha);
    const D2D1_RECT_F d{r.x + width * 0.5f, r.y + width * 0.5f,
                        r.x + r.w - width * 0.5f, r.y + r.h - width * 0.5f};
    g_app.rt->DrawRectangle(d, g_app.brush.get(), width);
}

static void FillRound(const Rect& r, float radius, uint32_t rgb,
                      float alpha = 1.0f) {
    if (!g_app.rt || !g_app.brush) return;
    SetBrush(rgb, alpha);
    const D2D1_ROUNDED_RECT rr{ToD2D(r), radius, radius};
    g_app.rt->FillRoundedRectangle(rr, g_app.brush.get());
}

// ---------------------------------------------------------------- treemap fx

// Composite a cell's fill against the background so label contrast is judged
// against what is actually on screen, not the nominal colour. A directory
// drawn at 0.32 alpha is far darker than its palette entry suggests.
static uint32_t BlendOverInk(uint32_t rgb, float alpha) {
    const auto ch = [&](int shift) {
        const float top =
            static_cast<float>((rgb >> shift) & 0xFF) / 255.0f;
        const float bot =
            static_cast<float>((theme::kInk >> shift) & 0xFF) / 255.0f;
        const float v = top * alpha + bot * (1.0f - alpha);
        return static_cast<uint32_t>(std::lround(v * 255.0f)) & 0xFFu;
    };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// Relative luminance, WCAG definition.
static float Luminance(uint32_t rgb) {
    const auto lin = [](float c) {
        return (c <= 0.04045f) ? (c / 12.92f)
                               : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    const float r = lin(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f);
    const float g = lin(static_cast<float>((rgb >> 8) & 0xFF) / 255.0f);
    const float b = lin(static_cast<float>(rgb & 0xFF) / 255.0f);
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

// White on amber or yellow is close to unreadable; dark text on those is not.
static uint32_t LabelInkFor(uint32_t fill, float alpha) {
    return (Luminance(BlendOverInk(fill, alpha)) > 0.40f) ? 0x0E1218u
                                                          : 0xFFFFFFu;
}

// Nested directories get progressively lighter so hierarchy is visible without
// drawing a border around every single cell.
static uint32_t ShadeForDepth(uint32_t base, int depth) {
    const float lift = std::min(0.10f * static_cast<float>(depth), 0.34f);
    auto ch = [&](int shift) {
        const float v = static_cast<float>((base >> shift) & 0xFF) / 255.0f;
        const float out = v + (1.0f - v) * lift;
        return static_cast<uint32_t>(std::lround(out * 255.0f)) & 0xFFu;
    };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// ------------------------------------------------------------------ scanning

static void RebuildTreemap();   // defined below; the cache path uses it early

// Everything that holds a Node* into the current tree. One function with
// two callers - starting a scan, and adopting a finished one - because the
// two drifted apart once already and the result was a use-after-free that
// only showed up when a rescan landed under the pointer.
static void DropTreeReferences() {
    g_app.trail.clear();
    g_app.cells.clear();
    g_app.hoverNode  = nullptr;
    g_app.hoverPrev  = nullptr;
    g_app.hoverIndex = -1;
    // The panel caches hold Node pointers too, and a paint arrives mid-scan
    // because the progress timer forces one every frame.
    g_app.extStats.clear();
    g_app.fileList.clear();
    g_app.rowHits.clear();
    g_app.flashNode = nullptr;   // points into the tree being dropped
    g_app.cellDupe.clear();
    // Duplicate results deliberately survive: every Node pointer is
    // stripped before the hunt starts, so the report is owned strings only,
    // and losing an expensive answer just because the user looked at
    // another drive is the opposite of useful. Each row carries its own
    // volume, so what is on screen stays unambiguous.
}

static void JoinDupeWorker();

// "1 file", "2 files". The bare plural after a count of one reads as a
// mistake everywhere it appears.
static std::wstring FormatFiles(uint64_t n) {
    return FormatCount(n) + (n == 1 ? L" file" : L" files");
}

static uint64_t UnixNowMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const uint64_t ticks =
        (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return ticks / 10000 - 11644473600000ULL;
}

static void JoinWorker() {
    if (g_app.worker) {
        g_app.progress.cancel.store(true, std::memory_order_relaxed);
        WaitForSingleObject(g_app.worker, INFINITE);
        CloseHandle(g_app.worker);
        g_app.worker = nullptr;
    }
    g_app.scanning = false;
}

struct ScanRequest {
    std::wstring root;
    HWND         hwnd = nullptr;
    uint64_t     gen  = 0;
    // Copied from settings at start time so the scan thread never reads the
    // live settings struct the UI thread may be editing.
    bool         keepCache = true;
    // Which Progress the walk reports through. The foreground scan and the
    // launch prefetch each own one, so cancelling either never touches the
    // other's flags.
    Progress*    progress = nullptr;
    // A prefetch exists to leave a cache file behind: the tree is freed on
    // this thread and only a completion note is posted.
    bool         prefetch = false;
};

// Scan thread entry. Wrapped end to end: an exception escaping here would
// call std::terminate and take the process down mid-scan with nothing shown
// to the user.
unsigned __stdcall ScanThread(void* param) {
    std::unique_ptr<ScanRequest> req(static_cast<ScanRequest*>(param));
    // A request without an explicit Progress reports through the
    // foreground one rather than dereferencing null.
    if (req->progress == nullptr) req->progress = &g_app.progress;
    try {
        SYSTEM_INFO si{};
        GetNativeSystemInfo(&si);

        auto res = std::make_unique<ScanResult>(
            Scan(req->root, si.dwNumberOfProcessors, req->progress));

        // Cache the tree while this thread still owns it, before the post
        // hands it to the UI thread. Cancelled or faulted scans are not
        // saved: a truncated tree served instantly next launch would look
        // authoritative and be wrong.
        if (req->keepCache &&
            !req->progress->cancel.load(std::memory_order_relaxed) &&
            !res->stats.faulted) {
            SaveScanCache(req->root, *res);
        }

        if (req->prefetch) {
            // The cache file was the whole point; the tree dies here, on
            // the thread that built it, and the post carries only the
            // generation so the queue can move on.
            res.reset();
            PostMessageW(req->hwnd, WM_PREFETCH_DONE,
                         static_cast<WPARAM>(req->gen), 0);
            return 0;
        }

        // Ownership passes to the UI thread only if the post succeeds. If the
        // window has already gone, the unique_ptr frees it on the way out.
        if (PostMessageW(req->hwnd, WM_SCAN_DONE,
                         static_cast<WPARAM>(req->gen),
                         reinterpret_cast<LPARAM>(res.get()))) {
            // Deliberate: WM_SCAN_DONE re-adopts the pointer into a
            // unique_ptr on the UI thread. Static analysis cannot follow
            // ownership across a message post and reports a leak here.
            static_cast<void>(res.release());  // NOLINT(bugprone-unused-return-value)
        }
    } catch (...) {
        // Report failure rather than dying: a null result tells the handler
        // the scan did not complete.
        PostMessageW(req->hwnd,
                     req->prefetch ? WM_PREFETCH_DONE : WM_SCAN_DONE,
                     static_cast<WPARAM>(req->gen), 0);
    }
    return 0;
}

// Scan an explicit path. `volumeIndex` is the drive card to light up, or -1
// when the target is not one of the enumerated volumes - a UNC share or a
// folder handed over on the command line.
// The duplicate hunt, off the UI thread. It is handed owned paths only -
// every Node pointer is stripped before the request is built - so a rescan
// replacing the tree while it runs cannot invalidate anything it holds.
struct DupRequest {
    std::vector<DupFile> candidates;
    std::wstring         rootPath;
    HWND                 hwnd = nullptr;
    uint64_t             gen  = 0;
    uint64_t             lastNote = 0;   // throttle for the reading-now notes
};

// Runs on the hunt thread. Hands the interface the file about to be read,
// throttled, as an owned string it adopts. Swallows its own failures: a
// missed note must never abort a hunt.
static void DupeFileNote(void* ctx, const std::wstring& full) {
    DupRequest* r = static_cast<DupRequest*>(ctx);
    const uint64_t now = GetTickCount64();
    if (now - r->lastNote < 200) return;
    r->lastNote = now;
    try {
        auto copy = std::make_unique<std::wstring>(full);
        if (PostMessageW(r->hwnd, WM_DUPE_FILE,
                         static_cast<WPARAM>(r->gen),
                         reinterpret_cast<LPARAM>(copy.get()))) {
            static_cast<void>(copy.release());  // NOLINT
        }
    } catch (...) {
    }
}

// One group per set: the kept copy first, then the extras, each with its
// size. Owned strings only, so nothing the interface does can touch it.
struct BulkRequest {
    HWND     hwnd = nullptr;
    uint64_t gen  = 0;
    uint64_t lastNote = 0;
    std::vector<std::vector<std::pair<std::wstring, uint64_t>>> groups;
};

struct BulkOutcome {
    std::vector<std::wstring> recycled;
    uint64_t bytesFreed   = 0;
    size_t   skippedFiles = 0;
    size_t   skippedSets  = 0;
    bool     cancelled    = false;
};

static void BulkFileNote(BulkRequest* r, const std::wstring& full) {
    const uint64_t now = GetTickCount64();
    if (now - r->lastNote < 200) return;
    r->lastNote = now;
    try {
        auto copy = std::make_unique<std::wstring>(full);
        if (PostMessageW(r->hwnd, WM_DUPE_FILE,
                         static_cast<WPARAM>(r->gen),
                         reinterpret_cast<LPARAM>(copy.get()))) {
            static_cast<void>(copy.release());  // NOLINT
        }
    } catch (...) {
    }
}

unsigned __stdcall BulkThread(void* param) {
    std::unique_ptr<BulkRequest> req(static_cast<BulkRequest*>(param));
    auto outcome = std::make_unique<BulkOutcome>();
    try {
        uint64_t done = 0;
        for (const auto& g : req->groups) {
            if (g_app.dupeProgress.cancel.load(std::memory_order_relaxed)) {
                outcome->cancelled = true;
                break;
            }
            if (g.size() < 2) continue;
            const std::wstring& keeper = g[0].first;
            bool setInDoubt = false;
            for (size_t i = 1; i < g.size(); ++i) {
                if (g_app.dupeProgress.cancel.load(
                        std::memory_order_relaxed)) {
                    outcome->cancelled = true;
                    break;
                }
                if (setInDoubt) {
                    // A member no longer matching the kept copy means this
                    // set's identity is stale. Refusing the rest of it is
                    // the fail-safe direction.
                    ++outcome->skippedFiles;
                    continue;
                }
                BulkFileNote(req.get(), g[i].first);
                if (!VerifyFilesIdentical(g[i].first, keeper,
                                          &g_app.dupeProgress)) {
                    ++outcome->skippedFiles;
                    ++outcome->skippedSets;
                    setInDoubt = true;
                    continue;
                }
                if (RecycleToBin(g[i].first)) {
                    outcome->recycled.push_back(g[i].first);
                    outcome->bytesFreed += g[i].second;
                    ++done;
                    g_app.dupeProgress.files.store(
                        done, std::memory_order_relaxed);
                } else {
                    ++outcome->skippedFiles;
                }
            }
            if (outcome->cancelled) break;
        }
    } catch (...) {
        outcome->cancelled = true;
    }
    if (PostMessageW(req->hwnd, WM_BULK_DONE,
                     static_cast<WPARAM>(req->gen),
                     reinterpret_cast<LPARAM>(outcome.get()))) {
        static_cast<void>(outcome.release());  // NOLINT
    }
    return 0;
}

static void JoinBulkWorker() {
    if (!g_app.bulkWorker) return;
    g_app.dupeProgress.cancel.store(true, std::memory_order_relaxed);
    WaitForSingleObject(g_app.bulkWorker, INFINITE);
    CloseHandle(g_app.bulkWorker);
    g_app.bulkWorker = nullptr;
    g_app.bulkRunning = false;
}

unsigned __stdcall DupeThread(void* param) {
    std::unique_ptr<DupRequest> req(static_cast<DupRequest*>(param));
    try {
        auto rep = std::make_unique<DupReport>(
            HashCandidates(std::move(req->candidates), req->rootPath,
                           &g_app.dupeProgress, DupeFileNote, req.get()));
        if (PostMessageW(req->hwnd, WM_DUPES_DONE,
                         static_cast<WPARAM>(req->gen),
                         reinterpret_cast<LPARAM>(rep.get()))) {
            static_cast<void>(rep.release());  // NOLINT
        }
    } catch (...) {
        // Same contract as the scan thread: report failure rather than
        // letting an exception escape and call std::terminate.
        PostMessageW(req->hwnd, WM_DUPES_DONE,
                     static_cast<WPARAM>(req->gen), 0);
    }
    return 0;
}

static void JoinDupeWorker() {
    if (!g_app.dupeWorker) return;
    g_app.dupeProgress.cancel.store(true, std::memory_order_relaxed);
    WaitForSingleObject(g_app.dupeWorker, INFINITE);
    CloseHandle(g_app.dupeWorker);
    g_app.dupeWorker = nullptr;
    g_app.dupeRunning = false;
}

// Stop the background walk, now. Bumping the generation first means the
// completion note the joined thread already posted is recognised as stale
// and dropped. `requeue` puts the interrupted drive back at the head of the
// queue - false when whoever is cancelling is about to cover that drive
// itself.
static void CancelPrefetch(bool requeue) {
    if (g_app.prefetchWorker) {
        g_app.prefetchGen.fetch_add(1);
        g_app.prefetchProgress.cancel.store(true, std::memory_order_relaxed);
        WaitForSingleObject(g_app.prefetchWorker, INFINITE);
        CloseHandle(g_app.prefetchWorker);
        g_app.prefetchWorker = nullptr;
        if (requeue && !g_app.prefetchRoot.empty()) {
            g_app.prefetchQueue.insert(g_app.prefetchQueue.begin(),
                                       g_app.prefetchRoot);
        }
    }
    g_app.prefetching = false;
    g_app.prefetchRoot.clear();
}

// A hunt was requested while a scan runs. If the map is a complete cached
// tree and the scan is only revalidating it, the hunt wins: cancel the
// rescan, put the drive at the head of the background queue so freshness
// comes back later, and let the hunt use the tree on screen. Only a first
// scan with no tree behind it is worth blocking on, since there is
// nothing to hunt over yet. Returns true when the hunt may proceed.
static bool YieldScanToHunt() {
    if (!g_app.scanning) return true;
    if (!g_app.showingCache || !g_app.result) {
        MessageBoxW(g_app.hwnd,
                    L"The first scan of this drive is still running - "
                    L"there is nothing to search until it finishes.",
                    L"Spindle", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    JoinWorker();
    g_app.scanGen.fetch_add(1);   // orphan the cancelled scan's message
    if (g_app.selected >= 0 &&
        g_app.selected < static_cast<int>(g_app.volumes.size())) {
        const std::wstring& root =
            g_app.volumes[static_cast<size_t>(g_app.selected)].path;
        auto& q = g_app.prefetchQueue;
        if (std::find(q.begin(), q.end(), root) == q.end()) {
            q.insert(q.begin(), root);
        }
    }
    return true;
}

// Walk the next queued drive, if nothing that wants the disk is running.
// Called wherever the disk goes quiet: a scan or hunt finishing, a prefetch
// completing, launch.
static void StartPrefetchNext() {
    if (g_app.prefetchWorker || g_app.prefetchQueue.empty()) return;
    if (g_app.scanning || g_app.dupeRunning || g_app.bulkRunning) return;
    if (!g_app.settings.keepCaches || !g_app.settings.prefetchAll) return;

    const std::wstring root = g_app.prefetchQueue.front();
    g_app.prefetchQueue.erase(g_app.prefetchQueue.begin());

    g_app.prefetchProgress.files.store(0);
    g_app.prefetchProgress.dirs.store(0);
    g_app.prefetchProgress.bytes.store(0);
    g_app.prefetchProgress.cancel.store(false);
    g_app.prefetchProgress.done.store(false);

    auto req = std::make_unique<ScanRequest>();
    req->root      = root;
    req->hwnd      = g_app.hwnd;
    req->gen       = g_app.prefetchGen.fetch_add(1) + 1;
    req->keepCache = true;
    req->progress  = &g_app.prefetchProgress;
    req->prefetch  = true;

    const uintptr_t h =
        _beginthreadex(nullptr, 0, ScanThread, req.get(), 0, nullptr);
    if (h == 0) return;   // quietly: this work was never asked for out loud
    static_cast<void>(req.release());  // NOLINT(bugprone-unused-return-value)
    g_app.prefetchWorker = reinterpret_cast<HANDLE>(h);
    g_app.prefetching    = true;
    g_app.prefetchRoot   = root;
    SetTimer(g_app.hwnd, kTimerId, 33, nullptr);
}

// Queue every ready fixed drive except `excludeRoot` (the one already on
// screen). Removable and network drives are never touched unprompted -
// spinning up a sleeping USB disk or hammering a share nobody asked about
// is exactly what this program promises not to do. Cacheless drives go
// first; the rest oldest cache first.
static void QueueLaunchPrefetch(const std::wstring& excludeRoot) {
    if (!g_app.settings.keepCaches || !g_app.settings.prefetchAll) return;

    struct Entry { std::wstring root; uint64_t mtime; };
    std::vector<Entry> entries;
    for (const Volume& v : g_app.volumes) {
        if (!v.ready || !v.fixed || v.path == excludeRoot) continue;
        uint64_t mtime = 0;   // 0 = no cache yet = scan it first
        const std::wstring cp = CachePathForVolume(v.path);
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!cp.empty() &&
            GetFileAttributesExW(cp.c_str(), GetFileExInfoStandard, &fad)) {
            mtime = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime)
                     << 32) |
                    fad.ftLastWriteTime.dwLowDateTime;
        }
        entries.push_back(Entry{v.path, mtime});
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry& a, const Entry& b) {
                         return a.mtime < b.mtime;
                     });
    g_app.prefetchQueue.clear();
    for (Entry& e : entries) g_app.prefetchQueue.push_back(std::move(e.root));
    StartPrefetchNext();
}

static void StartScanPath(const std::wstring& root, int volumeIndex,
                          bool useCache) {
    if (root.empty()) return;
    JoinWorker();
    // A running duplicate hunt deliberately survives this: it holds owned
    // paths only, every row it produces carries its own volume, and the
    // panel can show its progress and its results from any drive. Killing
    // it here used to throw away minutes of reading because someone
    // glanced at another disk.

    // The prefetch yields the disk to anything the user actually asked for.
    // Its interrupted drive goes back to the head of the queue - unless it
    // is the very drive this scan is about to cover.
    CancelPrefetch(g_app.prefetchRoot != root);
    g_app.prefetchQueue.erase(std::remove(g_app.prefetchQueue.begin(),
                                          g_app.prefetchQueue.end(), root),
                              g_app.prefetchQueue.end());

    // Order matters: everything that points into the old tree is dropped
    // before the tree itself is freed.
    g_app.pendingReveal.clear();
    g_app.selected  = volumeIndex;
    DropTreeReferences();
    g_app.result.reset();
    g_app.showingCache = false;

    // A cached tree goes up immediately - no animation, that is the point -
    // and the scan below revalidates it. Skipped after a delete, where the
    // cache is known to describe the old state of exactly the files the user
    // is looking at.
    if (useCache && g_app.settings.keepCaches) {
        auto cached = std::make_unique<ScanResult>();
        CacheMeta meta;
        // A UNC path has no drive letter to key a cache to, so this simply
        // misses and the ordinary scan runs.
        if (LoadScanCache(root, *cached, meta)) {
            g_app.result = std::move(cached);
            g_app.trail.push_back(&g_app.result->root);
            RebuildTreemap();
            g_app.panelDirty   = true;
            g_app.showingCache = true;
            g_app.cacheSavedMs = meta.savedUnixMs;

            // Fresh enough to trust outright - the launch prefetch or a
            // scan minutes ago walked this drive - so no revalidating
            // rescan. Clicking between drives costs nothing; F5 forces.
            const uint64_t now = UnixNowMs();
            if (meta.savedUnixMs != 0 && now >= meta.savedUnixMs &&
                now - meta.savedUnixMs <= kFreshCacheMs) {
                StartPrefetchNext();
                InvalidateRect(g_app.hwnd, nullptr, FALSE);
                return;
            }
        }
    }

    g_app.progress.files.store(0);
    g_app.progress.dirs.store(0);
    g_app.progress.bytes.store(0);
    g_app.progress.cancel.store(false);
    g_app.progress.done.store(false);
    g_app.scanning = true;

    auto req = std::make_unique<ScanRequest>();
    req->root      = root;
    req->hwnd      = g_app.hwnd;
    req->gen       = g_app.scanGen.fetch_add(1) + 1;
    req->keepCache = g_app.settings.keepCaches;
    req->progress  = &g_app.progress;

    const uintptr_t h =
        _beginthreadex(nullptr, 0, ScanThread, req.get(), 0, nullptr);
    if (h == 0) {
        g_app.scanning = false;
        MessageBoxW(g_app.hwnd, L"Could not start the scan: the system "
                                L"refused a new thread.",
                    L"Spindle", MB_OK | MB_ICONERROR);
        return;
    }
    // The thread owns it now and frees it on the way out.
    static_cast<void>(req.release());  // NOLINT(bugprone-unused-return-value)
    g_app.worker = reinterpret_cast<HANDLE>(h);

    SetTimer(g_app.hwnd, kTimerId, 33, nullptr);
}

static void StartScan(int volumeIndex, bool useCache = true) {
    if (volumeIndex < 0 ||
        volumeIndex >= static_cast<int>(g_app.volumes.size())) {
        return;
    }
    StartScanPath(g_app.volumes[static_cast<size_t>(volumeIndex)].path,
                  volumeIndex, useCache);
}

// The layout is area-proportional, so scaling every cell by the same factor
// is visually near-identical to relaying out -- close enough to hold during a
// drag, and it costs microseconds rather than milliseconds.
static void ScaleCells(const Rect& from, const Rect& to) {
    if (from.w <= 0.01f || from.h <= 0.01f) return;
    const float sx = to.w / from.w;
    const float sy = to.h / from.h;
    for (Cell& c : g_app.cells) {
        c.rect.x = to.x + (c.rect.x - from.x) * sx;
        c.rect.y = to.y + (c.rect.y - from.y) * sy;
        c.rect.w *= sx;
        c.rect.h *= sy;
        c.header *= sy;
    }
}

static void RebuildTreemap() {
    g_app.cells.clear();
    if (g_app.trail.empty()) return;

    const Node* cur = g_app.trail.back();
    if (!cur) return;

    // minArea scales with the viewport so a large window shows more detail
    // without a small one emitting tens of thousands of invisible cells.
    const float area = g_app.mapBounds.w * g_app.mapBounds.h;
    const float minArea = std::max(6.0f, area / 42000.0f);

    BuildTreemap(*cur, g_app.mapBounds, 5, minArea, g_app.cells);
    MarkDupeCells();
}

// Rebuilds whichever side panel is showing, for the directory currently in
// view. Deferred behind a dirty flag: these walk the whole subtree, and doing
// that inside a paint would stall the window.
static void RefreshPanel() {
    g_app.extStats.clear();
    g_app.fileList.clear();
    if (g_app.trail.empty()) {
        g_app.panelDirty = false;
        return;
    }

    const Node& cur = *g_app.trail.back();
    switch (g_app.panel) {
        case App::Panel::Kinds:
            g_app.extStats = ExtensionBreakdown(cur, 12);
            break;
        case App::Panel::Largest:
            g_app.fileList = LargestFiles(cur, 40);
            break;
        case App::Panel::Search:
            g_app.fileList =
                FindMatching(cur, ParseQuery(g_app.query), 200);
            break;
        case App::Panel::Dupes:
            // Nothing to refresh: duplicates are found on request, not on
            // every navigation, because finding them reads files.
            break;
    }
    // Cleared last. Setting it first meant that if a walk above threw, the
    // panel was left empty but marked clean, and stayed blank until the
    // user happened to navigate.
    g_app.panelDirty = false;
}

static void NavigateTo(const Node* node, const Rect& from) {
    if (!node) return;
    g_app.zoomFrom  = from;
    g_app.zoom.Begin(g_app.motion ? kZoomMs : 0);
    g_app.panelDirty = true;
    RebuildTreemap();
    SetTimer(g_app.hwnd, kTimerId, kFrameMs, nullptr);
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static void GoUp() {
    if (g_app.trail.size() <= 1) return;
    g_app.trail.pop_back();
    g_app.hoverNode  = nullptr;
    g_app.hoverIndex = -1;
    NavigateTo(g_app.trail.back(), g_app.mapBounds);
}

// ------------------------------------------------------------------- drawing

static void DrawDriveCard(const Volume& v, const Rect& r, bool active,
                          int index) {
    g_app.driveHits.push_back(DriveHit{r, index});

    FillRound(r, 6.0f, active ? theme::kSlabHi : theme::kSlab);
    if (active) {
        SetBrush(theme::kSignal, 0.85f);
        const D2D1_ROUNDED_RECT rr{ToD2D(r), 6.0f, 6.0f};
        g_app.rt->DrawRoundedRectangle(rr, g_app.brush.get(), 1.0f);
    }

    const float pad = 11.0f;

    std::wstring letter = v.path.substr(0, 2);
    DrawText(letter, g_app.fmtHead.get(),
             Rect{r.x + pad, r.y + 6.0f, 40.0f, layout::kLineHead},
             active ? theme::kSignal : theme::kType);

    if (!v.label.empty()) {
        DrawText(SanitizeForDisplay(v.label), g_app.fmtSmall.get(),
                 Rect{r.x + pad + 34.0f, r.y + 6.0f,
                      r.w - pad * 2 - 34.0f, layout::kLineHead},
                 theme::kMute);
    }

    // Capacity bar. Fill is used space, so a nearly-full drive reads as a
    // nearly-full bar without needing to be labelled as such.
    const Rect bar{r.x + pad, r.y + 36.0f, r.w - pad * 2.0f, 5.0f};
    FillRound(bar, 2.5f, theme::kRule);

    if (v.capacity > 0) {
        const double used = static_cast<double>(v.capacity - v.free) /
                            static_cast<double>(v.capacity);
        const float w = static_cast<float>(used) * bar.w;
        if (w > 1.0f) {
            const bool tight = used > 0.90;
            const bool warn  = used > 0.78;
            FillRound(Rect{bar.x, bar.y, w, bar.h}, 2.5f,
                      tight ? 0xD9714F : (warn ? theme::kSignal : 0x4F9DD9));
        }
    }

    const std::wstring freeText = FormatSize(v.free) + L" free";
    DrawText(freeText, g_app.fmtSmall.get(),
             Rect{r.x + pad, r.y + 44.0f, r.w - pad * 2.0f,
                  layout::kLineSmall},
             theme::kMute, 1.0f, true);
}

static void DrawSidebar(const Rect& area) {
    FillRect(area, theme::kSlab, 0.55f);
    FillRect(Rect{area.right() - 1.0f, area.y, 1.0f, area.h}, theme::kRule);

    float y = area.y + layout::kPad;

    DrawText(L"Spindle", g_app.fmtHead.get(),
             Rect{area.x + layout::kPad, y, area.w - layout::kPad * 2,
                  layout::kLineHead},
             theme::kType);
    // About & settings, tucked beside the title where chrome belongs.
    g_app.menuHit = Rect{area.right() - 44.0f, y, 30.0f, 24.0f};
    DrawText(L"\u00B7\u00B7\u00B7", g_app.fmtBody.get(),
             Rect{g_app.menuHit.x, g_app.menuHit.y, g_app.menuHit.w,
                  layout::kLineBody},
             theme::kMute, 1.0f, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    y += 30.0f;
    DrawText(L"Disk space, mapped", g_app.fmtSmall.get(),
             Rect{area.x + layout::kPad, y, area.w - layout::kPad * 2,
                  layout::kLineSmall},
             theme::kMute);
    y += 34.0f;

    DrawText(L"Drives", g_app.fmtSmall.get(),
             Rect{area.x + layout::kPad, y, area.w - layout::kPad * 2,
                  layout::kLineSmall},
             theme::kMute);
    y += 22.0f;

    g_app.driveHits.clear();
    for (size_t i = 0; i < g_app.volumes.size(); ++i) {
        const Rect card{area.x + layout::kPad, y,
                        area.w - layout::kPad * 2.0f, layout::kDriveCardH};
        if (card.bottom() > area.bottom() - 200.0f) break;
        DrawDriveCard(g_app.volumes[i], card,
                      static_cast<int>(i) == g_app.selected,
                      static_cast<int>(i));
        y += layout::kDriveCardH + 8.0f;
    }

    if (g_app.cells.empty() && g_app.fileList.empty()) { return; }

    if (g_app.panelDirty) RefreshPanel();

    y += 10.0f;

    // --- tabs
    const wchar_t* labels[4] = {L"Kinds", L"Largest", L"Find", L"Dupes"};
    const float tabW = (area.w - layout::kPad * 2.0f) / 4.0f;
    g_app.panelTabs.clear();

    for (int i = 0; i < 4; ++i) {
        const Rect tab{area.x + layout::kPad + tabW * static_cast<float>(i), y,
                       tabW, 24.0f};
        g_app.panelTabs.push_back(tab);

        const bool active = (static_cast<int>(g_app.panel) == i);
        DrawText(labels[i], g_app.fmtSmall.get(),
                 Rect{tab.x, tab.y + 3.0f, tab.w, layout::kLineSmall},
                 active ? theme::kType : theme::kMute, 1.0f, false,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    // The underline slides between tabs rather than jumping. It is the only
    // thing on screen that moves when the panel changes, so it carries the
    // whole transition.
    {
        const int idx = static_cast<int>(g_app.panel);
        const Rect target{area.x + layout::kPad + tabW * static_cast<float>(idx),
                          y + 22.0f, tabW, 2.0f};
        Rect bar = target;
        if (g_app.tabSlide.Running()) {
            const float t = ease::OutQuint(g_app.tabSlide.Raw());
            bar = Lerp(g_app.tabFrom, target, t);
        }
        g_app.tabTo = target;
        FillRound(bar, 1.0f, theme::kSignal);
    }
    y += 30.0f;

    const float rowW = area.w - layout::kPad * 2.0f;
    g_app.rowHits.clear();
    // Cleared every frame and repopulated only by the panel that owns them,
    // or a stale rect from the Dupes tab would keep catching clicks after
    // the user moved to another one.
    g_app.dupeButton = Rect{};
    g_app.dupeAllButton = Rect{};
    g_app.dupeStopHit = Rect{};
    g_app.dupeBulkButton = Rect{};
    g_app.dupeRowHits.clear();
    g_app.dupeRowPaths.clear();
    g_app.dupeRowRef.clear();

    // --- duplicates
    if (g_app.panel == App::Panel::Dupes) {
        if (g_app.bulkRunning) {
            DrawText(L"Recycling extras\u2026", g_app.fmtSmall.get(),
                     Rect{area.x + layout::kPad, y, rowW,
                          layout::kLineSmall},
                     theme::kSignal, 1.0f, true);
            y += 20.0f;
            DrawText(FormatFiles(g_app.dupeProgress.files.load()) +
                         L" recycled  \u00B7  " +
                         FormatSize(g_app.dupeProgress.bytes.load()) +
                         L" verified",
                     g_app.fmtSmall.get(),
                     Rect{area.x + layout::kPad, y, rowW,
                          layout::kLineSmall},
                     theme::kMute, 1.0f, true);
            y += 18.0f;
            if (!g_app.dupeCurrentFile.empty()) {
                const size_t cut = g_app.dupeCurrentFile.find_last_of(L'\\');
                const std::wstring leaf =
                    (cut == std::wstring::npos)
                        ? g_app.dupeCurrentFile
                        : g_app.dupeCurrentFile.substr(cut + 1);
                DrawText(SanitizeForDisplay(leaf), g_app.fmtSmall.get(),
                         Rect{area.x + layout::kPad, y, rowW,
                              layout::kLineSmall},
                         theme::kMute, 0.75f, true);
                y += 18.0f;
            }
            g_app.dupeStopHit = Rect{area.x + layout::kPad, y, rowW, 15.0f};
            DrawText(L"Esc to stop \u00B7 or click here", g_app.fmtSmall.get(),
                     g_app.dupeStopHit,
                     theme::kMute, 0.85f, true);
            return;
        }
        if (g_app.dupeRunning) {
            // Live counters, so a long hunt visibly progresses instead of
            // looking like a hang - which is what it was before it moved
            // off this thread.
            DrawText(L"Reading candidates\u2026", g_app.fmtSmall.get(),
                     Rect{area.x + layout::kPad, y, rowW,
                          layout::kLineSmall},
                     theme::kSignal, 1.0f, true);
            y += 20.0f;
            const uint64_t nf =
                g_app.dupeProgress.files.load(std::memory_order_relaxed);
            const uint64_t nb =
                g_app.dupeProgress.bytes.load(std::memory_order_relaxed);
            DrawText(FormatFiles(nf) + L"  \u00B7  " + FormatSize(nb),
                     g_app.fmtSmall.get(),
                     Rect{area.x + layout::kPad, y, rowW,
                          layout::kLineSmall},
                     theme::kMute, 1.0f, true);
            y += 20.0f;
            if (!g_app.dupeCurrentFile.empty()) {
                const size_t cut = g_app.dupeCurrentFile.find_last_of(L'\\');
                const std::wstring leaf =
                    (cut == std::wstring::npos)
                        ? g_app.dupeCurrentFile
                        : g_app.dupeCurrentFile.substr(cut + 1);
                DrawText(SanitizeForDisplay(leaf), g_app.fmtSmall.get(),
                         Rect{area.x + layout::kPad, y, rowW,
                              layout::kLineSmall},
                         theme::kMute, 0.75f, true);
                y += 18.0f;
            }
            g_app.dupeStopHit = Rect{area.x + layout::kPad, y, rowW, 15.0f};
            DrawText(L"Esc to stop \u00B7 or click here", g_app.fmtSmall.get(),
                     g_app.dupeStopHit,
                     theme::kMute, 0.85f, true);
            return;
        }
        if (!g_app.dupesRun) {
            // Deliberately not automatic. Everything else in this program
            // reads directory entries; this reads file contents, and doing
            // that to a whole volume because a tab was clicked would be a
            // surprise with a real cost.
            g_app.dupeButton = Rect{area.x + layout::kPad, y, rowW, 26.0f};
            FillRound(g_app.dupeButton, 4.0f, theme::kSlabHi);
            DrawText(L"Find duplicates here", g_app.fmtSmall.get(),
                     Rect{g_app.dupeButton.x, g_app.dupeButton.y + 5.0f,
                          rowW, layout::kLineSmall},
                     theme::kType, 1.0f, false,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            y += 32.0f;
            g_app.dupeAllButton = Rect{area.x + layout::kPad, y, rowW, 26.0f};
            FillRound(g_app.dupeAllButton, 4.0f, theme::kSlab);
            DrawText(L"Across every scanned drive", g_app.fmtSmall.get(),
                     Rect{g_app.dupeAllButton.x,
                          g_app.dupeAllButton.y + 5.0f, rowW,
                          layout::kLineSmall},
                     theme::kType, 1.0f, false,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            y += 32.0f;
            DrawText(L"Reads the files that share a size with another. "
                     L"Cloud files are never downloaded.",
                     g_app.fmtSmall.get(),
                     Rect{area.x + layout::kPad, y, rowW,
                          layout::kLineSmall * 2.0f},
                     theme::kMute);
            return;
        }

        // Results are up, but the hunt must stay repeatable: files change,
        // drives change, and a report with no way to refresh it is a dead
        // end. Two compact buttons share the row the tall ones had.
        {
            const float half = (rowW - 6.0f) / 2.0f;
            g_app.dupeButton = Rect{area.x + layout::kPad, y, half, 22.0f};
            g_app.dupeAllButton =
                Rect{area.x + layout::kPad + half + 6.0f, y, half, 22.0f};
            FillRound(g_app.dupeButton, 4.0f, theme::kSlabHi);
            FillRound(g_app.dupeAllButton, 4.0f, theme::kSlab);
            DrawText(L"Hunt again", g_app.fmtSmall.get(),
                     Rect{g_app.dupeButton.x, g_app.dupeButton.y + 3.0f,
                          half, layout::kLineSmall},
                     theme::kType, 1.0f, false,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            DrawText(L"All drives", g_app.fmtSmall.get(),
                     Rect{g_app.dupeAllButton.x, g_app.dupeAllButton.y + 3.0f,
                          half, layout::kLineSmall},
                     theme::kType, 1.0f, false,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            y += 28.0f;
        }

        std::wstring head = FormatSize(g_app.dupes.totalWasted) +
                            L" recoverable in " +
                            FormatCount(g_app.dupes.groups.size()) +
                            L" sets";
        // A stopped hunt has seen only part of the tree, and presenting a
        // partial answer as a complete one is the kind of quiet lie this
        // program tries not to tell.
        if (g_app.dupes.cancelled) head += L"  (stopped early)";
        DrawText(head, g_app.fmtSmall.get(),
                 Rect{area.x + layout::kPad, y, rowW, layout::kLineSmall},
                 theme::kSignal, 1.0f, true);
        y += 20.0f;

        if (g_app.dupes.skippedCloud > 0 || g_app.dupes.skippedUnread > 0) {
            DrawText(L"skipped " + FormatCount(g_app.dupes.skippedCloud) +
                         L" cloud, " +
                         FormatCount(g_app.dupes.skippedUnread) +
                         L" unreadable",
                     g_app.fmtSmall.get(),
                     Rect{area.x + layout::kPad, y, rowW,
                          layout::kLineSmall},
                     theme::kMute, 0.9f, true);
            y += 18.0f;
        }
        DrawText(L"click a file to see it on the map \u00B7 "
                 L"right-click: Explorer, recycle",
                 g_app.fmtSmall.get(),
                 Rect{area.x + layout::kPad, y, rowW, layout::kLineSmall},
                 theme::kMute, 0.8f, true);
        y += 18.0f;
        if (!g_app.dupes.groups.empty()) {
            g_app.dupeBulkButton =
                Rect{area.x + layout::kPad, y, rowW, 22.0f};
            FillRound(g_app.dupeBulkButton, 4.0f, theme::kSlab);
            DrawText(L"Recycle every extra copy\u2026", g_app.fmtSmall.get(),
                     Rect{g_app.dupeBulkButton.x,
                          g_app.dupeBulkButton.y + 3.0f, rowW,
                          layout::kLineSmall},
                     theme::kType, 0.95f, false,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            y += 28.0f;
        }

        g_app.dupeRowHits.clear();
        g_app.dupeRowPaths.clear();
        g_app.dupeRowRef.clear();

        for (size_t gi = 0; gi < g_app.dupes.groups.size(); ++gi) {
            const DupGroup& g = g_app.dupes.groups[gi];
            if (y + 20.0f > area.bottom() - layout::kPad) break;
            DrawText(FormatSize(g.size) + L"  x" +
                         FormatCount(g.files.size()) + L"  (" +
                         FormatSize(g.wasted) + L" spare)",
                     g_app.fmtSmall.get(),
                     Rect{area.x + layout::kPad, y, rowW,
                          layout::kLineSmall},
                     theme::kType);
            y += 16.0f;
            for (size_t i = 0; i < g.files.size() && i < 4; ++i) {
                if (y + 16.0f > area.bottom() - layout::kPad) break;
                // The full path, because a pooled hunt spans volumes and
                // "copy1.bin" on its own would not say which drive.
                const std::wstring full = g.files[i].Full();
                const Rect row{area.x + layout::kPad - 2.0f, y,
                               rowW + 4.0f, 15.0f};
                g_app.dupeRowHits.push_back(row);
                g_app.dupeRowPaths.push_back(full);
                g_app.dupeRowRef.push_back(
                    {static_cast<int>(gi), static_cast<int>(i)});
                if (row.contains(g_app.mouseX, g_app.mouseY)) {
                    FillRound(row, 3.0f, theme::kSlabHi);
                }
                DrawText(SanitizeForDisplay(full), g_app.fmtSmall.get(),
                         Rect{area.x + layout::kPad + 10.0f, y,
                              rowW - 10.0f, layout::kLineSmall},
                         theme::kMute, 0.85f, true);
                y += 15.0f;
            }
            if (g.files.size() > 4) {
                DrawText(L"   +" + FormatCount(g.files.size() - 4) + L" more",
                         g_app.fmtSmall.get(),
                         Rect{area.x + layout::kPad + 10.0f, y,
                              rowW - 10.0f, layout::kLineSmall},
                         theme::kMute, 0.7f, true);
                y += 15.0f;
            }
            y += 6.0f;
        }
        return;
    }

    // --- search box
    if (g_app.panel == App::Panel::Search) {
        g_app.searchBox = Rect{area.x + layout::kPad, y, rowW, 26.0f};
        FillRound(g_app.searchBox, 4.0f, theme::kInk);
        if (g_app.searchSelectAll && !g_app.query.empty()) {
            // The whole query is selected; tint it the way every other text
            // box does, so replace-on-type is not a surprise.
            FillRound(Rect{g_app.searchBox.x + 4.0f, g_app.searchBox.y + 4.0f,
                           g_app.searchBox.w - 8.0f,
                           g_app.searchBox.h - 8.0f},
                      3.0f, theme::kSignal, 0.28f);
        }
        if (g_app.searchFocus) {
            SetBrush(theme::kSignal, 0.8f);
            const D2D1_ROUNDED_RECT rr{ToD2D(g_app.searchBox), 4.0f, 4.0f};
            g_app.rt->DrawRoundedRectangle(rr, g_app.brush.get(), 1.0f);
        }
        const std::wstring shown =
            g_app.query.empty()
                ? std::wstring(L"name, kind:media, ext:pak, >500mb, -temp")
                : g_app.query + (g_app.searchFocus ? L"|" : L"");
        DrawText(shown, g_app.fmtSmall.get(),
                 Rect{g_app.searchBox.x + 8.0f, g_app.searchBox.y + 4.0f,
                      rowW - 16.0f, layout::kLineSmall},
                 g_app.query.empty() ? theme::kMute : theme::kType);
        y += 32.0f;
    }

    // --- rows
    if (g_app.panel == App::Panel::Kinds) {
        for (const ExtStat& e : g_app.extStats) {
            if (y + 22.0f > area.bottom() - layout::kPad) break;

            const int ci = static_cast<int>(e.cat);
            const uint32_t colour =
                theme::kCat[(ci > 0 && ci < static_cast<int>(Cat::COUNT))
                                ? ci : static_cast<int>(Cat::Other)];
            FillRound(Rect{area.x + layout::kPad, y + 6.0f, 9.0f, 9.0f}, 2.0f,
                      colour);

            const Rect row{area.x + layout::kPad - 4.0f, y, rowW + 8.0f,
                           21.0f};
            g_app.rowHits.push_back(row);

            const std::wstring label =
                e.ext.empty() ? std::wstring(L"(none)") : L"." + e.ext;
            DrawText(label, g_app.fmtSmall.get(),
                     Rect{area.x + layout::kPad + 16.0f, y + 2.0f, 96.0f,
                          layout::kLineSmall},
                     theme::kType);
            DrawText(FormatSize(e.bytes), g_app.fmtSmall.get(),
                     Rect{area.right() - layout::kPad - 82.0f, y + 2.0f,
                          82.0f, layout::kLineSmall},
                     theme::kMute, 1.0f, true, DWRITE_TEXT_ALIGNMENT_TRAILING);
            y += 22.0f;
        }
        return;
    }

    // Largest / Find both render a file list.
    if (g_app.fileList.empty()) {
        std::wstring empty = L"No files here";
        if (g_app.panel == App::Panel::Search) {
            // Search is scoped to the directory being viewed, and the empty
            // states are where that is worth saying out loud.
            const std::wstring where =
                SanitizeForDisplay(TrailPath(g_app.trail));
            if (g_app.query.empty()) {
                empty = where.empty() ? std::wstring()
                                      : L"Searches " + where;
            } else {
                empty = L"Nothing matches in " +
                        (where.empty() ? std::wstring(L"this view") : where);
            }
        }
        DrawText(empty, g_app.fmtSmall.get(),
                 Rect{area.x + layout::kPad, y, rowW, layout::kLineSmall},
                 theme::kMute);
        return;
    }

    for (const FileHit& hit : g_app.fileList) {
        if (y + 34.0f > area.bottom() - layout::kPad) break;

        const Rect row{area.x + layout::kPad - 4.0f, y, rowW + 8.0f, 32.0f};
        const bool hot = (hit.node == g_app.hoverNode);
        if (hot) FillRound(row, 3.0f, theme::kSlabHi);
        g_app.rowHits.push_back(row);

        const int ci = static_cast<int>(hit.node->cat);
        const uint32_t colour =
            theme::kCat[(ci > 0 && ci < static_cast<int>(Cat::COUNT))
                            ? ci : static_cast<int>(Cat::Other)];
        FillRound(Rect{area.x + layout::kPad, y + 6.0f, 3.0f, 20.0f}, 1.5f,
                  colour);

        DrawText(SanitizeForDisplay(hit.node->name), g_app.fmtSmall.get(),
                 Rect{area.x + layout::kPad + 11.0f, y + 1.0f,
                      rowW - 11.0f, layout::kLineSmall},
                 theme::kType);
        // The parent folder rides along with the size: several results can
        // share one filename (pak00.pak in every game) and the name alone
        // does not say which one is eating the space.
        std::wstring detail = FormatSize(hit.size);
        if (hit.path.size() > hit.node->name.size() + 1) {
            detail += L"  \u00B7  " + SanitizeForDisplay(hit.path.substr(
                          0, hit.path.size() - hit.node->name.size() - 1));
        }
        DrawText(detail, g_app.fmtSmall.get(),
                 Rect{area.x + layout::kPad + 11.0f, y + 15.0f,
                      rowW - 11.0f, layout::kLineSmall},
                 theme::kMute, 0.9f, true);
        y += 34.0f;
    }
}

static void DrawBreadcrumb(const Rect& area) {
    g_app.crumbHits.clear();
    if (g_app.trail.empty()) return;

    const float avail = area.w - layout::kPad * 2.0f;

    // Measure every crumb first, then keep the deepest ones that fit. Laying
    // out head-first pushes the folder you are actually looking at off the
    // right edge once you are a few levels down.
    struct Crumb { std::wstring text; float w; };
    std::vector<Crumb> crumbs;
    crumbs.reserve(g_app.trail.size());

    for (const Node* n : g_app.trail) {
        Crumb c;
        c.text = SanitizeForDisplay(n->name);
        c.w = 90.0f;
        Com<IDWriteTextLayout> probe;
        if (SUCCEEDED(g_app.dwrite->CreateTextLayout(
                c.text.c_str(), static_cast<UINT32>(c.text.size()),
                g_app.fmtBody.get(), 420.0f, 20.0f, probe.put()))) {
            DWRITE_TEXT_METRICS tm{};
            probe->GetMetrics(&tm);
            c.w = std::min(tm.width + 2.0f, 250.0f);
        }
        crumbs.push_back(std::move(c));
    }

    constexpr float kSepW = 20.0f;
    const float kElideW = 22.0f;

    size_t first = 0;
    for (;;) {
        float total = (first > 0) ? kElideW : 0.0f;
        for (size_t i = first; i < crumbs.size(); ++i) {
            total += crumbs[i].w;
            if (i + 1 < crumbs.size()) total += kSepW;
        }
        if (total <= avail || first + 1 >= crumbs.size()) break;
        ++first;
    }

    float x = area.x + layout::kPad;
    const float baseY = area.y + 11.0f;

    if (first > 0) {
        DrawText(L"\u2026", g_app.fmtBody.get(),
                 Rect{x, baseY, kElideW, layout::kLineBody}, theme::kMute);
        x += kElideW;
    }

    for (size_t i = first; i < crumbs.size(); ++i) {
        const bool last = (i + 1 == crumbs.size());
        const Rect box{x, baseY, crumbs[i].w, layout::kLineBody};

        DrawText(crumbs[i].text, g_app.fmtBody.get(), box,
                 last ? theme::kType : theme::kMute);

        if (g_app.crumbHits.empty()) g_app.crumbFirst = i;
        g_app.crumbHits.push_back(Rect{x, area.y, crumbs[i].w, area.h});
        x += crumbs[i].w;

        if (!last) {
            DrawText(L"\u203A", g_app.fmtBody.get(),
                     Rect{x + 6.0f, baseY, 14.0f, layout::kLineBody},
                     theme::kRule);
            x += kSepW;
        }
    }
}

static void DrawCell(const Cell& c, const Rect& r, bool hovered,
                     float scaleY, float alpha = 1.0f,
                     float hoverT = 1.0f) {
    if (r.w < 1.0f || r.h < 1.0f) return;
    if (alpha <= 0.01f) return;

    const int catIdx = static_cast<int>(c.node->cat);
    const uint32_t base =
        theme::kCat[(catIdx >= 0 && catIdx < static_cast<int>(Cat::COUNT))
                        ? catIdx
                        : static_cast<int>(Cat::Other)];
    const uint32_t shade = ShadeForDepth(base, c.depth);
    const float fillA = (c.node->dir ? 0.32f : 0.92f) * alpha;

    // Hovered cells lift slightly as well as gaining an outline. The lift is
    // what makes the pointer feel attached to the map.
    const float lift = 0.10f * hoverT;
    FillRect(r, shade, std::min(1.0f, fillA + lift));

    // A single reused gradient gives every cell a top-lit edge, which is what
    // makes the map read as stacked blocks rather than flat colour fields.
    if (r.w > 6.0f && r.h > 6.0f && g_app.sheen && alpha > 0.9f) {
        g_app.sheen->SetStartPoint(D2D1_POINT_2F{r.x, r.y});
        g_app.sheen->SetEndPoint(D2D1_POINT_2F{r.x, r.y + r.h});
        g_app.rt->FillRectangle(ToD2D(r), g_app.sheen.get());
    }

    if (r.w > 3.0f && r.h > 3.0f) {
        StrokeRect(r, theme::kInk, (c.node->dir ? 0.65f : 0.45f) * alpha, 1.0f);
    }
    if (hovered && hoverT > 0.01f) {
        StrokeRect(r, theme::kSignal, hoverT, 1.0f + hoverT);
    }

    const float header = c.header * scaleY;

    if (c.expanded) {
        // Children are drawn over this cell's body. Its label goes in the
        // reserved strip, or nowhere at all if no strip was reserved --
        // drawing it in the body is what caused labels to pile up.
        if (header < 11.0f || r.w < 52.0f) return;

        const Rect strip{r.x, r.y, r.w, header};
        FillRect(strip, theme::kInk, 0.34f);

        const uint32_t ink = LabelInkFor(shade, fillA + 0.34f);
        const std::wstring size = FormatSize(c.node->size);

        // Size is right-aligned and the name is clipped short of it, so a long
        // directory name cannot run underneath its own size.
        float sizeW = 0.0f;
        if (r.w > 130.0f) {
            sizeW = std::min(72.0f, r.w * 0.34f);
            DrawText(size, g_app.fmtSmall.get(),
                     Rect{r.right() - sizeW - 5.0f, r.y, sizeW, header},
                     ink, 0.72f * alpha, true,
                     DWRITE_TEXT_ALIGNMENT_TRAILING);
        }

        DrawText(SanitizeForDisplay(c.node->name), g_app.fmtSmall.get(),
                 Rect{r.x + 5.0f, r.y,
                      r.w - 10.0f - sizeW - (sizeW > 0.0f ? 6.0f : 0.0f),
                      header},
                 ink, 0.95f * alpha);
        return;
    }

    // Leaf cell: nothing is drawn on top, so the body carries the label.
    if (r.w > 74.0f && r.h > 21.0f) {
        const uint32_t ink = LabelInkFor(shade, fillA);
        DrawText(SanitizeForDisplay(c.node->name), g_app.fmtSmall.get(),
                 Rect{r.x + 5.0f, r.y + 2.0f, r.w - 10.0f,
                      layout::kLineSmall}, ink, 0.94f * alpha);

        if (r.h > 38.0f) {
            DrawText(FormatSize(c.node->size), g_app.fmtSmall.get(),
                     Rect{r.x + 5.0f, r.y + 18.0f, r.w - 10.0f,
                          layout::kLineSmall}, ink, 0.66f * alpha, true);
        }
    }
}

static void DrawTreemap(const Rect& area) {
    FillRect(area, theme::kInk);

    // With a cached map on screen the rescan runs behind it; the overlay is
    // only for the blank first scan, when there is nothing better to show.
    if (g_app.scanning && g_app.cells.empty()) {
        const uint64_t f = g_app.progress.files.load(std::memory_order_relaxed);
        const uint64_t b = g_app.progress.bytes.load(std::memory_order_relaxed);

        const Rect centre{area.x, area.y + area.h * 0.42f, area.w,
                          layout::kLineHead};
        DrawText(L"Scanning", g_app.fmtHead.get(), centre, theme::kType, 1.0f,
                 false, DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(FormatFiles(f) + L"  \u00B7  " + FormatSize(b),
                 g_app.fmtBody.get(),
                 Rect{area.x, centre.bottom() + 6.0f, area.w,
                      layout::kLineBody},
                 theme::kMute, 1.0f, true, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    if (g_app.cells.empty()) {
        const Rect centre{area.x, area.y + area.h * 0.44f, area.w,
                          layout::kLineBody};
        DrawText(g_app.result ? L"Nothing large enough to map here"
                              : L"Pick a drive to map it",
                 g_app.fmtBody.get(), centre, theme::kMute, 1.0f, false,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    const float zoomT = ease::OutQuint(g_app.zoom.Raw());
    const bool  zooming = zoomT < 1.0f;

    const float revealRaw = g_app.reveal.Raw();
    const bool  revealing = revealRaw < 1.0f;

    const float sx = (g_app.mapBounds.w > 0.0f)
                         ? g_app.zoomFrom.w / g_app.mapBounds.w : 1.0f;
    const float sy = (g_app.mapBounds.h > 0.0f)
                         ? g_app.zoomFrom.h / g_app.mapBounds.h : 1.0f;

    // Hover strength is animated rather than binary, so sweeping the pointer
    // across the map does not strobe.
    const float hoverT = ease::OutCubic(g_app.hoverFade.Raw());

    // Deepest cell drawn, so the reveal stagger can be normalised against it.
    int maxDepth = 1;
    for (const Cell& c : g_app.cells) maxDepth = std::max(maxDepth, c.depth);

    for (const Cell& c : g_app.cells) {
        Rect r = c.rect;
        float scaleY = 1.0f;

        if (zooming) {
            const Rect start{
                g_app.zoomFrom.x + (c.rect.x - g_app.mapBounds.x) * sx,
                g_app.zoomFrom.y + (c.rect.y - g_app.mapBounds.y) * sy,
                c.rect.w * sx, c.rect.h * sy};
            r = Lerp(start, c.rect, zoomT);
            // The label strip must shrink with the cell, or a mid-animation
            // frame draws a strip taller than the cell holding it.
            scaleY = (c.rect.h > 0.01f) ? (r.h / c.rect.h) : 1.0f;
        }

        // Entrance: shallow cells settle first, deeper ones follow. The map
        // assembles outside-in, which reads as structure appearing rather
        // than as a single flat fade.
        float alpha = 1.0f;
        if (revealing) {
            const float lead =
                0.45f * static_cast<float>(c.depth) /
                static_cast<float>(maxDepth);
            const float local =
                (revealRaw - lead) / std::max(0.15f, 1.0f - lead);
            alpha = ease::OutCubic(std::max(0.0f, std::min(1.0f, local)));
            if (alpha <= 0.01f) continue;

            // A small settle inward as it fades up, anchored on the centre.
            const float shrink = (1.0f - alpha) * 0.06f;
            const float dx = r.w * shrink * 0.5f;
            const float dy = r.h * shrink * 0.5f;
            r = Rect{r.x + dx, r.y + dy, r.w - dx * 2.0f, r.h - dy * 2.0f};
        }

        const bool hot = !zooming && c.node == g_app.hoverNode;
        DrawCell(c, r, hot, scaleY, alpha, hot ? hoverT : 0.0f);

        // Duplicate markers, only while the Dupes tab is up: a thin amber
        // outline on every file the report names, and a brighter fading
        // one on the file a click just revealed.
        const size_t ci = static_cast<size_t>(&c - g_app.cells.data());
        if (g_app.panel == App::Panel::Dupes && ci < g_app.cellDupe.size() &&
            g_app.cellDupe[ci] != 0) {
            StrokeRect(r, theme::kSignal, 0.55f * alpha, 1.0f);
        }
        if (c.node == g_app.flashNode && g_app.dupeFlash.Running()) {
            const float t = 1.0f - g_app.dupeFlash.Raw();
            StrokeRect(r, theme::kSignal, 0.35f + 0.65f * t, 2.0f);
        }
    }
}

// How old a cached map is, in the coarsest unit that still reads as true.
// Precision is noise here: the point is "roughly now" versus "last week".
static std::wstring FormatAge(uint64_t savedUnixMs) {
    const uint64_t nowMs = UnixNowMs();
    if (nowMs <= savedUnixMs) return L"just now";

    const uint64_t s = (nowMs - savedUnixMs) / 1000;
    if (s < 90) return L"just now";
    if (s < 90 * 60) return std::to_wstring(s / 60) + L"m ago";
    if (s < 36 * 3600) return std::to_wstring(s / 3600) + L"h ago";
    return std::to_wstring(s / 86400) + L"d ago";
}

// One quiet clause, not a progress bar: the walk asks for no attention,
// but a disk light blinking with no explanation on screen reads as
// something being hidden.
static void AppendPrefetchNote(std::wstring& line) {
    if (!g_app.prefetching || g_app.prefetchRoot.size() < 2) return;
    line += L"  \u00B7  reading " + g_app.prefetchRoot.substr(0, 2) +
            L" behind  (" +
            FormatFiles(g_app.prefetchProgress.files.load()) + L")";
}

static void DrawStatus(const Rect& area) {
    FillRect(area, theme::kSlab, 0.7f);
    FillRect(Rect{area.x, area.y, area.w, 1.0f}, theme::kRule);

    const float pad = layout::kPad;

    if (g_app.hoverNode) {
        const std::wstring path =
            SanitizeForDisplay(CellPath(g_app.hoverIndex));
        DrawText(path, g_app.fmtSmall.get(),
                 Rect{area.x + pad, area.y + 8.0f,
                      area.w - pad * 2 - 220.0f, layout::kLineSmall},
                 theme::kType);

        std::wstring right = FormatSize(g_app.hoverNode->size);
        if (g_app.hoverNode->dir) {
            right += L"  \u00B7  " + FormatFiles(g_app.hoverNode->files);
        }
        DrawText(right, g_app.fmtSmall.get(),
                 Rect{area.right() - 214.0f, area.y + 8.0f, 200.0f,
                      layout::kLineSmall},
                 theme::kSignal, 1.0f, true);
        return;
    }

    // The cached map is on screen. Say so, and say that fresher data is on
    // its way - silently presenting yesterday's numbers as current would be
    // a lie of omission.
    if (g_app.result && g_app.showingCache) {
        const ScanStats& s = g_app.result->stats;
        std::wstring line = L"cached " + FormatAge(g_app.cacheSavedMs);
        if (g_app.scanning) line += L"  \u00B7  rescanning\u2026";
        line += L"  \u00B7  " + FormatFiles(s.fileCount) + L"  \u00B7  " +
                FormatSize(s.bytes);
        AppendPrefetchNote(line);
        DrawText(line, g_app.fmtSmall.get(),
                 Rect{area.x + pad, area.y + 8.0f, area.w - pad * 2,
                      layout::kLineSmall},
                 theme::kMute, 1.0f, true);
        return;
    }

    if (g_app.result) {
        const ScanStats& s = g_app.result->stats;
        wchar_t buf[64];
        const int written = std::swprintf(buf, 64, L"%.1fs", s.seconds);
        const std::wstring elapsed =
            (written > 0 && written < 64)
                ? std::wstring(buf, static_cast<size_t>(written))
                : std::wstring(L"--");

        std::wstring line = FormatFiles(s.fileCount) + L"  \u00B7  " +
                            FormatCount(s.dirCount) + L" folders  \u00B7  " +
                            FormatSize(s.bytes) + L"  \u00B7  " + elapsed;
        if (s.usedMft) line += L"  \u00B7  MFT";
        // Bytes that look reclaimable and are not. Saying nothing here is
        // how a tool promises 8 GB back from WinSxS and delivers none of it.
        if (s.hardlinkBytes > 0) {
            line += L"  \u00B7  " + FormatSize(s.hardlinkBytes) +
                    L" hardlinked";
        }
        if (s.cloudBytes > 0) {
            line += L"  \u00B7  " + FormatSize(s.cloudBytes) + L" in cloud";
        }
        if (s.deniedCount > 0) {
            line += L"  \u00B7  " + FormatCount(s.deniedCount) +
                    L" unreadable";
        }
        if (s.faulted) line += L"  \u00B7  incomplete";
        AppendPrefetchNote(line);
        DrawText(line, g_app.fmtSmall.get(),
                 Rect{area.x + pad, area.y + 8.0f, area.w - pad * 2,
                      layout::kLineSmall},
                 theme::kMute, 1.0f, true);
        return;
    }

    std::wstring idle =
        L"Click a drive to scan it. Click a block to go deeper, "
        L"backspace to come back. Ctrl+F to search, Ctrl+E to export.";
    AppendPrefetchNote(idle);
    DrawText(idle, g_app.fmtSmall.get(),
             Rect{area.x + pad, area.y + 8.0f, area.w - pad * 2,
                  layout::kLineSmall},
             theme::kMute);
}

// ------------------------------------------------------------------ resources

// Window DPI, resolved at runtime: GetDpiForWindow is Windows 10 1607+.
static float QueryDpiScale(HWND hwnd) {
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using GetDpiFn = UINT(WINAPI*)(HWND);
        const FARPROC raw = GetProcAddress(user32, "GetDpiForWindow");
        if (raw) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
            const auto fn = reinterpret_cast<GetDpiFn>(raw);
#pragma GCC diagnostic pop
            const UINT dpi = fn(hwnd);
            if (dpi >= 48 && dpi <= 480) {
                return static_cast<float>(dpi) / 96.0f;
            }
        }
    }
    return 1.0f;
}

static bool CreateDeviceResources() {
    if (g_app.rt) return true;
    if (!g_app.d2d) return false;

    RECT rc{};
    GetClientRect(g_app.hwnd, &rc);
    const D2D1_SIZE_U size{static_cast<UINT32>(std::max<LONG>(rc.right, 1)),
                           static_cast<UINT32>(std::max<LONG>(rc.bottom, 1))};

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                          D2D1_ALPHA_MODE_IGNORE);

    if (FAILED(g_app.d2d->CreateHwndRenderTarget(
            props, D2D1::HwndRenderTargetProperties(g_app.hwnd, size),
            g_app.rt.put()))) {
        return false;
    }

    // Grayscale rather than ClearType. Subpixel rendering puts coloured
    // fringes on glyph edges, which is unobtrusive on white and clearly
    // visible on a dark background -- especially on the muted greys used for
    // secondary text.
    g_app.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    // Declaring per-monitor awareness stops Windows scaling the window for us,
    // so Direct2D has to be told the real DPI or every dimension below is
    // interpreted at 96 and the whole interface renders undersized.
    const float dpi = g_app.dpiScale * 96.0f;
    g_app.rt->SetDpi(dpi, dpi);

    if (FAILED(g_app.rt->CreateSolidColorBrush(theme::Hex(theme::kType),
                                               g_app.brush.put()))) {
        return false;
    }

    D2D1_GRADIENT_STOP stops[2];
    stops[0].position = 0.0f;
    stops[0].color    = D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 0.09f};
    stops[1].position = 1.0f;
    stops[1].color    = D2D1_COLOR_F{0.0f, 0.0f, 0.0f, 0.13f};

    Com<ID2D1GradientStopCollection> coll;
    if (SUCCEEDED(g_app.rt->CreateGradientStopCollection(
            stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, coll.put()))) {
        g_app.rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1_POINT_2F{0, 0},
                                                D2D1_POINT_2F{0, 1}),
            coll.get(), g_app.sheen.put());
    }
    return true;
}

static void DiscardDeviceResources() {
    g_app.sheen.reset();
    g_app.brush.reset();
    g_app.rt.reset();
}

static bool CreateTextFormats() {
    struct Spec {
        Com<IDWriteTextFormat>* out;
        float                   size;
        DWRITE_FONT_WEIGHT      weight;
    };
    struct Spec2 { Com<IDWriteTextFormat>* out; float size;
                   DWRITE_FONT_WEIGHT weight; float line; };
    const Spec2 specs[] = {
        {&g_app.fmtBody,  14.0f, DWRITE_FONT_WEIGHT_NORMAL,    layout::kLineBody},
        {&g_app.fmtSmall, 12.0f, DWRITE_FONT_WEIGHT_NORMAL,    layout::kLineSmall},
        {&g_app.fmtHead,  19.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, layout::kLineHead},
        {&g_app.fmtNum,   12.0f, DWRITE_FONT_WEIGHT_NORMAL,    layout::kLineSmall},
    };

    for (const Spec2& s : specs) {
        if (FAILED(g_app.dwrite->CreateTextFormat(
                L"Segoe UI", nullptr, s.weight, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, s.size, L"en-us", s.out->put()))) {
            return false;
        }
        (*s.out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        // Centred vertically inside whatever rect it is given, with a fixed
        // line box. A rect that is a little too generous now centres the text
        // rather than pinning it to the top, and one that is a little too
        // tight can no longer crop it.
        (*s.out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        (*s.out)->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, s.line,
                                 s.line * 0.78f);
        // An ellipsis sign makes truncation legible. A hard character clip
        // leaves a half-word that reads like a different name entirely.
        DWRITE_TRIMMING trim{};
        trim.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;

        Com<IDWriteInlineObject> sign;
        if (SUCCEEDED(g_app.dwrite->CreateEllipsisTrimmingSign(s.out->get(),
                                                               sign.put()))) {
            (*s.out)->SetTrimming(&trim, sign.get());
        } else {
            (*s.out)->SetTrimming(&trim, nullptr);
        }
    }

    if (SUCCEEDED(g_app.dwrite->CreateTypography(g_app.typoTabular.put()))) {
        const DWRITE_FONT_FEATURE f{
            DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1};
        g_app.typoTabular->AddFontFeature(f);
    }
    return true;
}

// ----------------------------------------------------------------- rendering

static void Render() {
    if (!CreateDeviceResources()) return;

    const D2D1_SIZE_F sz = g_app.rt->GetSize();

    // BeginDraw and EndDraw must pair even when something between them
    // throws. They did not: an allocation failure mid-frame unwound past
    // EndDraw, and the next frame's BeginDraw then failed with
    // D2DERR_WRONG_STATE for ever, so the window stopped updating
    // permanently. The guard that was meant to make a dropped frame
    // harmless was what made it terminal.
    struct DrawScope {
        ID2D1HwndRenderTarget* rt;
        ~DrawScope() {
            if (!rt) return;
            const HRESULT hr = rt->EndDraw();
            if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
                DiscardDeviceResources();
            }
        }
    } scope{g_app.rt.get()};

    g_app.rt->BeginDraw();
    g_app.rt->Clear(theme::Hex(theme::kInk));

    const Rect side{0, 0, layout::kSidebar, sz.height};
    const Rect main{layout::kSidebar, 0, sz.width - layout::kSidebar,
                    sz.height};
    const Rect crumb{main.x, 0, main.w, layout::kCrumbH};
    const Rect status{main.x, sz.height - layout::kStatusH, main.w,
                      layout::kStatusH};
    const Rect map{main.x, layout::kCrumbH, main.w,
                   sz.height - layout::kCrumbH - layout::kStatusH};

    const bool boundsChanged =
        std::fabs(map.w - g_app.mapBounds.w) > 0.5f ||
        std::fabs(map.h - g_app.mapBounds.h) > 0.5f ||
        std::fabs(map.x - g_app.mapBounds.x) > 0.5f ||
        std::fabs(map.y - g_app.mapBounds.y) > 0.5f;

    if (boundsChanged && !g_app.trail.empty()) {
        if (g_app.resizing && !g_app.cells.empty()) {
            ScaleCells(g_app.mapBounds, map);
            g_app.mapBounds = map;
        } else {
            g_app.mapBounds = map;
            RebuildTreemap();
        }
    } else {
        g_app.mapBounds = map;
    }

    DrawSidebar(side);
    DrawBreadcrumb(crumb);
    FillRect(Rect{crumb.x, crumb.bottom() - 1.0f, crumb.w, 1.0f}, theme::kRule);
    DrawTreemap(map);
    DrawStatus(status);

    // EndDraw runs in ~DrawScope, on the way out of this function by any
    // route.
}

// ------------------------------------------------------------------ commands

static void CopyPathToClipboard(const std::wstring& path) {
    if (path.empty()) return;
    if (!OpenClipboard(g_app.hwnd)) return;

    EmptyClipboard();
    const size_t bytes = (path.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* dst = GlobalLock(h)) {
            memcpy(dst, path.c_str(), bytes);
            GlobalUnlock(h);
            if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

static void RevealInExplorer(const std::wstring& path) {
    if (path.empty()) return;

    // Hand Explorer an item, not a command line. Building "/select,\"...\""
    // meant a filename containing a quote could close the argument and
    // append another - and explorer.exe opening a second item means running
    // it, if that item is a program. Win32 forbids a quote in a name, but
    // names here come off raw NTFS structures and out of a cache file, so
    // "forbidden" is not the same as "cannot happen".
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0,
                                     nullptr)) &&
        pidl != nullptr) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        CoTaskMemFree(pidl);
        return;
    }
    // Could not resolve it: open the containing folder rather than
    // constructing a command line as a fallback.
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) return;
    const std::wstring parent = path.substr(0, slash);
    if (parent.find(L'"') != std::wstring::npos) return;
    ShellExecuteW(g_app.hwnd, L"open", parent.c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
}

// Walk the current tree to `full`, retarget the map at its parent and flash
// the file's cell. Returns false when the tree on screen does not contain
// the path (wrong drive, or the tree is older than the report).
static bool NavigateToPath(const std::wstring& full) {
    if (!g_app.result) return false;
    const Node* root = &g_app.result->root;
    const std::wstring& rootName = root->name;
    if (rootName.empty() || full.size() <= rootName.size()) return false;
    if (CompareStringOrdinal(full.c_str(),
                             static_cast<int>(rootName.size()),
                             rootName.c_str(),
                             static_cast<int>(rootName.size()),
                             TRUE) != CSTR_EQUAL) {
        return false;
    }

    size_t pos = rootName.size();
    if (pos < full.size() && full[pos] == L'\\') ++pos;

    std::vector<const Node*> trail{root};
    const Node* cur      = root;
    const Node* fileNode = nullptr;
    while (pos < full.size()) {
        const size_t sep = full.find(L'\\', pos);
        const std::wstring comp =
            (sep == std::wstring::npos) ? full.substr(pos)
                                        : full.substr(pos, sep - pos);
        pos = (sep == std::wstring::npos) ? full.size() : sep + 1;
        if (comp.empty()) continue;

        const Node* child = nullptr;
        for (const Node& c : cur->children) {
            if (lstrcmpiW(c.name.c_str(), comp.c_str()) == 0) {
                child = &c;
                break;
            }
        }
        if (!child) return false;
        if (child->dir) {
            trail.push_back(child);
            cur = child;
        } else {
            fileNode = child;
            break;
        }
    }
    if (!fileNode) return false;

    g_app.trail      = std::move(trail);
    g_app.hoverNode  = nullptr;
    g_app.hoverPrev  = nullptr;
    g_app.hoverIndex = -1;
    RebuildTreemap();
    g_app.panelDirty = true;
    g_app.flashNode  = fileNode;
    g_app.dupeFlash.Begin(g_app.motion ? 1600 : 800);
    SetTimer(g_app.hwnd, kTimerId, kFrameMs, nullptr);
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
    return true;
}

// Show a duplicate where it lives in this program: on the map. Switches
// drive when the file is on another volume (instant when its cache is
// fresh, which the launch prefetch makes the normal case). Explorer is the
// fallback for a path the trees cannot serve, and stays available from the
// right-click menu regardless.
static void ShowDupeInMap(const std::wstring& full) {
    if (NavigateToPath(full)) return;

    if (full.size() >= 3 && full[1] == L':' && full[2] == L'\\') {
        for (size_t i = 0; i < g_app.volumes.size(); ++i) {
            const std::wstring& vp = g_app.volumes[i].path;
            if (!vp.empty() &&
                towupper(vp[0]) == towupper(full[0])) {
                StartScan(static_cast<int>(i));
                if (NavigateToPath(full)) return;
                // The tree is on its way; finish when it lands.
                g_app.pendingReveal = full;
                return;
            }
        }
    }
    RevealInExplorer(full);
}

// Paste into the search box: first line only, printable characters only, the
// same 128-character cap as typing. Clipboard text is as attacker-controlled
// as filenames are.
static void PasteIntoSearch() {
    if (!OpenClipboard(g_app.hwnd)) return;
    if (const HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* text = static_cast<const wchar_t*>(GlobalLock(h))) {
            if (g_app.searchSelectAll) {
                g_app.query.clear();
                g_app.searchSelectAll = false;
            }
            for (const wchar_t* p = text; *p != 0; ++p) {
                if (*p == L'\r' || *p == L'\n') break;
                if (*p < 0x20 || *p == 0x7F) continue;
                if (g_app.query.size() >= 128) break;
                g_app.query.push_back(*p);
            }
            GlobalUnlock(h);
            g_app.panelDirty = true;
            InvalidateRect(g_app.hwnd, nullptr, FALSE);
        }
    }
    CloseClipboard();
}

// CSV export, the interchange format every comparable tool offers.
static void DoExport() {
    if (g_app.trail.empty() || !g_app.result) return;

    wchar_t file[MAX_PATH] = L"spindle-export.csv";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_app.hwnd;
    ofn.lpstrFilter = L"CSV file\0*.csv\0All files\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = L"csv";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
                      OFN_NOCHANGEDIR;

    if (!GetSaveFileNameW(&ofn)) return;

    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const bool ok = ExportCsv(*g_app.trail.back(), TrailPath(g_app.trail),
                              file);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    if (!ok) {
        MessageBoxW(g_app.hwnd, L"Could not write that file. Check the "
                                L"folder is writable and try again.",
                    L"Spindle", MB_OK | MB_ICONWARNING);
    }
}

// What changed since the cache was written. The cache holds the previous
// finished scan, so this costs nothing but the comparison itself.
static void ShowDiffAgainstCache() {
    if (!g_app.result || g_app.selected < 0 ||
        g_app.selected >= static_cast<int>(g_app.volumes.size())) {
        MessageBoxW(g_app.hwnd, L"Scan a drive first.", L"Spindle",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    const std::wstring volume =
        g_app.volumes[static_cast<size_t>(g_app.selected)].path;

    ScanResult previous;
    CacheMeta meta;
    if (!LoadScanCache(volume, previous, meta)) {
        MessageBoxW(g_app.hwnd,
                    L"There is no cached scan of this drive to compare "
                    L"against yet. Scan it once more and the comparison "
                    L"will have something to work from.",
                    L"Spindle", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // 1 MB floor: below that a listing is noise rather than an answer.
    const DiffReport diff =
        DiffTrees(previous.root, g_app.result->root, 1u << 20);

    std::wstring text = L"Since the cached scan (" +
                        FormatAge(meta.savedUnixMs) + L"):\n\n";
    if (diff.changes.empty()) {
        text += L"Nothing moved by more than a megabyte.";
    } else {
        text += L"grew " + FormatSize(diff.grewBy) + L", shrank " +
                FormatSize(diff.shrankBy) + L"\n\n";
        for (size_t i = 0; i < diff.changes.size() && i < 20; ++i) {
            const Change& c = diff.changes[i];
            const uint64_t mag = (c.delta < 0)
                                     ? static_cast<uint64_t>(-c.delta)
                                     : static_cast<uint64_t>(c.delta);
            text += (c.delta < 0 ? L"  -" : L"  +");
            text += FormatSize(mag);
            text += L"  ";
            text += ChangeKindName(c.kind);
            text += L"  ";
            text += SanitizeForDisplay(c.path.empty() ? volume : c.path);
            text += L'\n';
        }
        if (diff.changes.size() > 20) {
            text += L"\n...and " + FormatCount(diff.changes.size() - 20) +
                    L" more.";
        }
    }
    MessageBoxW(g_app.hwnd, text.c_str(), L"Since last scan", MB_OK);
}

// Everything that is not the map: about, the key list, and the only two
// settings the program actually has. A native popup, not a dialog - the
// same chrome budget as the rest of the interface.
static void ShowAppMenu(POINT screenPt) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING, 1, L"About Spindle");
    AppendMenuW(menu, MF_STRING, 2, L"Controls");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu,
                MF_STRING | (g_app.settings.keepCaches ? MF_CHECKED : 0u), 3,
                L"Keep scan caches");
    AppendMenuW(menu,
                MF_STRING | (g_app.settings.resumeOnLaunch ? MF_CHECKED : 0u),
                4, L"Resume last drive at launch");
    AppendMenuW(menu,
                MF_STRING | (g_app.settings.prefetchAll ? MF_CHECKED : 0u),
                9, L"Read every drive at launch");
    AppendMenuW(menu,
                MF_STRING | (ShellVerbRegistered() ? MF_CHECKED : 0u), 7,
                L"Show in Explorer's folder menu");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 8, L"Compare with the cached scan...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 5, L"Open cache folder");
    AppendMenuW(menu, MF_STRING, 6, L"Clear scan caches");

    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                   screenPt.x, screenPt.y, 0, g_app.hwnd,
                                   nullptr);
    DestroyMenu(menu);

    switch (cmd) {
        case 1: {
            const std::wstring about =
                std::wstring(L"Spindle ") + kAppVersion +
                L"\n\nDisk space, mapped. Native Win32 and Direct2D, one "
                L"portable executable, nothing installed.\n\n"
                L"github.com/thetrueartist/spindle";
            MessageBoxW(g_app.hwnd, about.c_str(), L"About Spindle", MB_OK);
            break;
        }
        case 2:
            MessageBoxW(g_app.hwnd,
                        L"Click a drive - scan it\n"
                        L"Click a block - descend into it\n"
                        L"Backspace - go up a level\n"
                        L"Click a breadcrumb - jump to that level\n"
                        L"Right-click a block - Explorer, copy path,\n"
                        L"                       recycle, force remove\n"
                        L"Ctrl+F - search names\n"
                        L"Ctrl+E - export CSV\n"
                        L"F5 - rescan\n"
                        L"Esc - cancel a running scan",
                        L"Controls", MB_OK);
            break;
        case 3:
            g_app.settings.keepCaches = !g_app.settings.keepCaches;
            SaveSettings(g_app.settings);
            break;
        case 4:
            g_app.settings.resumeOnLaunch = !g_app.settings.resumeOnLaunch;
            SaveSettings(g_app.settings);
            break;
        case 9:
            g_app.settings.prefetchAll = !g_app.settings.prefetchAll;
            SaveSettings(g_app.settings);
            if (g_app.settings.prefetchAll) {
                // Turned on mid-session: start now rather than making the
                // toggle a promise about some future launch.
                std::wstring cur;
                if (g_app.selected >= 0 &&
                    g_app.selected < static_cast<int>(g_app.volumes.size())) {
                    cur = g_app.volumes[static_cast<size_t>(g_app.selected)]
                              .path;
                }
                QueueLaunchPrefetch(cur);
            } else {
                CancelPrefetch(false);
                g_app.prefetchQueue.clear();
            }
            break;
        case 5: {
            const std::wstring dir = CacheDirPath();
            if (!dir.empty()) {
                ShellExecuteW(g_app.hwnd, L"open", L"explorer.exe",
                              (L"\"" + dir + L"\"").c_str(), nullptr,
                              SW_SHOWNORMAL);
            }
            break;
        }
        case 6:
            ClearScanCaches();
            break;
        case 7: {
            // The only registry write in the program, and only ever because
            // this was ticked. Unticking removes exactly what it added.
            const bool on = ShellVerbRegistered();
            const bool ok = on ? UnregisterShellVerb() : RegisterShellVerb();
            if (!ok) {
                MessageBoxW(g_app.hwnd,
                            L"Could not change the Explorer menu entry.",
                            L"Spindle", MB_OK | MB_ICONWARNING);
            }
            break;
        }
        case 8:
            ShowDiffAgainstCache();
            break;
        default:
            break;
    }
}

// Force removal: permanent, no Recycle Bin, and it will terminate the
// processes holding the target open. Everything about this function is
// about making that impossible to do by accident - the deletion itself is
// three lines at the bottom.
// Takes the node's figures by value rather than a Node*, so no caller can
// reintroduce a pointer that outlives the tree across a modal dialog.
static void DoForceRemove(const std::wstring& path, uint64_t nodeSize,
                          uint32_t nodeFiles, bool nodeDir) {
    if (path.empty()) return;

    // First gate: the paths that are never removable, whatever anyone
    // clicks. ForceRemove refuses these again on its own.
    if (IsProtectedSystemPath(path)) {
        MessageBoxW(g_app.hwnd,
                    (L"Spindle will not force-remove this:\n\n" +
                     SanitizeForDisplay(path) +
                     L"\n\nIt is a drive root or part of Windows itself, or "
                     L"it is spelled in a way that resolves somewhere else.")
                        .c_str(),
                    L"Spindle", MB_OK | MB_ICONERROR);
        return;
    }

    // Second gate: say plainly what is about to happen, with the scale.
    // The path is sanitised: a filename carrying a right-to-left override
    // renders as something else entirely, and this is the dialog where the
    // user decides whether to destroy it.
    std::wstring warn =
        L"PERMANENTLY DELETE this? It does NOT go to the Recycle Bin and "
        L"cannot be undone.\n\n" + SanitizeForDisplay(path) + L"\n\n" +
        FormatSize(nodeSize);
    if (nodeDir) {
        warn += L" across " + FormatCount(nodeFiles) + L" files";
    }
    if (MessageBoxW(g_app.hwnd, warn.c_str(), L"Force remove",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    // Third gate: name the processes that would have to die, and let the
    // user stop here and close them properly instead. Critical processes
    // are listed as refused rather than offered.
    bool terminate = false;
    const std::vector<Locker> lockers = FindLockers(path);
    if (!lockers.empty()) {
        std::wstring msg =
            L"These programs currently have it open:\n\n";
        size_t killable = 0;
        for (size_t i = 0; i < lockers.size() && i < 12; ++i) {
            msg += L"    " + SanitizeForDisplay(lockers[i].name);
            if (lockers[i].critical) {
                msg += L"  (system - will not be touched)";
            } else {
                ++killable;
            }
            msg += L'\n';
        }
        if (lockers.size() > 12) {
            msg += L"    ...and " + FormatCount(lockers.size() - 12) +
                   L" more\n";
        }
        msg += killable > 0
                   ? L"\nEnd them and continue? Unsaved work in those "
                     L"programs will be lost.\n\nNo is the safe answer: "
                     L"close them yourself and try again."
                   : L"\nAll of them are system processes that Spindle will "
                     L"not end. The removal will probably fail.\n\n"
                     L"Continue anyway?";
        if (MessageBoxW(g_app.hwnd, msg.c_str(), L"Force remove",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
            return;
        }
        terminate = (killable > 0);
    }

    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const ForceRemoveResult r = ForceRemove(path, terminate);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    if (r.ok) {
        // The tree on screen still contains what was just removed, and the
        // cache describes the old state of exactly these files.
        StartScan(g_app.selected, false);
        return;
    }

    std::wstring failed =
        L"Removed " + FormatCount(r.filesDeleted) +
        L" items, then stopped.\n\n";
    if (r.blocked) {
        failed = L"Refused: that path is protected.\n\n";
    } else if (!r.remaining.empty()) {
        failed += L"Still held open by:\n";
        for (size_t i = 0; i < r.remaining.size() && i < 8; ++i) {
            failed += L"    " + SanitizeForDisplay(r.remaining[i].name) +
                      L'\n';
        }
    } else if (r.lastError == ERROR_ACCESS_DENIED) {
        failed += L"Access was denied. Running Spindle as administrator "
                  L"would let it take ownership.";
    } else if (r.lastError != 0) {
        failed += L"Windows error " + std::to_wstring(r.lastError) + L".";
    }
    MessageBoxW(g_app.hwnd, failed.c_str(), L"Force remove",
                MB_OK | MB_ICONWARNING);

    if (r.filesDeleted > 0) StartScan(g_app.selected, false);
}

// Right-clicking a duplicate row. Recycling a copy is gated on proving,
// byte for byte, that an identical copy remains - so the last copy can
// never be the one deleted, and a file is never removed on the strength of
// a hash alone. Reversible: it goes to the Recycle Bin.
static void ShowDupeMenu(int rowIndex, POINT screenPt) {
    if (rowIndex < 0 ||
        rowIndex >= static_cast<int>(g_app.dupeRowPaths.size()) ||
        rowIndex >= static_cast<int>(g_app.dupeRowRef.size())) {
        return;
    }
    if (g_app.dupeRunning) return;   // the report is being rebuilt

    const std::wstring path = g_app.dupeRowPaths[static_cast<size_t>(rowIndex)];
    const std::pair<int, int> ref =
        g_app.dupeRowRef[static_cast<size_t>(rowIndex)];

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, 1, L"Reveal in Explorer");
    AppendMenuW(menu, MF_STRING, 2, L"Recycle this copy (keep the others)");
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                   screenPt.x, screenPt.y, 0, g_app.hwnd,
                                   nullptr);
    DestroyMenu(menu);

    if (cmd == 1) { RevealInExplorer(path); return; }
    if (cmd != 2) return;

    // Locate a different member of the same group to fall back on.
    if (ref.first < 0 ||
        ref.first >= static_cast<int>(g_app.dupes.groups.size())) {
        return;
    }
    DupGroup& g = g_app.dupes.groups[static_cast<size_t>(ref.first)];
    std::wstring twin;
    for (size_t j = 0; j < g.files.size(); ++j) {
        const std::wstring cand = g.files[j].Full();
        if (cand != path) { twin = cand; break; }
    }
    if (twin.empty()) {
        MessageBoxW(g_app.hwnd,
                    L"There is no other copy to fall back on, so this will "
                    L"not be recycled.",
                    L"Spindle", MB_OK | MB_ICONWARNING);
        return;
    }

    // Prove it byte for byte before anything is touched. This reads both
    // files in full; for a very large pair it blocks briefly, with the wait
    // cursor to say so.
    g_app.dupeProgress.cancel.store(false, std::memory_order_relaxed);
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const bool identical =
        VerifyFilesIdentical(path, twin, &g_app.dupeProgress);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    if (!identical) {
        MessageBoxW(g_app.hwnd,
                    L"Could not confirm an identical copy still exists - the "
                    L"file may have changed since the search - so nothing "
                    L"was deleted.",
                    L"Spindle", MB_OK | MB_ICONWARNING);
        return;
    }

    const std::wstring prompt =
        L"Recycle this copy?\n\n" + SanitizeForDisplay(path) +
        L"\n\nA verified identical copy remains:\n" +
        SanitizeForDisplay(twin);
    if (MessageBoxW(g_app.hwnd, prompt.c_str(), L"Spindle",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    if (!RecycleToBin(path)) {
        MessageBoxW(g_app.hwnd,
                    L"Could not move that file to the Recycle Bin.",
                    L"Spindle", MB_OK | MB_ICONWARNING);
        return;
    }

    // Drop the recycled file from the report so the panel reflects it. Below
    // two members a group is no longer a duplicate and goes entirely.
    for (size_t j = 0; j < g.files.size(); ++j) {
        if (g.files[j].Full() == path) {
            g.files.erase(g.files.begin() + static_cast<long>(j));
            break;
        }
    }
    if (g.files.size() >= 2) {
        g.wasted = g.size * (g.files.size() - 1);
    } else {
        g_app.dupes.groups.erase(
            g_app.dupes.groups.begin() + ref.first);
    }
    RebuildDupePathSet();
    MarkDupeCells();
    g_app.dupes.totalWasted = 0;
    for (const DupGroup& grp : g_app.dupes.groups) {
        g_app.dupes.totalWasted += grp.wasted;
    }
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static void ShowCellMenu(int cellIndex, POINT screenPt) {
    if (cellIndex < 0 ||
        static_cast<size_t>(cellIndex) >= g_app.cells.size()) {
        return;
    }
    const Node* node = g_app.cells[static_cast<size_t>(cellIndex)].node;
    if (!node) return;
    const std::wstring path = CellPath(cellIndex);

    // Snapshot everything the dialogs below need. TrackPopupMenu and
    // MessageBoxW each run a modal message loop that dispatches posted
    // messages, and WM_SCAN_DONE is posted - so a rescan landing while the
    // menu is open frees the tree this node lives in. Reading a size out of
    // that freed memory to fill a delete confirmation is the worst place in
    // the program to be wrong.
    const uint64_t nodeSize  = node->size;
    const uint32_t nodeFiles = node->files;
    const bool     nodeDir   = node->dir;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING, 1, L"Show in Explorer");
    AppendMenuW(menu, MF_STRING, 2, L"Copy path");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // No keyboard shortcut on purpose: deleting should take deliberate
    // pointing, not a stray keypress. The ellipsis is the Windows convention
    // for "asks before doing anything".
    AppendMenuW(menu, MF_STRING, 3, L"Move to Recycle Bin...");
    // Force removal is permanent and can kill processes, so it sits below
    // the reversible option and never becomes the default action.
    AppendMenuW(menu, MF_STRING, 5, L"Force remove (permanent)...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 4, L"Export this folder to CSV\tCtrl+E");

    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                   screenPt.x, screenPt.y, 0, g_app.hwnd,
                                   nullptr);
    DestroyMenu(menu);

    if (cmd == 1) {
        RevealInExplorer(path);
    } else if (cmd == 2) {
        CopyPathToClipboard(path);
    } else if (cmd == 3) {
        // The same refusal the permanent path uses. The reversible option
        // guarding less than the irreversible one was backwards: the shell
        // canonicalises identically, and a folder recycled by mistake is
        // still a folder gone from where it was.
        if (IsProtectedSystemPath(path)) {
            MessageBoxW(g_app.hwnd,
                        L"Spindle will not recycle this. It is a drive root "
                        L"or part of Windows itself.",
                        L"Spindle", MB_OK | MB_ICONWARNING);
            return;
        }

        const std::wstring prompt =
            L"Move this to the Recycle Bin?\n\n" +
            SanitizeForDisplay(path) + L"\n\n" + FormatSize(nodeSize) +
            (nodeDir ? L" across " + FormatCount(nodeFiles) + L" files"
                     : L"");
        if (MessageBoxW(g_app.hwnd, prompt.c_str(), L"Spindle",
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
            return;
        }

        // A folder is a recursive delete, and one Yes is too cheap for
        // that. Folders ask twice, with the scale restated and No the
        // default both times.
        if (nodeDir) {
            const std::wstring again =
                L"That folder holds " + FormatCount(nodeFiles) +
                L" files (" + FormatSize(nodeSize) +
                L").\n\nMove all of it to the Recycle Bin?";
            if (MessageBoxW(g_app.hwnd, again.c_str(), L"Spindle",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) !=
                IDYES) {
                return;
            }
        }

        // SHFileOperationW takes a double-NUL-terminated list. Building it by
        // hand rather than relying on the string's own terminator, because a
        // missing second NUL makes the shell read past the buffer.
        std::vector<wchar_t> from(path.begin(), path.end());
        from.push_back(L'\0');
        from.push_back(L'\0');

        SHFILEOPSTRUCTW op{};
        op.hwnd   = g_app.hwnd;
        op.wFunc  = FO_DELETE;
        op.pFrom  = from.data();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING;

        if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted) {
            // Sizes are now stale; rescan - and not from the cache, which
            // still contains the files that were just recycled.
            StartScan(g_app.selected, false);
        }
    } else if (cmd == 4) {
        DoExport();
    } else if (cmd == 5) {
        DoForceRemove(path, nodeSize, nodeFiles, nodeDir);
    }
}

// -------------------------------------------------------------- window proc

static void OnMouseMove(int x, int y) {
    // Mouse coordinates are physical pixels; the layout is in DIPs.
    const float inv = (g_app.dpiScale > 0.0f) ? 1.0f / g_app.dpiScale : 1.0f;
    g_app.mouseX = static_cast<float>(x) * inv;
    g_app.mouseY = static_cast<float>(y) * inv;
    const float fx = static_cast<float>(x) * inv;
    const float fy = static_cast<float>(y) * inv;

    const Node* before = g_app.hoverNode;
    g_app.hoverNode  = nullptr;
    g_app.hoverIndex = -1;

    for (size_t i = 0; i < g_app.rowHits.size(); ++i) {
        if (g_app.rowHits[i].contains(fx, fy) && i < g_app.fileList.size()) {
            g_app.hoverNode = g_app.fileList[i].node;
            g_app.hoverIndex = -1;
            InvalidateRect(g_app.hwnd, nullptr, FALSE);
            return;
        }
    }

    if (!g_app.zoom.Running() && g_app.mapBounds.contains(fx, fy)) {
        const int idx = HitTestIndex(g_app.cells, fx, fy);
        if (idx >= 0) {
            const Cell& c = g_app.cells[static_cast<size_t>(idx)];
            g_app.hoverNode  = c.node;
            g_app.hoverIndex = idx;
            g_app.hoverRect  = c.rect;
        }
    }
    if (g_app.hoverNode != before) {
        if (g_app.hoverNode) {
            g_app.hoverPrev = g_app.hoverNode;
            g_app.hoverFade.Begin(g_app.motion ? kHoverMs : 0);
            SetTimer(g_app.hwnd, kTimerId, kFrameMs, nullptr);
        }
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
    }
}

static void OnLeftClick(int x, int y) {
    const float inv = (g_app.dpiScale > 0.0f) ? 1.0f / g_app.dpiScale : 1.0f;
    const float fx = static_cast<float>(x) * inv;
    const float fy = static_cast<float>(y) * inv;

    if (g_app.menuHit.contains(fx, fy)) {
        POINT pt{x, y};
        ClientToScreen(g_app.hwnd, &pt);
        ShowAppMenu(pt);
        return;
    }

    // Stop a running hunt or bulk recycle by mouse alone. Esc does the
    // same; a cancel that only a keyboard can reach is not much of a
    // cancel.
    if ((g_app.dupeRunning || g_app.bulkRunning) &&
        g_app.dupeStopHit.w > 0.0f && g_app.dupeStopHit.contains(fx, fy)) {
        g_app.dupeProgress.cancel.store(true, std::memory_order_relaxed);
        return;
    }

    // Recycle every extra copy: one copy of each set is kept, and every
    // extra is verified byte for byte against its kept copy immediately
    // before it goes to the bin. The whole run is reversible and can be
    // stopped; a set whose members no longer match is skipped whole.
    if (g_app.dupeBulkButton.w > 0.0f &&
        g_app.dupeBulkButton.contains(fx, fy) && !g_app.bulkRunning &&
        !g_app.dupeRunning) {
        if (g_app.scanning && !YieldScanToHunt()) return;

        size_t   extras = 0;
        uint64_t bytes  = 0;
        for (const DupGroup& grp : g_app.dupes.groups) {
            if (grp.files.size() < 2) continue;
            extras += grp.files.size() - 1;
            bytes  += grp.wasted;
        }
        if (extras == 0) return;

        const std::wstring ask =
            L"Recycle every extra copy?\n\n" + FormatCount(extras) +
            L" files across " + FormatCount(g_app.dupes.groups.size()) +
            L" sets, " + FormatSize(bytes) +
            L".\n\nThe first copy of each set is kept. Every extra is "
            L"verified byte for byte against the kept copy immediately "
            L"before it is recycled; a set that no longer matches is "
            L"skipped.\n\nEverything goes to the Recycle Bin.";
        if (MessageBoxW(g_app.hwnd, ask.c_str(), L"Spindle",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) !=
            IDYES) {
            return;
        }

        CancelPrefetch(true);
        auto req = std::make_unique<BulkRequest>();
        req->hwnd = g_app.hwnd;
        req->gen  = g_app.dupeGen.fetch_add(1) + 1;
        try {
            for (const DupGroup& grp : g_app.dupes.groups) {
                if (grp.files.size() < 2) continue;
                std::vector<std::pair<std::wstring, uint64_t>> g;
                g.reserve(grp.files.size());
                for (const DupFile& f : grp.files) {
                    g.push_back({f.Full(), grp.size});
                }
                req->groups.push_back(std::move(g));
            }
        } catch (...) {
            MessageBoxW(g_app.hwnd, L"Ran out of memory preparing the run.",
                        L"Spindle", MB_OK | MB_ICONWARNING);
            return;
        }

        g_app.dupeCurrentFile.clear();
        g_app.dupeProgress.files.store(0);
        g_app.dupeProgress.bytes.store(0);
        g_app.dupeProgress.cancel.store(false, std::memory_order_relaxed);

        const uintptr_t h =
            _beginthreadex(nullptr, 0, BulkThread, req.get(), 0, nullptr);
        if (h == 0) {
            MessageBoxW(g_app.hwnd, L"Could not start: the system refused "
                                    L"a new thread.",
                        L"Spindle", MB_OK | MB_ICONERROR);
            return;
        }
        static_cast<void>(req.release());  // NOLINT
        g_app.bulkWorker  = reinterpret_cast<HANDLE>(h);
        g_app.bulkRunning = true;
        SetTimer(g_app.hwnd, kTimerId, kFrameMs, nullptr);
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }

    // A duplicate row shows the file where it lives on the map, switching
    // drive if it has to. Each row carries its own full path, so this
    // works for a pooled hunt spanning drives as well as one folder.
    // Explorer and recycling stay on the right-click menu.
    for (size_t i = 0; i < g_app.dupeRowHits.size() &&
                       i < g_app.dupeRowPaths.size(); ++i) {
        if (g_app.dupeRowHits[i].contains(fx, fy)) {
            ShowDupeInMap(g_app.dupeRowPaths[i]);
            return;
        }
    }

    // Across every drive: pooled from the cached scans, so nothing is
    // re-walked. A file that exists once on C: and once on D: is exactly
    // the duplicate a single-folder hunt can never find.
    if (g_app.dupeAllButton.w > 0.0f &&
        g_app.dupeAllButton.contains(fx, fy) && !g_app.dupeRunning) {
        if (g_app.bulkRunning) return;
        if (g_app.scanning && !YieldScanToHunt()) return;
        CancelPrefetch(true);   // the hunt reads files; the walk can wait
        auto req = std::make_unique<DupRequest>();
        size_t drives = 0;
        try {
            for (const Volume& v : g_app.volumes) {
                ScanResult cached;
                CacheMeta meta;
                const Node* tree = nullptr;
                // The drive on screen is already in memory and fresher than
                // anything on disk; the rest come from their caches.
                if (g_app.result && g_app.selected >= 0 &&
                    g_app.selected < static_cast<int>(g_app.volumes.size()) &&
                    g_app.volumes[static_cast<size_t>(g_app.selected)].path ==
                        v.path) {
                    tree = &g_app.result->root;
                } else if (LoadScanCache(v.path, cached, meta)) {
                    tree = &cached.root;
                }
                if (!tree) continue;
                ++drives;
                // Collected, not filtered: the shared-size filter runs
                // once over the pool below, because a file whose only twin
                // is on another drive has a unique size on its own.
                std::vector<DupFile> part =
                    CollectDupFiles(*tree, v.path, kDefaultDupMinSize);
                for (DupFile& f : part) {
                    f.node = nullptr;   // `cached` dies at the end of this turn
                    req->candidates.push_back(std::move(f));
                }
            }
        } catch (...) {
            MessageBoxW(g_app.hwnd,
                        L"Ran out of memory gathering candidates.",
                        L"Spindle", MB_OK | MB_ICONWARNING);
            return;
        }
        if (drives == 0) {
            MessageBoxW(g_app.hwnd,
                        L"No scanned drives to compare yet. Scan a drive "
                        L"or two first - their results are remembered, and "
                        L"this then compares across all of them.",
                        L"Spindle", MB_OK | MB_ICONINFORMATION);
            return;
        }
        req->candidates = FilterBySharedSize(std::move(req->candidates));
        for (DupFile& f : req->candidates) f.node = nullptr;
        req->rootPath = std::wstring();   // each file carries its own volume
        req->hwnd     = g_app.hwnd;
        req->gen      = g_app.dupeGen.fetch_add(1) + 1;

        g_app.dupeCurrentFile.clear();
        g_app.dupeProgress.files.store(0);
        g_app.dupeProgress.bytes.store(0);
        g_app.dupeProgress.cancel.store(false, std::memory_order_relaxed);

        const uintptr_t h =
            _beginthreadex(nullptr, 0, DupeThread, req.get(), 0, nullptr);
        if (h == 0) {
            MessageBoxW(g_app.hwnd,
                        L"Could not start the duplicate search.",
                        L"Spindle", MB_OK | MB_ICONERROR);
            return;
        }
        static_cast<void>(req.release());
        g_app.dupeWorker  = reinterpret_cast<HANDLE>(h);
        g_app.dupeRunning = true;
        SetTimer(g_app.hwnd, kTimerId, kFrameMs, nullptr);
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }

    // Running the duplicate hunt is an explicit act, and it blocks: it is
    // reading files. The wait cursor is the honest signal, and Esc cancels
    // through the same flag a scan uses.
    if (g_app.dupeButton.w > 0.0f && g_app.dupeButton.contains(fx, fy) &&
        !g_app.trail.empty() && g_app.result) {
        // Not while a scan is running: the two would fight for the disk.
        if (g_app.bulkRunning) return;
        if (g_app.scanning && !YieldScanToHunt()) return;
        if (g_app.dupeRunning) return;   // already hunting
        CancelPrefetch(true);   // the hunt reads files; the walk can wait

        // Choose the candidates here, on the thread that owns the tree, and
        // strip every Node pointer before handing the list over. What the
        // worker receives is owned strings and sizes, so it cannot be
        // invalidated by anything the interface does while it runs.
        auto req = std::make_unique<DupRequest>();
        try {
            req->candidates = DuplicateCandidatesIn(
                *g_app.trail.back(), TrailPath(g_app.trail),
                kDefaultDupMinSize);
        } catch (...) {
            MessageBoxW(g_app.hwnd,
                        L"Ran out of memory looking for duplicates here. "
                        L"Try a smaller folder.",
                        L"Spindle", MB_OK | MB_ICONWARNING);
            return;
        }
        for (DupFile& f : req->candidates) f.node = nullptr;
        req->rootPath = TrailPath(g_app.trail);
        req->hwnd     = g_app.hwnd;
        req->gen      = g_app.dupeGen.fetch_add(1) + 1;

        g_app.dupeCurrentFile.clear();
        g_app.dupeProgress.files.store(0);
        g_app.dupeProgress.bytes.store(0);
        g_app.dupeProgress.cancel.store(false, std::memory_order_relaxed);

        const uintptr_t h =
            _beginthreadex(nullptr, 0, DupeThread, req.get(), 0, nullptr);
        if (h == 0) {
            MessageBoxW(g_app.hwnd,
                        L"Could not start the duplicate search: the system "
                        L"refused a new thread.",
                        L"Spindle", MB_OK | MB_ICONERROR);
            return;
        }
        static_cast<void>(req.release());  // the thread owns it now
        g_app.dupeWorker  = reinterpret_cast<HANDLE>(h);
        g_app.dupeRunning = true;
        // Repaint on the timer so the count climbs while it works.
        SetTimer(g_app.hwnd, kTimerId, kFrameMs, nullptr);
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }

    for (const DriveHit& d : g_app.driveHits) {
        if (d.rect.contains(fx, fy)) {
            StartScan(d.index);
            InvalidateRect(g_app.hwnd, nullptr, FALSE);
            return;
        }
    }

    for (size_t i = 0; i < g_app.panelTabs.size(); ++i) {
        if (!g_app.panelTabs[i].contains(fx, fy)) continue;
        if (static_cast<int>(g_app.panel) != static_cast<int>(i)) {
            g_app.tabFrom = g_app.tabTo;
            g_app.tabSlide.Begin(g_app.motion ? kTabMs : 0);
            SetTimer(g_app.hwnd, kTimerId, kFrameMs, nullptr);
        }
        g_app.panel = static_cast<App::Panel>(i);
        g_app.searchFocus = (g_app.panel == App::Panel::Search);
        g_app.panelDirty = true;
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }

    if (g_app.panel == App::Panel::Search &&
        g_app.searchBox.contains(fx, fy)) {
        g_app.searchFocus = true;
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }

    for (size_t i = 0; i < g_app.rowHits.size(); ++i) {
        if (!g_app.rowHits[i].contains(fx, fy)) continue;

        // Clicking a row in Kinds searches for that extension. The panel is
        // already a list of the things worth looking at, so it should be the
        // way you look at them.
        if (g_app.panel == App::Panel::Kinds) {
            if (i >= g_app.extStats.size()) return;
            const ExtStat& e = g_app.extStats[i];
            if (e.ext == L"\u2026") return;     // the folded remainder row
            g_app.query = e.ext.empty() ? std::wstring(L"is:file")
                                        : (L"ext:" + e.ext);
            g_app.tabFrom = g_app.tabTo;
            g_app.tabSlide.Begin(g_app.motion ? kTabMs : 0);
            g_app.panel = App::Panel::Search;
            g_app.searchFocus = true;
            g_app.panelDirty = true;
            SetTimer(g_app.hwnd, kTimerId, kFrameMs, nullptr);
            InvalidateRect(g_app.hwnd, nullptr, FALSE);
            return;
        }

        // A row in the file list opens that file's location, which is the
        // point of having the list next to the map. Joined with
        // AppendComponent, not naive concatenation: at the volume root the
        // trail path already ends in a backslash, and Explorer's /select
        // quietly opens the default folder when handed the doubled
        // separator that produced.
        if (i >= g_app.fileList.size()) return;
        {
            std::wstring full = TrailPath(g_app.trail);
            AppendComponent(full, g_app.fileList[i].path);
            RevealInExplorer(full);
        }
        return;
    }

    if (fx < layout::kSidebar) {
        g_app.searchFocus = false;
        g_app.searchSelectAll = false;
        return;
    }

    for (size_t i = 0; i < g_app.crumbHits.size(); ++i) {
        if (!g_app.crumbHits[i].contains(fx, fy)) continue;
        const size_t target = g_app.crumbFirst + i;
        if (target + 1 >= g_app.trail.size()) return;
        g_app.trail.resize(target + 1);
        g_app.hoverNode = nullptr;
        NavigateTo(g_app.trail.back(), g_app.mapBounds);
        return;
    }

    if (g_app.zoom.Running()) return;
    const int idx = HitTestIndex(g_app.cells, fx, fy);
    if (idx < 0) return;

    const Cell& cell = g_app.cells[static_cast<size_t>(idx)];
    if (!cell.node->dir || cell.node->children.empty()) return;

    // Push every directory between the current view and the clicked cell, so
    // the breadcrumb matches the path actually navigated to.
    for (const Node* n : CellChain(g_app.cells, idx)) g_app.trail.push_back(n);
    g_app.hoverNode  = nullptr;
    g_app.hoverIndex = -1;
    NavigateTo(cell.node, cell.rect);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_app.hwnd = hwnd;

            // Dark titlebar on Win10 1809+. Ignored on older builds, which is
            // why the return value is not checked.
            const BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, 20 /* USE_IMMERSIVE_DARK_MODE */,
                                  &dark, sizeof(dark));

            // Explicit IIDs rather than __uuidof: the templated
            // D2D1CreateFactory overload deduces its type parameter from the
            // out-pointer, and a void** deduces to void, which then fails to
            // link against __mingw_uuidof<void>.
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                         kIidD2D1Factory, nullptr,
                                         g_app.d2d.putVoid()))) {
                return -1;
            }
            if (FAILED(DWriteCreateFactory(
                    DWRITE_FACTORY_TYPE_SHARED, kIidDWriteFactory,
                    reinterpret_cast<IUnknown**>(g_app.dwrite.put())))) {
                return -1;
            }
            if (!CreateTextFormats()) return -1;

            // System-wide reduced-motion preference. Every duration below
            // becomes zero when it is off, which skips the animation rather
            // than shortening it.
            BOOL animate = TRUE;
            if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate,
                                      0)) {
                g_app.motion = (animate != FALSE);
            }

            g_app.dpiScale = QueryDpiScale(hwnd);
            g_app.volumes  = EnumerateVolumes();
            g_app.settings = LoadSettings();

            // An explicit target beats everything: it is what the user
            // typed, or the folder they right-clicked.
            if (!g_app.startPath.empty()) {
                int match = -1;
                for (size_t i = 0; i < g_app.volumes.size(); ++i) {
                    if (g_app.volumes[i].path == g_app.startPath) {
                        match = static_cast<int>(i);
                        break;
                    }
                }
                StartScanPath(g_app.startPath, match, true);
                QueueLaunchPrefetch(g_app.startPath);
                return 0;
            }

            // Otherwise open where the user left off: the drive with the
            // freshest cache comes up mapped immediately. The other fixed
            // drives are then walked one at a time in the background, so
            // within a few minutes of launch every drive answers a click
            // instantly - see QueueLaunchPrefetch for what is deliberately
            // never touched, and the menu toggle that turns it all off.
            std::wstring opened;
            if (g_app.settings.resumeOnLaunch && g_app.settings.keepCaches) {
                int      best = -1;
                FILETIME bestTime{};
                for (size_t i = 0; i < g_app.volumes.size(); ++i) {
                    const std::wstring cp =
                        CachePathForVolume(g_app.volumes[i].path);
                    if (cp.empty()) continue;
                    WIN32_FILE_ATTRIBUTE_DATA fad{};
                    if (!GetFileAttributesExW(cp.c_str(),
                                              GetFileExInfoStandard, &fad)) {
                        continue;
                    }
                    if (best < 0 ||
                        CompareFileTime(&fad.ftLastWriteTime, &bestTime) > 0) {
                        best     = static_cast<int>(i);
                        bestTime = fad.ftLastWriteTime;
                    }
                }
                if (best >= 0) {
                    opened = g_app.volumes[static_cast<size_t>(best)].path;
                    StartScan(best);
                }
            }
            QueueLaunchPrefetch(opened);
            return 0;
        }

        case WM_SIZE: {
            if (g_app.rt) {
                const D2D1_SIZE_U s{
                    static_cast<UINT32>(std::max<int>(LOWORD(lp), 1)),
                    static_cast<UINT32>(std::max<int>(HIWORD(lp), 1))};
                g_app.rt->Resize(s);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_DPICHANGED: {
            g_app.dpiScale = static_cast<float>(LOWORD(wp)) / 96.0f;
            if (g_app.rt) {
                const float d = g_app.dpiScale * 96.0f;
                g_app.rt->SetDpi(d, d);
            }
            // Windows supplies the correctly scaled frame for the new monitor.
            if (const RECT* target = reinterpret_cast<const RECT*>(lp)) {
                SetWindowPos(hwnd, nullptr, target->left, target->top,
                             target->right - target->left,
                             target->bottom - target->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_SETTINGCHANGE: {
            BOOL animate = TRUE;
            if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate,
                                      0)) {
                g_app.motion = (animate != FALSE);
            }
            return 0;
        }

        case WM_ENTERSIZEMOVE:
            g_app.resizing = true;
            return 0;

        case WM_EXITSIZEMOVE:
            g_app.resizing = false;
            if (!g_app.trail.empty()) RebuildTreemap();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_ERASEBKGND:
            return 1;   // fully repainted in WM_PAINT; skip the flicker

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            // A paint that throws must not take the process with it: under
            // memory pressure the string churn in Render can hit bad_alloc
            // (crash report #2 died exactly there). Dropping the frame is
            // harmless - the next invalidation repaints. Three in a row is
            // not pressure, it is a bug: let it reach the crash handler.
            static int paintFailures = 0;
            try {
                Render();
                paintFailures = 0;
            } catch (...) {
                if (++paintFailures >= 3) {
                    EndPaint(hwnd, &ps);
                    throw;
                }
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_TIMER: {
            if (wp != kTimerId) break;
            if (g_app.scanning || g_app.dupeRunning || g_app.zoom.Running() ||
                g_app.reveal.Running() || g_app.hoverFade.Running() ||
                g_app.dupeFlash.Running() ||
                g_app.tabSlide.Running()) {
                InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                KillTimer(hwnd, kTimerId);
            }
            return 0;
        }

        case WM_SCAN_DONE: {
            // Adopting the pointer first means a discarded result is still
            // freed rather than leaked.
            std::unique_ptr<ScanResult> res(
                reinterpret_cast<ScanResult*>(lp));

            // Superseded by a later scan: drop it. Its thread was already
            // joined by the StartScan that replaced it, so nothing to clean up.
            if (static_cast<uint64_t>(wp) != g_app.scanGen.load()) return 0;

            if (g_app.worker) {
                WaitForSingleObject(g_app.worker, INFINITE);
                CloseHandle(g_app.worker);
                g_app.worker = nullptr;
            }
            g_app.scanning = false;
            StartPrefetchNext();   // the disk is free again

            // A null result means the scan thread aborted rather than
            // finished. Say so instead of showing an empty map.
            if (!res) {
                MessageBoxW(hwnd, L"The scan stopped before it finished. "
                                  L"Nothing was changed on disk.",
                            L"Spindle", MB_OK | MB_ICONWARNING);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (!g_app.progress.cancel.load(std::memory_order_relaxed)) {
                // Drop every pointer into the outgoing tree BEFORE it is
                // freed. When a cached map is on screen the user has been
                // hovering and clicking a tree that is about to disappear
                // under them, so this is the ordinary path, not a race.
                DropTreeReferences();
                g_app.result = std::move(res);
                g_app.trail.push_back(&g_app.result->root);
                RebuildTreemap();
                // The panel lists were dropped above; without this they
                // stay empty until the user next changes view.
                g_app.panelDirty   = true;
                g_app.showingCache = false;
                g_app.zoomFrom = g_app.mapBounds;
                g_app.zoom.Begin(0);
                g_app.reveal.Begin(g_app.motion ? kRevealMs : 0);
                SetTimer(hwnd, kTimerId, kFrameMs, nullptr);
                if (!g_app.pendingReveal.empty()) {
                    const std::wstring full = g_app.pendingReveal;
                    g_app.pendingReveal.clear();
                    NavigateToPath(full);   // quietly gives up on a miss
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_PREFETCH_DONE: {
            // A cancelled walk was already joined - and its generation
            // bumped - by whoever cancelled it, so a stale note means
            // nothing is ours to clean up.
            if (static_cast<uint64_t>(wp) != g_app.prefetchGen.load()) {
                return 0;
            }
            if (g_app.prefetchWorker) {
                WaitForSingleObject(g_app.prefetchWorker, INFINITE);
                CloseHandle(g_app.prefetchWorker);
                g_app.prefetchWorker = nullptr;
            }
            g_app.prefetching = false;
            g_app.prefetchRoot.clear();
            StartPrefetchNext();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_DUPE_FILE: {
            std::unique_ptr<std::wstring> file(
                reinterpret_cast<std::wstring*>(lp));
            if (static_cast<uint64_t>(wp) != g_app.dupeGen.load()) return 0;
            if (file) g_app.dupeCurrentFile = std::move(*file);
            return 0;
        }

        case WM_BULK_DONE: {
            std::unique_ptr<BulkOutcome> res(
                reinterpret_cast<BulkOutcome*>(lp));
            if (static_cast<uint64_t>(wp) != g_app.dupeGen.load()) return 0;
            if (g_app.bulkWorker) {
                WaitForSingleObject(g_app.bulkWorker, INFINITE);
                CloseHandle(g_app.bulkWorker);
                g_app.bulkWorker = nullptr;
            }
            g_app.bulkRunning = false;
            g_app.dupeCurrentFile.clear();
            if (!res) return 0;

            // Prune what actually went from the report, then say what
            // happened. The groups shrink; a set reduced to one copy is
            // no longer a set.
            {
                std::unordered_set<std::wstring> gone;
                for (const std::wstring& p2 : res->recycled) {
                    gone.insert(FoldPath(p2));
                }
                for (DupGroup& grp : g_app.dupes.groups) {
                    grp.files.erase(
                        std::remove_if(grp.files.begin(), grp.files.end(),
                                       [&](const DupFile& f) {
                                           return gone.count(
                                                      FoldPath(f.Full())) !=
                                                  0;
                                       }),
                        grp.files.end());
                    grp.wasted = grp.size * (grp.files.size() > 0
                                                 ? grp.files.size() - 1
                                                 : 0);
                }
                g_app.dupes.groups.erase(
                    std::remove_if(g_app.dupes.groups.begin(),
                                   g_app.dupes.groups.end(),
                                   [](const DupGroup& grp) {
                                       return grp.files.size() < 2;
                                   }),
                    g_app.dupes.groups.end());
                g_app.dupes.totalWasted = 0;
                for (const DupGroup& grp : g_app.dupes.groups) {
                    g_app.dupes.totalWasted += grp.wasted;
                }
                RebuildDupePathSet();
                MarkDupeCells();
            }

            std::wstring done = L"Recycled " +
                                FormatFiles(res->recycled.size()) + L", " +
                                FormatSize(res->bytesFreed) +
                                L" to the Recycle Bin.";
            if (res->skippedFiles > 0) {
                done += L"\n\nSkipped " + FormatFiles(res->skippedFiles);
                if (res->skippedSets > 0) {
                    done += L" (" + FormatCount(res->skippedSets) +
                            L" sets no longer matched and were left "
                            L"alone)";
                }
                done += L".";
            }
            if (res->cancelled) done += L"\n\nStopped early.";
            MessageBoxW(hwnd, done.c_str(), L"Spindle",
                        MB_OK | MB_ICONINFORMATION);

            // The trees still draw the recycled files; refresh the drive
            // on screen now and queue the rest for the background walk.
            std::unordered_set<wchar_t> letters;
            for (const std::wstring& p2 : res->recycled) {
                if (p2.size() >= 2 && p2[1] == L':') {
                    letters.insert(
                        static_cast<wchar_t>(towupper(p2[0])));
                }
            }
            bool currentAffected = false;
            for (size_t i = 0; i < g_app.volumes.size(); ++i) {
                const std::wstring& vp = g_app.volumes[i].path;
                if (vp.empty() ||
                    letters.count(static_cast<wchar_t>(
                        towupper(vp[0]))) == 0) {
                    continue;
                }
                if (static_cast<int>(i) == g_app.selected) {
                    currentAffected = true;
                } else if (std::find(g_app.prefetchQueue.begin(),
                                     g_app.prefetchQueue.end(),
                                     vp) == g_app.prefetchQueue.end()) {
                    g_app.prefetchQueue.insert(g_app.prefetchQueue.begin(),
                                               vp);
                }
            }
            if (currentAffected && g_app.selected >= 0) {
                StartScan(g_app.selected, false);
            } else {
                StartPrefetchNext();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_DUPES_DONE: {
            // Adopt first, so a superseded report is freed rather than
            // leaked, then discard it if it is not the one being awaited.
            std::unique_ptr<DupReport> rep(
                reinterpret_cast<DupReport*>(lp));

            if (static_cast<uint64_t>(wp) != g_app.dupeGen.load()) return 0;

            if (g_app.dupeWorker) {
                WaitForSingleObject(g_app.dupeWorker, INFINITE);
                CloseHandle(g_app.dupeWorker);
                g_app.dupeWorker = nullptr;
            }
            g_app.dupeRunning = false;
            g_app.dupeCurrentFile.clear();
            StartPrefetchNext();   // the disk is free again

            if (rep) {
                g_app.dupes = std::move(*rep);
                g_app.dupesRun = true;
                RebuildDupePathSet();
                MarkDupeCells();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_MOUSEMOVE:
            OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;

        case WM_MOUSELEAVE:
            g_app.hoverNode = nullptr;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN:
            SetFocus(hwnd);
            OnLeftClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;

        case WM_RBUTTONDOWN: {
            const float inv =
                (g_app.dpiScale > 0.0f) ? 1.0f / g_app.dpiScale : 1.0f;
            const float fx = static_cast<float>(GET_X_LPARAM(lp)) * inv;
            const float fy = static_cast<float>(GET_Y_LPARAM(lp)) * inv;
            // A duplicate row in the sidebar takes the right-click first;
            // it offers reveal and recycle rather than the treemap actions.
            for (size_t i = 0; i < g_app.dupeRowHits.size(); ++i) {
                if (g_app.dupeRowHits[i].contains(fx, fy)) {
                    POINT rp{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                    ClientToScreen(hwnd, &rp);
                    ShowDupeMenu(static_cast<int>(i), rp);
                    return 0;
                }
            }
            // Hit tests use final positions, so suppress the menu mid-zoom
            // where the cell under the cursor is not the one being drawn there.
            if (g_app.zoom.Running()) return 0;
            const int idx = HitTestIndex(g_app.cells, fx, fy);
            if (idx >= 0) {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ClientToScreen(hwnd, &pt);
                ShowCellMenu(idx, pt);
            }
            return 0;
        }

        case WM_XBUTTONDOWN:
            if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) GoUp();
            return TRUE;

        case WM_CHAR: {
            if (!g_app.searchFocus) return 0;
            const wchar_t c = static_cast<wchar_t>(wp);
            if (c == L'\b') {
                if (g_app.searchSelectAll) {
                    g_app.query.clear();
                    g_app.searchSelectAll = false;
                } else if (!g_app.query.empty()) {
                    g_app.query.pop_back();
                }
            } else if (c >= 0x20 && c != 0x7F) {
                if (g_app.searchSelectAll) {
                    g_app.query.clear();
                    g_app.searchSelectAll = false;
                }
                if (g_app.query.size() < 128) g_app.query.push_back(c);
            } else {
                return 0;
            }
            g_app.panelDirty = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_KEYDOWN: {
            // Backspace types into the search box when it has focus, rather
            // than navigating up a level.
            if (g_app.searchFocus && wp == VK_BACK) return 0;
            if (wp == VK_ESCAPE && g_app.searchFocus) {
                g_app.searchFocus = false;
                g_app.searchSelectAll = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            // The editing keys every text box owes its user. Selection is
            // all-or-nothing: Ctrl+A marks the query, and the next
            // keystroke, paste or backspace replaces it.
            if (g_app.searchFocus && (GetKeyState(VK_CONTROL) & 0x8000)) {
                if (wp == 'A') {
                    g_app.searchSelectAll = !g_app.query.empty();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (wp == 'C' || wp == 'X') {
                    if (!g_app.query.empty()) {
                        CopyPathToClipboard(g_app.query);
                    }
                    if (wp == 'X') {
                        g_app.query.clear();
                        g_app.searchSelectAll = false;
                        g_app.panelDirty = true;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                }
                if (wp == 'V') {
                    PasteIntoSearch();
                    return 0;
                }
            }
            if (wp == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                g_app.panel = App::Panel::Search;
                g_app.searchFocus = true;
                g_app.panelDirty = true;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wp == 'E' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                DoExport();
                return 0;
            }
            switch (wp) {
                case VK_BACK:   GoUp(); break;
                case VK_ESCAPE:
                    if (g_app.scanning) {
                        g_app.progress.cancel.store(true,
                                                    std::memory_order_relaxed);
                    } else if (g_app.prefetching) {
                        // Esc means "leave the disk alone", so the whole
                        // queue goes, not just the drive mid-walk. The next
                        // launch rebuilds it.
                        CancelPrefetch(false);
                        g_app.prefetchQueue.clear();
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    // Reachable now that the hunt has its own thread; while
                    // it ran on this one the key could never be dispatched.
                    if (g_app.dupeRunning || g_app.bulkRunning) {
                        g_app.dupeProgress.cancel.store(
                            true, std::memory_order_relaxed);
                    }
                    break;
                case VK_F5:
                    if (g_app.selected >= 0) StartScan(g_app.selected);
                    break;
                default: break;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            const float sc = (g_app.dpiScale > 0.0f) ? g_app.dpiScale : 1.0f;
            mmi->ptMinTrackSize.x = static_cast<LONG>(900.0f * sc);
            mmi->ptMinTrackSize.y = static_cast<LONG>(560.0f * sc);
            return 0;
        }

        case WM_CLOSE:
            JoinDupeWorker();
            JoinWorker();
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            JoinWorker();
            JoinDupeWorker();
            JoinBulkWorker();
            CancelPrefetch(false);
            DiscardDeviceResources();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}


// ------------------------------------------------------------ crash report

// If Spindle does fall over, leave something behind that can actually be
// acted on. The report records the faulting address as an offset from the
// module base, which is what makes it meaningful under ASLR: a raw address
// differs every run, while base+offset maps straight onto the binary.
// module+offset for an address, whatever module that is. A raw address is
// meaningless under ASLR, and a spindle-relative offset for an address that
// actually lives in kernelbase is worse: it looks symbolisable and is not.
// ASCII out, because the report writer is deliberately ASCII-only.
static int DescribeAddress(uintptr_t addr, char* out, size_t cap) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &mod) &&
        mod != nullptr) {
        char name[64] = "?";
        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameW(mod, path, MAX_PATH) > 0) {
            const wchar_t* leaf = wcsrchr(path, L'\\');
            leaf = leaf ? leaf + 1 : path;
            size_t i = 0;
            for (; leaf[i] != 0 && i < sizeof(name) - 1; ++i) {
                const wchar_t c = leaf[i];
                name[i] = (c < 0x20 || c > 0x7E) ? '?'
                                                 : static_cast<char>(c);
            }
            name[i] = 0;
        }
        const bool self = (mod == GetModuleHandleW(nullptr));
        return _snprintf_s(out, cap, _TRUNCATE, "%s+0x%llX",
                           self ? "spindle" : name,
                           addr - reinterpret_cast<uintptr_t>(mod));
    }
    return _snprintf_s(out, cap, _TRUNCATE, "0x%llX (unmapped)", addr);
}

// The few exception codes that keep turning up, named, so a report reads
// without a lookup table. 0x20474343 is "GCC " - MinGW's tag for a C++
// exception that nothing caught.
static const char* ExceptionCodeNote(DWORD code) {
    switch (code) {
        case 0xC0000005: return " (access violation)";
        case 0x20474343: return " (unhandled C++ exception)";
        case 0xE06D7363: return " (unhandled C++ exception, MSVC)";
        case 0xC0000374: return " (heap corruption)";
        case 0xC00000FD: return " (stack overflow)";
        default:         return "";
    }
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_EXECUTE_HANDLER;

    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    std::wstring report(path);
    const size_t slash = report.find_last_of(L'\\');
    if (slash != std::wstring::npos) report.resize(slash + 1);
    report += L"spindle-crash.txt";

    const HANDLE f = CreateFileW(report.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
    if (f == INVALID_HANDLE_VALUE) return EXCEPTION_EXECUTE_HANDLER;

    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto addr =
        reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);

    char where[128];
    DescribeAddress(addr, where, sizeof(where));

    char buf[2048];
    int n = _snprintf_s(
        buf, sizeof(buf), _TRUNCATE,
        "Spindle crash report\r\n"
        "--------------------\r\n"
        "exception code : 0x%08lX%s\r\n"
        "exception at   : %s\r\n"
        "spindle base   : 0x%016llX\r\n"
        "thread id      : %lu\r\n"
        "scanning       : %s\r\n"
        "cells          : %zu\r\n",
        ep->ExceptionRecord->ExceptionCode,
        ExceptionCodeNote(ep->ExceptionRecord->ExceptionCode),
        where,
        static_cast<unsigned long long>(base),
        GetCurrentThreadId(),
        g_app.scanning ? "yes" : "no",
        g_app.cells.size());

    if (n > 0) {
        DWORD written = 0;
        WriteFile(f, buf, static_cast<DWORD>(n), &written, nullptr);

        // Return addresses, each named against the module it belongs to.
        // "spindle+0x..." lines symbolise against this binary; the rest say
        // which DLL they were in, which is usually all they need to.
        void* frames[32] = {};
        const USHORT got = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
        const char* hdr = "\r\nreturn addresses\r\n";
        WriteFile(f, hdr, static_cast<DWORD>(strlen(hdr)), &written, nullptr);
        for (USHORT i = 0; i < got; ++i) {
            DescribeAddress(reinterpret_cast<uintptr_t>(frames[i]), where,
                            sizeof(where));
            n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "  [%2u] %s\r\n", i,
                            where);
            if (n > 0) {
                WriteFile(f, buf, static_cast<DWORD>(n), &written, nullptr);
            }
        }
    }
    CloseHandle(f);

    MessageBoxW(nullptr,
                L"Spindle hit a fault and has to close.\n\n"
                L"A report was written to spindle-crash.txt next to the "
                L"executable. Nothing on disk was changed.",
                L"Spindle", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

// std::terminate does not go through the exception filter, so it needs its
// own hook or an escaping exception still dies silently.
static void OnTerminate() {
    MessageBoxW(nullptr,
                L"Spindle ran out of memory or hit an unrecoverable error "
                L"and has to close.\n\nNothing on disk was changed.",
                L"Spindle", MB_OK | MB_ICONERROR);
    _exit(3);
}

// ---------------------------------------------------------------- entry point

// Write a line to the console that launched us, if there was one. A GUI
// binary has no console of its own, so --help from a prompt would otherwise
// print into the void.
static void ConsoleLine(const std::wstring& text) {
    static bool attached = false;
    if (!attached) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        attached = true;
    }
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const std::wstring line = text + L"\r\n";
    WriteConsoleW(out, line.c_str(), static_cast<DWORD>(line.size()),
                  &written, nullptr);
}

// Headless scan-and-export. No window, no cache, an exit code Task
// Scheduler can act on - which is what makes an external scheduler a
// complete answer rather than an excuse.
static int RunHeadlessExport(const CommandLine& cl) {
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);

    Progress progress;
    ScanResult res = Scan(cl.path, si.dwNumberOfProcessors, &progress);
    if (res.root.size == 0 && res.stats.fileCount == 0) {
        ConsoleLine(L"spindle: nothing scanned at " + cl.path);
        return 1;
    }

    if (!ExportCsv(res.root, cl.path, cl.csvOut)) {
        ConsoleLine(L"spindle: could not write " + cl.csvOut);
        return 1;
    }

    ConsoleLine(L"spindle: " + FormatCount(res.stats.fileCount) +
                L" files, " + FormatSize(res.stats.bytes) + L" -> " +
                cl.csvOut);

    if (cl.wantDuplicates) {
        const DupReport dup =
            FindDuplicates(res.root, cl.path, cl.minDup, &progress);
        ConsoleLine(L"spindle: " + FormatCount(dup.groups.size()) +
                    L" duplicate sets, " + FormatSize(dup.totalWasted) +
                    L" recoverable");
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    SetUnhandledExceptionFilter(CrashHandler);
    std::set_terminate(OnTerminate);

    // Command line before anything graphical is created, so the headless
    // modes never touch Direct2D or open a window.
    CommandLine cl;
    {
        int argc = 0;
        std::vector<std::wstring> args;
        if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
            for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
            LocalFree(argv);
        }
        cl = ParseCommandLine(args);
    }
    if (!cl.valid) {
        ConsoleLine(L"spindle: " + cl.error);
        ConsoleLine(CommandLineHelp());
        return 2;
    }
    if (cl.mode == CommandLine::Mode::Help) {
        ConsoleLine(CommandLineHelp());
        return 0;
    }
    if (cl.mode == CommandLine::Mode::Version) {
        ConsoleLine(std::wstring(L"spindle ") + kAppVersion);
        return 0;
    }
    if (cl.mode == CommandLine::Mode::Export) {
        return RunHeadlessExport(cl);
    }
    g_app.startPath = cl.path;

    // Per-monitor DPI v2 where available. Resolved at runtime so the binary
    // still starts on builds that predate the API.
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetCtxFn = BOOL(WINAPI*)(HANDLE);
        const FARPROC raw =
            GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (raw) {
            // GetProcAddress returns FARPROC, so reaching any real API means
            // casting to a signature the compiler cannot verify. Scoped to
            // this one call rather than turning the warning off globally.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
            const auto fn = reinterpret_cast<SetCtxFn>(raw);
#pragma GCC diagnostic pop
            static_cast<void>(fn(reinterpret_cast<HANDLE>(-4)));  // PMv2
        }
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"SpindleWindow";

    // LoadImage rather than LoadIcon so the large and small icons each come
    // from the matching frame in the .ico. LoadIcon returns one size and lets
    // the shell rescale it, which is visibly soft in the titlebar.
    wc.hIcon = static_cast<HICON>(LoadImageW(
        inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
        LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(
        inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));

    // Degrade in steps rather than straight to the stock icon: LoadIcon is
    // more forgiving about which frame it picks, so it can still succeed
    // where the explicit-size request did not.
    if (!wc.hIcon)   wc.hIcon   = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APPICON));
    if (!wc.hIconSm) wc.hIconSm = wc.hIcon;
    if (!wc.hIcon)   wc.hIcon   = LoadIconW(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc)) {
        CoUninitialize();
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Spindle", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
        CW_USEDEFAULT, 1280, 800, nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        CoUninitialize();
        return 1;
    }

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
