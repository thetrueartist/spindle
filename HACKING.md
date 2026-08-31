# Spindle

A disk space analyser for Windows. Native C++17, Win32 + Direct2D, no runtime
and no third-party libraries. Single portable executable, ~1 MB.

Scans a volume, shows where the space went as a treemap coloured by file kind,
with a side panel for extension breakdown, largest files, and search. Reads
the NTFS Master File Table directly when it can, which makes a million-file
scan take well under a second.

## Commands

```
make            cross-compile build/spindle.exe (MinGW-w64)
make test       771 assertions under ASan, UBSan and ThreadSanitizer
make stress     walk a real tree with the scanner's concurrency structure
make analyze    cppcheck + clang-tidy
make icon       regenerate spindle.ico (prints each frame's encoding)
make clean
```

Cross-compiles from Linux with `mingw-w64`; also builds under MSYS2. Analysis
targets need `cppcheck` and `clang-tidy`. `make icon` needs Python 3 stdlib
only.

## Layout

```
src/spindle.h        types, and the public interface of everything below
src/sync.h           SRWLOCK/CONDITION_VARIABLE, pthreads for host tests
src/workqueue.h      the scanner's work queue — testable on any host
src/ntfs.h/.cpp      NTFS on-disk structures — pure bytes, fuzz-tested
src/mft.cpp          raw volume I/O and tree assembly (Windows)
src/scan.cpp         parallel FindFirstFileEx walker, volumes, file writing
src/core.cpp         treemap, categories, reports, search, CSV, easing
src/ui.cpp           Win32 window, Direct2D renderer, navigation, animation
res/                 icon, version info, manifest
tools/make_icon.py   icon generator, stdlib only
tests/               core, ntfs, queue, stress
docs/                README media, captured from the real binary under Wine
```

The split is deliberate: **`core.cpp`, `ntfs.cpp` and `workqueue.h` contain no
Windows headers**, so they compile and run under sanitizers on any host. That
is where the tests live, and it is why the fuzzing exists at all. Keep it that
way — anything that needs `windows.h` belongs in `scan.cpp`, `mft.cpp` or
`ui.cpp`.

## Invariants

These each encode a bug that was actually hit. Breaking one reintroduces it.

**Threading is Win32, never `<thread>`/`<mutex>`.** MinGW's win32 threading
model backs the standard types with an implementation that crashes under a
sixteen-thread scanner sustained for seconds. `SRWLOCK`, `CONDITION_VARIABLE`
and `_beginthreadex` instead. Both MinGW threading models must produce
identical imports.

**Thread entry points catch everything.** An exception escaping a thread calls
`std::terminate` — process gone, no dialog. A large scan does millions of
allocations, so one `bad_alloc` took the whole app down.

**Scans carry a generation number.** Clicking drive A then B before A finishes
used to adopt A's tree while B was still running, and join B's thread on the
UI thread. Stale results are discarded by generation.

**Cells carry a parent index.** Cells nest up to five levels below the viewed
directory, so breadcrumb + cell name is *not* the path. Getting this wrong
made "Move to Recycle Bin" target the wrong file. Reconstruct via
`CellChain()`.

**Expanded directories reserve a label strip.** A parent and its children both
drawing labels put them on top of each other, because children are inset by
1–3px. `Cell::header` and `Cell::expanded` drive this; a cell too small for a
strip yields its label to the children over it.

**All size arithmetic goes through `SatAdd`.** A volume reporting absurd sizes
must clamp, not wrap to something small and vanish from the map.

**Filenames are attacker-controlled.** `SanitizeForDisplay` strips C0/C1
controls, bidi overrides (U+202A–202E, U+2066–2069) and zero-width characters
before display *and* before CSV export. `invoice\u202Egpj.exe` renders as
`invoicexe.jpg` otherwise.

**Tree walks are iterative.** Directory nesting depth comes off the disk.
`RollUp`, `SortTree`, `ForEachNode` and the MFT tree build all use explicit
stacks. The one recursive function, `BuildRecursive` in the treemap, is
bounded twice over.

**Reparse points are never traversed.** Junctions and symlinks would loop or
double-count.

**Nothing that holds a `Node*` may survive a tree swap.** `StartScan` clears
the panel caches (`fileList`, `extStats`, `rowHits`) and hover state before
`result.reset()`, and adopting a fresh tree marks the panel dirty. The
0xC0000005 that forced this rule was the side panel drawing the previous
scan's file list mid-rescan, through pointers into the freed tree.

**The scan cache is input, not state.** `%LOCALAPPDATA%\Spindle\*.spincache`
is parsed with the same posture as `ntfs.cpp`: every length, count and enum
validated, refusal bounds on totals, tree built iteratively with exact
reserves. It is keyed to the volume serial, written only for clean
uncancelled scans, and replaced atomically (temp + rename) so a torn file
cannot exist.

**Text rects are sized from `layout::kLine*`.** Formats set uniform line
spacing to those constants; a rect shorter than its line box gets cropped by
`DRAW_TEXT_OPTIONS_CLIP`. Paragraph alignment is centred so over-generous
rects degrade gracefully. Text alignment is a property of the shared format
object and must be restored after any non-leading draw.

**ICO frames: DIB up to 128, PNG only at 256.** Windows will not decode PNG at
small sizes, and `LoadImage` fails silently into the stock Windows icon.

## Security posture

The threat model is that this is pointed at drives full of malware samples,
and reads raw disk structures while elevated.

- `ntfs.cpp` treats every byte as hostile. Every field goes through a cursor
  that cannot read past its buffer; no length, offset or count from disk is
  used without validation. Fuzzing found two real bugs there — a 64-bit shift
  in run-list sign extension, and a signed overflow accumulating the cluster
  delta.
- The volume is opened `FILE_READ_DATA`, sharing read/write/delete.
- Deletion is recycle-bin only, confirmed (twice for folders, No the default
  both times), has no keyboard shortcut, and refuses anything that looks
  like a volume root. The `SHFileOperationW` buffer is built with an explicit
  double NUL. Nothing in the program deletes without that dialog chain.
- **No network, registry-write, process-creation or injection APIs.**
  `ShellExecuteW` is present for exactly one thing: opening Explorer at a
  path. `LoadLibraryW`/`VirtualProtect`/`CryptGenRandom` appear in the import
  table but are not called by any line of Spindle — they come from the MinGW
  runtime.
- DEP, ASLR, high-entropy ASLR and NO_SEH are on. Control Flow Guard is not:
  it is MSVC-only (`/guard:cf`) and GCC cannot emit it.
- A crash handler writes `spindle-crash.txt` with the fault address as a
  **module-base offset** — a raw address is meaningless under ASLR.

## Known limits

- Sizes are logical file length, so compressed, sparse and deduplicated files
  read larger than their footprint. WinSxS is heavily hardlinked and its
  apparent size is not reclaimable.
- Memory is roughly 90 bytes plus the filename per node — around 170 MB for a
  1.6M-file volume. A string pool and index-based tree would cut that
  substantially and is the obvious next optimisation.
- The MFT path has been verified by parser fuzzing and review, not by running
  against a real volume (the development environment is Linux). It falls back
  automatically on any failure.
- Cache revalidation is a full rescan running behind the cached view, not an
  incremental diff. Reading the NTFS USN journal to patch the cached tree in
  place is the obvious next step, and needs a real volume to develop against.
- Not implemented: duplicate detection, scan scheduling, network share
  discovery.

## Conventions

- Comments explain *why*, not what. If a line needs a comment to say what it
  does, rewrite the line.
- Every non-obvious constant gets a sentence on where the number came from.
- Tests assert invariants, not implementation. Several exist purely to encode
  a bug so it cannot come back.
- British spelling in prose; American in identifiers that mirror Win32.
