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
make test       924 assertions under ASan, UBSan and ThreadSanitizer
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

**The root of a cached tree is a path; every other name is a component.**
The root is named for the volume - `D:\` - so validating it as a path
component rejects it, and because an unreadable cache is deleted and
rescanned, the only symptom was that caching silently stopped working
everywhere. The test fixtures used an empty root name and so could not
catch it; one now uses a real volume path.

**A duplicate is never deleted on the strength of a hash.** Recycling a
copy from the Dupes panel is gated: it finds a *different* member of the
same group, proves the two byte-for-byte with VerifyFilesIdentical, and
only then offers to recycle - so the last copy can never be the one
removed, and a 128-bit collision can never cause a deletion. Reparse
points are refused by the open, hardlinks are already excluded from
groups, deletion is the Recycle Bin (reversible) with a protected-path
refusal and a No-default confirmation. The failure mode is to refuse,
never to delete the wrong thing.

**A pair is confirmed by comparison, not by hashing.** To prove two files
identical you must read both in full either way; hashing's only advantage
is that it turns an all-pairs comparison of a large group from O(n^2) into
O(n). For a group of exactly two - the common case - there is no such
advantage, so those are compared byte for byte instead: exact where a
128-bit digest is only near-certain, stopped at the first differing byte
where a hash must read to the end, and no crafted collision can make two
different files look the same. Three or more still go through the digest.
This is the verification the header used to claim without providing, and
it is what any future deletion will pass through before touching a file.

**A file is never read in full to prove it is different.** Sharing a size
makes two files *possible* duplicates, not actual ones, and the original
finder spent a whole file discovering otherwise - two 40 GB images differing
in their first block cost 80 GB of reading. Comparison is tiered: exact size
(free), then a 16 KB head, then a 16 KB tail, and only what survives all
three is read in full. The tail tier exists because disk images and media
containers share fixed headers, so a head probe alone does not separate them.
Only a complete digest is ever allowed to call two files equal. Measured on
20 same-size files differing at byte 0: 3.24s to 0.28s cold, 7.09s to 0.14s
warm.

**Anything that reads the disk runs off the UI thread.** The scan always
did; the duplicate hunt did not, and a window that never pumps its message
loop is a window Windows paints a spinning cursor over and marks Not
Responding - including the Esc-to-cancel the panel advertised, which could
never be delivered. Candidates are chosen on the UI thread (a tree walk,
fast, and it owns the tree), every `Node*` is stripped from them, and only
owned paths cross to the worker. Completion arrives as `WM_DUPES_DONE`
carrying a generation number, so a superseded report is freed rather than
adopted.

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

The threat model is that every byte off a disk — names, sizes, on-disk
structures — is untrusted input, read while elevated.

- `ntfs.cpp` treats every byte as hostile. Every field goes through a cursor
  that cannot read past its buffer; no length, offset or count from disk is
  used without validation. Fuzzing found two real bugs there — a 64-bit shift
  in run-list sign extension, and a signed overflow accumulating the cluster
  delta.
- The volume is opened `FILE_READ_DATA`, sharing read/write/delete.
- Ordinary deletion is recycle-bin only, confirmed (twice for folders, No the
  default both times), has no keyboard shortcut, and refuses anything that
  looks like a volume root. The `SHFileOperationW` buffer is built with an
  explicit double NUL.
- **Force removal** is the one genuinely destructive feature: permanent
  deletion, taking ownership when the ACL refuses, and terminating the
  processes holding the target open. "Force" applies to locks and
  permissions, never to the confirmations — it is a separate menu item below
  the reversible one, never the default, has no shortcut, and passes three
  gates: the protected-path refusal, a permanent-deletion warning stating
  the size and file count, and a named list of the processes that would be
  ended. No is the default on every one.
**Names off the disk are not path components until they are checked.**
`IsSafeNodeName` refuses a backslash, a forward slash, a colon, a NUL, the
Win32-reserved characters, `.`/`..`, and a trailing dot or space — applied in
`ntfs.cpp` where `$FILE_NAME` is read and in `ReadRecord` where the cache is.
NTFS's POSIX namespace permits every one of those, and the cache file is
writable without elevation while being read with it, so a name could
otherwise carry a separator and aim a later delete somewhere it was never
displayed. An embedded NUL is the sharpest version: the panel draws the whole
string, `CreateFileW` stops at the NUL, and the two are different files.

**The protected-path check and the Win32 name resolver must agree.**
`IsProtectedSystemPath` compares strings, but every caller hands the same
string to an API that re-parses it — so any spelling Win32 resolves
differently is a bypass. Forward slashes, `.`/`..` components, trailing dots
or spaces (`C:\Windows ` opens `C:\Windows`), 8.3 aliases (`PROGRA~1`),
drive-relative forms (`C:Windows`, which resolves against a current directory
that is System32 under UAC) and embedded NULs are all refused outright rather
than normalised. Deletion then runs on the `\\?\` form, which disables
canonicalisation so the object deleted is the one that was checked.

**Trees are bounded in depth wherever they are built.** `Node` owns its
children by value, so the compiler's destructor recurses once per level: a
tree deeper than the stack can unwind cannot be *freed*, and that crash is
not catchable. The walker's `kMaxDepth` was not enough — the cache reader and
the MFT builder both produce trees too, and both now stop at
`kMaxTreeDepth`. The cache also caps the running total of declared children
against the declared node count, because per-record bounds allowed a chain of
directories to reserve gigabytes from a small file.

- What force removal may never touch lives in `core.cpp`, not in the dialog:
  `IsProtectedSystemPath` (drive roots, UNC, `\Windows`, `System Volume
  Information`, `$Recycle.Bin`, `Recovery`, `Boot`, the boot files at and
  below; the `Program Files`, `ProgramData`, `Users` and `PerfLogs` roots
  themselves, their contents allowed) and `IsCriticalProcess` (pid ≤ 4,
  smss/csrss/wininit/winlogon/services/lsass/svchost/dwm, and spindle itself).
  Both are host-tested, `ForceRemove` re-checks the path itself so no caller
  can route around the dialog, and the tree walk re-checks per directory
  because it is following names off the disk. The Recycle Bin path runs the
  same refusal: the reversible option guarding less than the permanent one
  was backwards.
- A locking process is identified by its **image name**, never by the Restart
  Manager's `strAppName`. That field is a display string from the version
  resource — `lsass.exe` presents as "Local Security Authority Process" — so
  a list written in image names matched none of the processes it existed to
  protect. The Restart Manager's own `RmCritical`/`RmService` classification
  is trusted first, the image name is re-read from the handle about to be
  killed, and the process creation time is compared so a recycled PID cannot
  redirect the kill.
- Taking ownership acts on a **handle opened `FILE_FLAG_OPEN_REPARSE_POINT`**,
  never on a path, and it **merges** into the existing DACL. The path-based
  form followed junctions, so a link aimed at a system directory plus a deny
  ACE on the link was enough to rewrite the target's owner and permissions;
  passing a null old ACL then discarded every existing entry, DENY aces
  included. Escalation is skipped entirely for reparse points: a link's own
  ACL is not why a delete failed.
- Reparse points are removed as links, never followed — the deletion walk
  inherits the scanner's rule, where breaking it would delete the target.
- The Explorer folder-menu entry is the one registry write in the program.
  It is off by default, ticked on from the menu, writes three keys under
  `HKCU\Software\Classes\Directory\shell\Spindle` (per-user, no elevation,
  nothing machine-wide), and unticking deletes exactly what it created. The
  command is written with `"%1"` quoted, because a folder name with a space
  otherwise arrives as several arguments.
- Finding duplicates is the only thing that reads file *contents*, so it
  never runs on its own: it is a button, not a panel refresh. Only files
  sharing an exact size with another are opened, cloud placeholders are
  excluded and the handle is opened `FILE_FLAG_OPEN_NO_RECALL` besides, so
  a placeholder cannot be silently downloaded to be hashed.
- **No network, process-creation or injection APIs.**
  `ShellExecuteW` is present for exactly one thing: opening Explorer at a
  path. `LoadLibraryW`/`VirtualProtect`/`CryptGenRandom` appear in the import
  table but are not called by any line of Spindle — they come from the MinGW
  runtime.
- Force removal adds exactly two capabilities to that list, and no more:
  `OpenProcess`/`TerminateProcess` to end a process holding a file open
  (found via the documented Restart Manager — `RmStartSession`, no handle
  table walking and no injection), and `SetNamedSecurityInfoW` /
  `AdjustTokenPrivileges` to take ownership. The registry is still never
  written: an uninstaller can map an application to its keys because it has
  an uninstall database, and a treemap cannot. Guessing at keys from a
  folder name would be a fast way to break a machine, so Spindle does not.
- DEP, ASLR, high-entropy ASLR and NO_SEH are on. Control Flow Guard is not:
  it is MSVC-only (`/guard:cf`) and GCC cannot emit it.
- A crash handler writes `spindle-crash.txt` with the fault address as a
  **module-base offset** — a raw address is meaningless under ASLR.

## Known limits

- Sizes are logical file length, so compressed, sparse and deduplicated files
  read larger than their footprint. Hardlinked files are counted once and
  marked as such on the MFT path, which is where the link count is free; the
  directory walker cannot see it without opening a handle per file, so a
  walked scan still double-counts a hardlink.
- Memory is roughly 90 bytes plus the filename per node — around 170 MB for a
  1.6M-file volume. A string pool and index-based tree would cut that
  substantially and is the obvious next optimisation.
- The MFT path has been verified by parser fuzzing and review, not by running
  against a real volume (the development environment is Linux). It falls back
  automatically on any failure.
- Cache revalidation is a full rescan running behind the cached view, not an
  incremental diff. Reading the NTFS USN journal to patch the cached tree in
  place is the obvious next step, and needs a real volume to develop against.
- A mapped network drive (one with a letter) is enumerated and scans like any
  other volume, over the directory walker rather than the MFT. What is not
  implemented is *discovery*: there is no way to browse to a UNC path like
  `\\server\share` that has not been mapped.
- Not implemented: duplicate detection, scan scheduling, UNC browsing,
  cloud-storage awareness, and any export format other than CSV.
- The binary is not code-signed, so a fresh download gets a SmartScreen
  warning until enough people run it. Signing needs a certificate, which
  needs a legal identity and an annual fee.
- x86-64 only. No 32-bit build, and no ARM64 build for Windows on ARM.
- The interface is drawn rather than composed from Win32 controls, so there
  is no screen-reader support: no UI Automation tree, no keyboard focus
  model beyond the search box. That is a real accessibility gap, not an
  oversight to rediscover later.

## Conventions

- Comments explain *why*, not what. If a line needs a comment to say what it
  does, rewrite the line.
- Every non-obvious constant gets a sentence on where the number came from.
- Tests assert invariants, not implementation. Several exist purely to encode
  a bug so it cannot come back.
- British spelling in prose; American in identifiers that mirror Win32.
