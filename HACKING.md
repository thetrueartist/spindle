# Spindle

A disk space analyser for Windows. Native C++17, Win32 + Direct2D, no runtime
and no third-party libraries. Single portable executable, about 1.5 MB.

Scans a volume, shows where the space went as a treemap coloured by file
kind, with a side panel for extension breakdown, largest files, search and
duplicates. Reads the NTFS Master File Table directly when it can, which
makes a million-file scan take well under a second.

## Commands

```
make            cross-compile build/spindle.exe (MinGW-w64)
make test       host tests under ASan, UBSan and ThreadSanitizer
make test-image the MFT assembler against a real NTFS image (ntfs-3g, root)
make test-win   Windows-side tests; run with `wine build/test_win.exe`
make fuzz       the parsers under random mutation, any compiler, ASan + UBSan
make fuzz-lib   the same targets under libFuzzer (clang + libclang-rt-dev)
make bench      the portable hot paths on a synthetic million-file volume
make stress     walk a real tree with the scanner's concurrency structure
make analyze    cppcheck + clang-tidy
make hygiene    refuse personal paths, secrets and typographic dashes
make hooks      run the hygiene check on every commit
make icon       regenerate spindle.ico (prints each frame's encoding)
make clean
```

Cross-compiles from Linux with `mingw-w64`; also builds under MSYS2. ARM64
builds with llvm-mingw via `make ARCH=aarch64`. Analysis targets need
`cppcheck` and `clang-tidy`. `make icon` needs Python 3 stdlib only.

## Layout

```
src/spindle.h        types, and the public interface of everything below
src/sync.h           SRWLOCK/CONDITION_VARIABLE, pthreads for host tests
src/workqueue.h      the scanner's work queue, testable on any host
src/ntfs.h/.cpp      NTFS on-disk structures, pure bytes, fuzz-tested
src/mfttree.h/.cpp   the tree built from MFT records, portable, tested on a real image
src/mft.cpp          raw volume I/O for the MFT scan (Windows)
src/scan.cpp         parallel FindFirstFileEx walker, volumes, file writing
src/core.cpp         treemap, categories, reports, search, CSV, easing
src/ui.cpp           Win32 window, Direct2D renderer, navigation, animation
res/                 icon, version info, manifest
tools/make_icon.py   icon generator, stdlib only
tools/hygiene.sh     what may never be committed; wired to the git hooks
tools/wine-*.sh      the interface harness and the acceptance run under Wine
tools/make-ntfs-image.sh  an NTFS image with a known tree, for the assembler test
tools/win-screenshots.ps1 draws the README images on a real Windows desktop
tests/               core, ntfs, mft, queue, stress; test_win for the Windows side
docs/guide.md        using the program, in full
docs/design.md       speed, architecture, text rendering, motion
docs/*.png, *.gif    README media, captured from the real binary under Wine
```

The split is deliberate: `core.cpp`, `ntfs.cpp`, `mfttree.cpp` and
`workqueue.h` contain no Windows headers, so they compile and run under
sanitizers on any host. That is where the tests live, and it is why the
fuzzing exists at all. Keep it that way. Anything that needs `windows.h`
belongs in `scan.cpp`, `mft.cpp` or `ui.cpp`.

The MFT scan is the clearest case. `mft.cpp` opens the volume and reads
the table in overlapped chunks, and that is all it does; every record goes
to `mfttree.cpp`, which decides what a parent reference is worth, what a
cycle or a chain past `kMaxTreeDepth` gets, and what the root must look
like for anything to attach. `make test` runs that assembler on tables
built to be hostile, and `make test-image` runs it on a real volume:
`tools/make-ntfs-image.sh` writes one with mkntfs, fills it through
ntfs-3g (long, spaced and non-ASCII names, a directory large enough to
need an index allocation, a forty-level chain, an empty file, a hard link)
and lists what it wrote, and the tree assembled from the image's own
table must match that list entry for entry.

## Invariants

Each of these encodes a bug that was actually hit. Breaking one
reintroduces it.

**Threading is Win32, never `<thread>`/`<mutex>`.** MinGW's win32 threading
model backs the standard types with an implementation that crashes under a
sixteen-thread scanner sustained for seconds. Use `SRWLOCK`,
`CONDITION_VARIABLE` and `_beginthreadex`. Both MinGW threading models must
produce identical imports.

**Thread entry points catch everything.** An exception escaping a thread
calls `std::terminate` and the process dies with no dialog. A large scan
does millions of allocations, so one `bad_alloc` used to take the whole app
down.

**Scans carry a generation number.** Clicking drive A then B before A
finished used to adopt A's tree while B was still running, and join B's
thread on the UI thread. Stale results are discarded by generation.

**Cells carry a parent index.** Cells nest up to five levels below the
viewed directory, so breadcrumb plus cell name is not the path. Getting
this wrong made "Move to Recycle Bin" target the wrong file. Reconstruct
paths via `CellChain()`.

**Expanded directories reserve a label strip.** A parent and its children
both drawing labels put them on top of each other, because children are
inset by only a few pixels. `Cell::header` and `Cell::expanded` drive this;
a cell too small for a strip yields its label to the children over it.

**All size arithmetic goes through `SatAdd`.** A volume reporting absurd
sizes must clamp, not wrap to something small and vanish from the map.

**Filenames are attacker-controlled.** `SanitizeForDisplay` strips C0/C1
controls, bidi overrides (U+202A to U+202E, U+2066 to U+2069) and
zero-width characters before display and before CSV export. Otherwise
`invoice\u202Egpj.exe` renders as `invoicexe.jpg`.

**Tree walks are iterative.** Directory nesting depth comes off the disk.
`RollUp`, `SortTree`, `ForEachNode` and the MFT tree build all use explicit
stacks. The one recursive function, `BuildRecursive` in the treemap, is
bounded twice over.

**Reparse points are never traversed.** Junctions and symlinks would loop
or double-count.

**The root of a cached tree is a path; every other name is a component.**
The root is named for the volume, for example `D:\`, so validating it as a
path component rejects it, and because an unreadable cache is deleted and
rescanned, the only symptom is that caching silently stops working
everywhere. One test fixture uses a real volume path for exactly this
reason.

**The cache slot belongs to the volume root alone.** A folder scan saved
into the volume's slot comes back on the next launch whatever folder was
asked for, and with a fresh enough cache the fast path never rescans, so
the window shows the wrong folder forever. Save and load both refuse
non-root paths, and load also refuses a cache whose stored root does not
name the requested volume, deleting it so the next clean scan replaces
it.

**A duplicate is never deleted on the strength of a hash.** Recycling a
copy from the Dupes panel is gated: it finds a different member of the same
group, proves the two identical byte for byte with `VerifyFilesIdentical`,
and only then offers to recycle. The last copy can never be the one
removed, and a 128-bit collision can never cause a deletion. Reparse points
are refused by the open, hardlinks are already excluded from groups, and
deletion goes to the Recycle Bin (reversible) behind a protected-path
refusal and a No-default confirmation. The failure mode is to refuse, never
to delete the wrong thing. The bulk "recycle every extra copy" run is
the same gate applied per file: the first copy of each set is the
keeper, every extra is verified against it immediately before its
recycle, a mismatch skips the rest of that set, and the summary reports
recycled and skipped counts exactly.

**A pair is confirmed by comparison, not by hashing.** Proving two files
identical requires reading both in full either way; hashing's only
advantage is turning the all-pairs comparison of a large group from
O(n squared) into O(n). A group of exactly two, the common case, has no
such advantage, so those are compared byte for byte: exact where a digest
is only near-certain, stopping at the first differing byte where a hash
must read to the end, and immune to crafted collisions. Groups of three or
more still go through the digest. Any deletion passes through this
verification before touching a file.

**A file is never read in full to prove it is different.** Sharing a size
makes two files possible duplicates, not actual ones, and reading whole
files to discover otherwise is ruinous: two 40 GB images differing in
their first block would cost 80 GB of reading. Comparison is tiered:
exact size (free), a 16 KB head, a 16 KB tail, and for files past 8 MB
(`kDeepProbeFile`) three interior probes at the quarter points, with the
middle first. Only what survives every probe is read in full. The tail
and interior tiers exist because disk images and media containers share
fixed headers and footers, so a head probe alone does not separate two
same-size VM images that differ only in the middle. Only a complete
digest may call two files equal, and every reader publishes progress
bytes per chunk, because a counter that only moves between files reads
as a hang during a 60 GB verify.

**Anything that reads the disk runs off the UI thread.** A window that
never pumps its message loop gets the spinning cursor and a Not
Responding title, and cannot receive the Esc-to-cancel its panel
advertises.
Candidates are chosen on the UI thread (a fast tree walk, and that thread
owns the tree), every `Node*` is stripped, and only owned paths cross to
the worker. Completion arrives as `WM_DUPES_DONE` carrying a generation
number, so a superseded report is freed rather than adopted.

**Nothing that holds a `Node*` may survive a tree swap.** `StartScan`
clears the panel caches (`fileList`, `extStats`, `rowHits`) and hover
state before `result.reset()`, and adopting a fresh tree marks the panel
dirty. The 0xC0000005 that forced this rule was the side panel drawing the
previous scan's file list mid-rescan, through pointers into the freed
tree.

**The scan cache is input, not state.** `%LOCALAPPDATA%\Spindle\*.spincache`
is parsed with the same posture as `ntfs.cpp`: every length, count and enum
validated, refusal bounds on totals, tree built iteratively with exact
reserves. It is keyed to the volume serial, written only for clean
uncancelled scans, and replaced atomically (temp file plus rename) so a
torn file cannot exist.

**Text rects are sized from `layout::kLine*`.** Formats set uniform line
spacing to those constants; a rect shorter than its line box gets cropped
by `DRAW_TEXT_OPTIONS_CLIP`. Paragraph alignment is centred so
over-generous rects degrade gracefully. Text alignment is a property of
the shared format object and must be restored after any non-leading draw.

**ICO frames: DIB up to 128, PNG only at 256.** Windows will not decode
PNG at small sizes, and `LoadImage` fails silently into the stock Windows
icon.

## Tabs

A view tab stores strings only: the scan root, the trail as component
names, the panel index and a title. Activating one re-loads its root
through the ordinary cache path and walks back down by name, stopping at
the deepest directory that still exists, so a tab can never dangle into
a freed tree and survives every rescan by construction. The active tab
mirrors the live view and is snapshotted the moment anything switches
away from it.

## Browse mode

A details list over the already-loaded tree, because the two things a
file lister is worst at on Windows (folder sizes, search speed) are the
two things this program already has. Design decisions:

- The list is a view of `trail.back()`'s children, nothing more. It
  reads no disk, so it is instant by construction, and every folder row
  shows its rolled-up size and file count because the tree already
  knows them.
- Columns: Name, Size, Kind, Files. No Modified column:
  `Node` deliberately stores no timestamps, and adding one is a cache
  format bump plus eight bytes per node, which is a separate decision.
- Sorting is per column, both directions, over an index vector owned by
  the view. The vector holds `Node*` into the live tree, so it obeys
  the tree-swap invariant: dropped in `DropTreeReferences`, rebuilt on
  demand.
- Rows are virtualised: only the visible span draws, so a 40,000-child
  directory scrolls like an empty one. Wheel plus a draggable thumb.
- Double-click a folder: navigate into it, staying in browse. Backspace
  and the breadcrumb behave exactly as on the map. Double-click a file:
  hand it to the shell to open, which is the one new power browse mode
  grants and is called out in the import audit.
- Right-click a row: the same menu as a map cell, same protected-path
  refusals, same confirmations, plus open in a new tab.
- The Map/List choice lives per tab, next to the breadcrumb, and a tab
  remembers it like it remembers its panel and search.
- Selection speaks the standard grammar: click, ctrl-click toggle,
  shift-click range, empty-space clear. The set holds Node pointers and
  is dropped with every tree swap like every other holder. A
  right-click inside a bigger selection acts on all of it.
- Rename is a real EDIT child window over the name cell, not a drawn
  imitation: the control brings the caret, selection and IME for free.
  Commit order is disk first (MoveFileW after IsSafeNodeName), then the
  tree patched in place by walking to the parent by name, then the
  cache of that drive deleted, so the next launch rescans rather than
  serving a name one edit out of date. A walk that misses patches
  nothing and the next scan shows the truth.

## Launch prefetch

At startup the fixed drives that are not on screen are walked one at a
time by a background scan whose only product is the cache file. The tree
is freed on the worker thread and never shown. Invariants: at most one
walk at a time; it is cancelled (and the drive re-queued) the moment a
foreground scan or a duplicate hunt wants the disk; removable and network
volumes are never queued; the whole thing is off when `keep_caches` or
`prefetch_all` is. A cache younger than five minutes (`kFreshCacheMs`) is
served without the revalidating rescan, which is what makes clicking
between drives free. The prefetch has its own `Progress` and generation
counter so cancelling it can never touch the foreground scan's flags, and
`CancelPrefetch` bumps the generation before joining so the
already-posted completion message is recognised as stale.

## Auto-update

A release is only an update once a manifest signed by the maintainer's
key says so, and producing that signature takes a human approval. The
updater fails closed on anything unsigned, malformed, mismatched,
replayed or unreachable.

Mechanics: at launch, when the launch check is on, a worker fetches the latest
release metadata from the GitHub API over WinHTTP, finds manifest.json
and manifest.sig among the assets, verifies the signature (ECDSA P-256
via CNG), requires the manifest to name the release's own tag and to
carry a serial newer than the last one this copy accepted, and only
then offers the update: a menu entry naming the version, never anything
silent. Accepting downloads the exe asset, capped at the signed size,
hashes it (SHA-256 via CNG), requires the hash the signed manifest
promised, then swaps by rename: the running exe moves aside as
spindle.exe.old, the verified download takes its name, and the user
restarts when they choose. No process is created, the old version
survives until the next launch cleans it, and every failure leaves the
current install untouched. Hosts are allowlisted to GitHub's API and
release servers, and at most one redirect between them is followed.

The JSON off the API is untrusted input, so the field extractor lives
in core.cpp with hard bounds and host tests, like every other parser
of hostile bytes. The accepted serial is persisted in the settings file,
which is parsed with the same care.

Signing runs in `.github/workflows/sign.yml`, called by the release
workflow after it publishes. The job lives in the `release-signing`
environment, which holds the private key as a secret and requires the
maintainer's approval before any run may start, so the key is never
available to anything unattended: a push, a hijacked action or a
workflow trigger gets as far as "waiting for approval" and stops. The
job downloads the release asset, refuses it unless it is byte for byte
what the same run built, signs it, verifies the result against the
embedded public key (so a wrong secret uploads nothing), checks the
manifest names the right tag and hash, and attaches the pair. It can
also be run by hand for an existing tag. `tools/sign-release.ps1` is
the offline fallback and does the same signing locally. The signer
lives in the exe itself: `--gen-update-key` writes a keypair,
`--sign-release` hashes a build and writes manifest.json plus
manifest.sig, and `--verify-update-manifest` checks a pair against a
public key.

The embedded public key constant decides whether any of this runs at
all: empty means no key, no network and no menu entry. A fork that
wants its own update channel replaces the constant with its own public
key; leaving someone else's in place would mean trusting their
signatures.

## Security posture

The threat model is that every byte off a disk (names, sizes, on-disk
structures) is untrusted input, read while elevated.

**Parsing.** `ntfs.cpp` treats every byte as hostile. Every field goes
through a cursor that cannot read past its buffer; no length, offset or
count from disk is used without validation, and the parser is fuzzed as
part of `make test`. The volume itself is opened `FILE_READ_DATA`,
sharing read, write and delete.

**Names off the disk are not path components until they are checked.**
`IsSafeNodeName` refuses a backslash, a forward slash, a colon, a NUL, the
Win32-reserved characters, `.` and `..`, and a trailing dot or space. It is
applied in `ntfs.cpp` where `$FILE_NAME` is read and in `ReadRecord` where
the cache is. NTFS's POSIX namespace permits every one of those, and the
cache file is writable without elevation while being read with it, so a
name could otherwise carry a separator and aim a later delete somewhere it
was never displayed. An embedded NUL is the sharpest version: the panel
draws the whole string, `CreateFileW` stops at the NUL, and the two are
different files.

**The protected-path check and the Win32 name resolver must agree.**
`IsProtectedSystemPath` compares strings, but every caller hands the same
string to an API that re-parses it, so any spelling Win32 resolves
differently is a bypass. Forward slashes, `.` and `..` components, trailing
dots or spaces (`C:\Windows ` opens `C:\Windows`), 8.3 aliases
(`PROGRA~1`), drive-relative forms (`C:Windows`, which resolves against a
current directory that is System32 under UAC) and embedded NULs are all
refused outright rather than normalised. Deletion then runs on the `\\?\`
form, which disables canonicalisation, so the object deleted is the one
that was checked.

**Trees are bounded in depth wherever they are built.** `Node` owns its
children by value, so the compiler's destructor recurses once per level; a
tree deeper than the stack can unwind cannot be freed, and that crash
cannot be caught. The walker, the cache reader and the MFT builder all
produce trees, and all stop at `kMaxTreeDepth`, with the walker one level
short so that what it writes is what the readers accept. The cache also
caps the running total of declared children against the declared node
count, and bounds the node count by memory rather than by file size,
because the reader reserves each directory's children up front.

**Ordinary deletion** is recycle-bin only, confirmed (twice for folders,
No the default both times), has no keyboard shortcut, and refuses anything
that looks like a volume root. The `SHFileOperationW` buffer is built with
an explicit double NUL.

**Force removal** is the one genuinely destructive feature: permanent
deletion, taking ownership when the ACL refuses, and terminating the
processes holding the target open. Force applies to locks and permissions,
never to the confirmations. It is a separate menu item below the
reversible one, never the default, has no shortcut, and passes three
gates: the protected-path refusal, a permanent-deletion warning stating
the size and file count, and a named list of the processes that would be
ended. No is the default on every one.

What force removal may never touch lives in `core.cpp`, not in the dialog.
`IsProtectedSystemPath` covers drive roots, every UNC path, `\Windows`,
`System Volume Information`, `$Recycle.Bin`, `Recovery`, `Boot` and the
boot files at and below, plus the `Program Files`, `ProgramData`, `Users`
and `PerfLogs` roots themselves (their contents are allowed).
`IsCriticalProcess` covers pid 4 and below, smss, csrss, wininit,
winlogon, services, lsass, svchost, dwm, and spindle itself. Both are
host-tested. `ForceRemove` re-checks the path itself so no caller can
route around the dialog, and the tree walk re-checks per directory because
it is following names off the disk. The Recycle Bin path runs the same
refusal, since the reversible option guarding less than the permanent one
made no sense.

A locking process is identified by its image name, never by the Restart
Manager's `strAppName`. That field is a display string from the version
resource (`lsass.exe` presents as "Local Security Authority Process"), so
a list written in image names would match none of the processes it
exists to protect. The Restart Manager's own `RmCritical`/`RmService`
classification is trusted first, the image name is re-read from the
handle about to be killed, and the process creation time is compared so a
recycled PID cannot redirect the kill. Only the processes that were
listed in the confirmation are ended; one that took the lock afterwards
makes the delete fail rather than dying unseen.

Taking ownership acts on a handle opened `FILE_FLAG_OPEN_REPARSE_POINT`,
never on a path, and it merges into the existing DACL. A path-based form
would follow junctions, so a link aimed at a system directory plus a
deny ACE on the link would be enough to rewrite the target's owner and
permissions, and passing a null old ACL would discard every existing
entry, DENY aces included. Escalation is skipped entirely for reparse
points and for files with more than one link: a link's own ACL is not
why a delete failed, and a hardlink shares one security descriptor with
every other name for the file. Reparse points are removed as links,
never followed, and the deletion walk confirms through a handle that a
directory is still a directory before enumerating it, so one swapped for
a junction mid-walk is refused rather than followed.

**The registry.** The Explorer folder-menu entry is the one registry write
in the program. It is off by default, ticked on from the menu, writes
two keys under `HKCU\Software\Classes\Directory\shell\Spindle`
(per-user, no elevation, nothing machine-wide), and unticking deletes
exactly what it created. The command is written with `"%1"` quoted,
because a folder name with a space otherwise arrives as several
arguments. Nothing else touches the registry, and force removal in
particular does not: an uninstaller can map an application to its keys
because it has an uninstall database, and a treemap cannot. Guessing at
keys from a folder name would be a fast way to break a machine.

**Reading file contents.** Finding duplicates is the only feature that
reads file contents, so it never runs on its own; it is a button, not a
panel refresh. Only files sharing an exact size with another are opened,
cloud placeholders are excluded, and the handle is opened
`FILE_FLAG_OPEN_NO_RECALL` besides, so a placeholder cannot be silently
downloaded to be hashed.

**Imports.** No process-creation or injection APIs, and the only network
use is the updater's WinHTTP client. `ShellExecuteW` is present to open
Explorer at a path, and to open a file from browse mode, and for nothing
else. `LoadLibraryW`, `VirtualProtect` and `CryptGenRandom` appear in the
import table but are not called by any line of Spindle; they come from
the MinGW runtime. Force removal adds exactly two capabilities:
`OpenProcess`/`TerminateProcess` to end a process holding a file open
(found via the documented Restart Manager, `RmStartSession`, with no
handle-table walking and no injection), and `SetSecurityInfo` on a handle with
`AdjustTokenPrivileges` to take ownership.

**Hardening.** DEP, ASLR, high-entropy ASLR and NO_SEH are on. Control
Flow Guard is not: it is MSVC-only (`/guard:cf`) and GCC cannot emit it.
A crash handler writes `spindle-crash.txt` with the fault address as a
module-base offset, because a raw address is meaningless under ASLR.

## Known limits

- Sizes are logical file length, so compressed, sparse and deduplicated
  files read larger than their footprint. Hardlinked files are counted
  once and marked as such on the MFT path, where the link count is free;
  the directory walker cannot see it without opening a handle per file,
  so a walked scan still double-counts a hardlink.
- Memory is roughly 90 bytes plus the filename per node, around 170 MB
  for a 1.6M-file volume. A string pool and index-based tree would cut
  that substantially and is the obvious next optimisation.
- There is no automated test against a real NTFS volume; the MFT path is
  covered by the parser tests and falls back to the directory walk on any
  failure.
- Cache revalidation is a full rescan running behind the cached view, not
  an incremental diff. Reading the NTFS USN journal to patch the cached
  tree in place is the obvious next step, and needs a real volume to
  develop against.
- A mapped network drive (one with a letter) is enumerated and scans like
  any other volume, over the directory walker rather than the MFT. A UNC
  path can be scanned by passing it on the command line. What is not
  implemented is discovery: there is no way to browse to an unmapped
  share from inside the window.
- CSV is the only export format.
- The binary is not code-signed, so a fresh download gets a SmartScreen
  warning until enough people run it. Signing needs a certificate, which
  needs a legal identity and an annual fee.
- The interface is drawn rather than composed from Win32 controls, so
  there is no screen-reader support: no UI Automation tree, no keyboard
  focus model beyond the search box. That is a real accessibility gap,
  not an oversight to rediscover later.

## Tests

Every test binary is built on `tests/check.h`: a suite is a function
declared with `SUITE(Identifier, "Name")`, and `CHECK(condition, "what it
means")` records one assertion. A failure prints the file and line, the
message, the expression and the suite, so one red line is enough to go
on. The binaries take `--list`, `--filter NAME` (a case-insensitive
substring) and `--repeat N`, and the Makefile passes `ARGS` through:

```
make test-core ARGS='--filter cache'
make test-queue ARGS='--repeat 20'      # a race that shows once in ten runs is still a race
wine build/test_win.exe --filter sealed
```

The host builds carry `_GLIBCXX_ASSERTIONS`, so an out-of-range index in a
standard container fails loudly under test rather than reading whatever
happens to be there. Each test binary depends on exactly what it compiles,
so `make test` after a one-line change rebuilds one test, not four.

Every parser has a fuzz target in `tests/fuzz_*.cpp`, written to the
libFuzzer entry point and seeded from the same fixtures the unit tests
use. `make fuzz-lib` runs them coverage-guided under clang, which CI does
for thirty seconds each on every push; `make fuzz` links the same targets
against `tests/fuzz_main.cpp`, a standalone driver that replays the seeds
and mutates them at random, so they run on a box without libFuzzer and a
crash file from CI can be replayed with `build/fuzz_<target> FILE`. A
`FUZZ_REQUIRE` inside a target is a property the parser must keep whatever
it was fed, and a failed one names itself.

`make bench` times the portable hot paths on a synthetic million-file
volume, median of five runs on an optimised build. Run it before and after
a change to the tree, the cache, the panels or the assembler; the numbers
that matter are in docs/design.md.

## Testing the interface under Wine

Logic lives in the portable files and is covered by `make test`. The Win32
interface is exercised by hand under Wine with `tools/wine-ui-test.sh`,
which runs a window manager so text boxes receive keyboard focus and reads
the client origin from xwininfo so clicks land where intended. Both are
easy to get wrong, and a bare Xvfb silently gets both wrong, so any visual
claim about a control (does it accept typing, does the font match) is only
trustworthy through that harness, never from an undecorated headless X.
Where a feature has pure logic behind it, that logic is split into the
portable files and host-tested; the interface then only wires it up.

`tools/wine-corpo-check.sh` runs the network-permission and cache-policy
acceptance checks end to end under Wine and prints a PASS/FAIL line per
check. It wants a prefix with Y: registered as a `network` drive and E:
as `floppy` under `HKLM\Software\Wine\Drives`, a fixed D: with content,
and a folder at `dosdevices/unc/nas/share`, which is how Wine spells
`\\nas\share`. Wine gives a mapped letter no share name, so the checks
that need a remembered identity use the UNC path.

## Conventions

- Comments explain why, not what. If a line needs a comment to say what
  it does, rewrite the line.
- Every non-obvious constant gets a sentence on where the number came
  from.
- Tests assert invariants, not implementation. Several exist purely to
  encode a bug so it cannot come back.
- British spelling in prose; American in identifiers that mirror Win32.
