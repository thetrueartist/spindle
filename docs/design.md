# How Spindle works

Engineering notes on the parts that are not obvious from using it.
For the invariants and the layout of the source, see
[HACKING.md](../HACKING.md).

## Speed

Scanning uses the NTFS Master File Table when it can. Instead of a
syscall round trip per directory it does a handful of large sequential
reads of the volume's own index, which is the difference between several
seconds and well under one on a million-file drive. WizTree is built
around the same technique.

The MFT path needs a local NTFS volume and an elevated process, because
raw volume access requires administrator rights. When any of that is
missing (a network share, exFAT, no elevation) Spindle falls back to the
parallel directory walk. The status bar shows `MFT` when the fast path
ran.

The side panel stays instant on large trees. On a 1.9M-file tree the
extension breakdown takes about 60 ms, the largest-files list under
30 ms, a name search about 25 ms and a treemap rebuild 13 ms, with hit
testing at a few microseconds per mouse move. Extensions are aggregated
by a hash computed in place rather than by building a string per file,
and full paths are only assembled for the rows a caller actually keeps.

`make bench` measures the portable paths on a synthetic volume of a
million files in seventy thousand folders, median of five runs on an
optimised build. On one ordinary machine:

| | |
|---|---|
| Roll-up of every folder size | 17 ms |
| Category of a file from its name | 49 ns each |
| Treemap layout, 8,500 cells | 3 ms |
| Kinds panel | 61 ms |
| Largest panel | 21 ms |
| Find, a query with three terms | 21 ms |
| Cache written (51 MB) and read back | 106 ms and 203 ms |
| Tree assembled from 400,000 MFT records | 395 ms, of which reading the records 111 ms |

The category lookup is a compile-time hash table over the extension
rules, because both the directory walk and the MFT assembly ask it once
per file; the record parser builds each name once; the tree build reuses
one scratch buffer for the whole table; and the cache parser reads each
name straight into its node. Together those took the MFT assembly from
506 ms to 395 ms and the cache read from 266 ms to 203 ms on the same
volume.

## Architecture

```
src/spindle.h        types and interfaces
src/sync.h           SRWLOCK / CONDITION_VARIABLE, pthreads for host tests
src/workqueue.h      the scanner's work queue (testable on any host)
src/ntfs.h/.cpp      NTFS on-disk structures, pure bytes, fuzz-tested
src/mfttree.h/.cpp   the tree built from MFT records (portable)
src/mft.cpp          raw volume I/O for the MFT scan (Windows)
src/core.cpp         treemap, categorisation, reports, CSV (portable)
src/scan.cpp         parallel FindFirstFileEx walker, volume enumeration
src/ui.cpp           Win32 window, Direct2D renderer, navigation
res/spindle.rc       icon, version info, manifest
res/spindle.manifest DPI awareness, Common Controls v6, asInvoker
tools/make_icon.py   generates spindle.ico
```

The icon is an asymmetric treemap in the category palette rather than a
generic disc, with amber dominant to match the accent. Every size from 16
to 256 is drawn natively instead of downscaled from one master, because a
16px downscale of a geometric mark loses its structure. `make icon`
regenerates it.

`tools/make_icon.py` uses only the Python standard library; it
rasterises, encodes and packs the ICO itself. Frame encoding is the part
that matters: the ICO container allows DIB or PNG frames, but Windows
only reliably decodes PNG at 256x256 and fails silently into the stock
icon otherwise. Frames are DIB up to 128 and PNG only at 256, and
`make icon` prints each frame's encoding so a regression is visible.

The manifest declares `PerMonitorV2` DPI awareness as a fallback for
builds predating `SetProcessDpiAwarenessContext`, and `asInvoker`.
Spindle never requests elevation itself, because prompting on every
launch to read protected paths the user may not care about is the wrong
default.

The scan runs on a pool of worker threads pulling from a shared queue, so
one enormous directory does not leave a single thread doing all the work.
Each directory's children are counted and the vector reserved before any
pointer into it is queued, which is what keeps the child pointers stable
without a lock. Sizes are rolled up in a single-threaded post-order pass
afterwards.

Layout is the squarified algorithm from Bruls, Huizing and van Wijk
(2000), which keeps cells close to square: a mean aspect ratio of about
1.2 on random inputs.

## Text rendering

Antialiasing is grayscale rather than ClearType. Subpixel rendering puts
coloured fringes on glyph edges, which is unobtrusive on white and
clearly visible on a dark background, especially on muted greys.

Line boxes are pinned explicitly rather than left to the font's own
metrics. Segoe UI at 19px needs about 25px of line box, and a rect
shorter than that gets its descenders clipped. Each text format sets
uniform line spacing to a known value, every rect is sized from that
constant, and paragraph alignment is centred, so a slightly generous
rect centres the text and a slightly tight one does not crop it.

## Motion

Four animations, all short. Past roughly 200 ms a transition starts to
feel like waiting.

| | |
|---|---|
| Zoom on drill-in | 145 ms, easeOutQuint |
| First paint after a scan | 230 ms, staggered by depth |
| Hover | 80 ms fade |
| Panel underline | 130 ms slide |

easeOutQuint is the default because it covers 87% of the distance in the
first third of the duration. A symmetric curve over the same period feels
sluggish even though it takes exactly as long: at t=0.33 an in-out cubic
has only moved 14% of the way, and the eye reads that as hesitation.

The scan entrance staggers by depth, so top-level blocks settle first and
nested ones follow, and the map assembles outside-in. Cells also settle
slightly inward as they arrive, anchored on their own centre.

Hover is animated rather than binary, so sweeping the pointer across a
thousand blocks does not strobe. The hovered cell lifts slightly as well
as gaining an outline.

Animations run on an 8 ms timer that stops as soon as nothing is moving.
There is no permanent repaint loop.

Reduced motion is honoured. `SPI_GETCLIENTAREAANIMATION` is read at
startup and on `WM_SETTINGCHANGE`, and when it is off every duration
becomes zero, which skips the transitions rather than shortening them.
Easing curves clamp out-of-range input, so a dropped frame cannot
overshoot into a position the layout never intended.
