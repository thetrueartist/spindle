# Using Spindle

Everything the window and the command line do, in one place. The
README is the short version; this is the long one.

## Running it

`spindle.exe` is self-contained. Double-click it.

Run it elevated if you want the whole picture. Without elevation Windows
hides `Program Files\WindowsApps`, `System Volume Information` and other
users' profiles, which on a typical system adds up to tens of gigabytes.

| | |
|---|---|
| Click a drive | scan it |
| Click a block | descend into it |
| Ctrl+F | search names |
| Ctrl+E | export to CSV |
| Backspace / mouse-back | go up |
| Click a breadcrumb | jump to that level |
| Ctrl+L, or click the current folder's name | type or paste a path, Enter goes there |
| Right-click a block | show in Explorer, copy path, recycle, force remove |
| F5 | rescan |
| Esc | cancel a running scan |

## What the map shows

Colour carries information. WinDirStat assigns cell colours arbitrarily, so
its map only shows sizes. Spindle colours each file by what it is (media,
archives, source, programs, disk images, databases), decided at scan time
from the extension, so you can tell what a drive is full of before reading
any labels.

Scanning reads directory entries, never file contents. Sizes come from
`WIN32_FIND_DATA` and no handle is opened on any file being scanned, so
nothing gets locked, and files held under an exclusive kernel lock such as
`pagefile.sys` and `hiberfil.sys` still report their real size.

Reparse points are never traversed. Junctions and directory symlinks
cannot cause loops or double counting.

## Caches

At launch, the fixed drives are walked once in the background, one at a
time, and the status bar notes it while it runs. After a few minutes every
drive answers a click instantly. Finished scans are cached under
`%LOCALAPPDATA%\Spindle`. A cache under five minutes old is served as-is;
an older one is shown immediately while a rescan revalidates behind it, and
the status bar reads `cached 2h ago · rescanning` until it finishes. F5
always forces a fresh walk, whatever the view. Removable and network drives are never read
unprompted, the background walk stops the moment you ask for anything
else, and the whole thing can be turned off in the `···` menu.

What a cache holds, for anyone who has to sign off on it: the folder
tree of one drive, each entry as a name, a size, a file count, a kind
and three flags (folder, hardlink, cloud placeholder), under one header
carrying the time the scan finished, the volume serial number and the
drive's totals. No file contents, no hashes, no per-file dates, no
owners. It is a listing of the
drive, so it is treated as one: only an internal fixed disk ever gets a
cache. Removable media, disks on a USB, FireWire or card-reader bus, and
network shares are never cached, so no listing outlives its media. At
launch any cache whose drive is gone, or of a kind that should never
have had one, is deleted, and turning Keep scan caches off deletes every
cache rather than merely stopping new ones. The trade-off is that an
external disk rescans each time it is opened.

Each cache is sealed before it touches the disk: a fresh random key per
file is protected with Windows data protection (DPAPI), and the tree
itself is encrypted under that key with AES-256-GCM in the same process,
which is why sealing costs nothing you can see. The key belongs to your
Windows account, Windows manages it, and Spindle never stores one. The cache can be read only by
a process holding that Windows account's DPAPI key: the same account on
the same machine, or, for a domain account, the same account on another
machine where Windows has made its key available. A copy, backup or disk
image of the cache cannot be read without that account's credentials.
Anyone holding the account's password and profile, or the domain's DPAPI
backup key, can read it, and can read the drive anyway.
It is not a defence against the account itself or code running as it,
which can read the drive anyway. Caches from earlier builds were plain;
they are deleted rather than read, so each drive rescans once.

The cache
is keyed to the volume serial and parsed with the same validation as
everything else read from disk.

## Network drives

Network drives are handled the way a company policy would want by
default. A mapped share is listed, marked `network` on its card, and read
only after you click it and answer a question that names the share and
says what a scan involves. Tick "remember" and that share never asks
again, up to 32 remembered shares; past that the status bar says so and
the answer lasts for this run. Leave it unticked and the answer lasts
for this run. The memory is
the share itself, `\\server\share`, not the drive letter, so a letter
mapped somewhere else later asks afresh, and a mapping with no name is
asked every time and never remembered. A folder that is a symbolic link,
junction or SUBST into a share counts as that share, wherever the path
started. Nothing at launch reaches a share: a network letter is
recognised from the local device table and listed by its letter alone,
without asking the server for a label or free space, until you allow it;
a path given on the command line or by the Explorer entry is judged from
the local link and device tables before the question is asked, and a
path that cannot be judged is treated as a network location. "Forget remembered network drives" in the `···` menu
clears every saved answer. The scan reads names and sizes; finding
duplicates on a share reads file contents too, and the permission text
says so.

![the question asked before a network drive is read](network.png)

## The list and the address bar

The map has a twin: a details list. The Map | List toggle beside the
breadcrumb switches the same view between the treemap and a list with
name, size, kind and file count columns, and because the list is a view
over the scan the program already holds, every folder row shows its
real rolled-up size instantly, sorted by any column in either
direction. Double-click descends, the ".." row and Backspace come back
out, and right-click carries the same actions as the map plus rename.
Selection speaks the usual grammar (click, ctrl, shift), the status bar
sums what is selected, and a right-click on the selection copies every
path or recycles the lot behind one confirmation. The breadcrumb doubles
as an address bar: click the folder you are in, or press Ctrl+L, and type
or paste a path; Enter goes there on any drive, and a path that names a
file outlines that file's cell. A UNC path such as `\\server\share` works
too, for a share that has no drive letter: it asks for permission the way
a network drive does, then opens. Tab completion lists a folder to do its
job, so on a share it works only once that share has been allowed. Tab completes the path against the real
filesystem, folders first, so "E:\Stea" and Tab becomes "E:\SteamLibrary\"
ready to keep going.

![the breadcrumb as an address bar](addressbar.png)

Recycling runs in the background: the status bar says which item is
moving and how far along the run is, Esc stops it after the current
item, and the window stays usable throughout.

![browse mode](browse.png)

## Tabs

Views come in tabs. Right-click anything and open it in its own tab: a
folder on the map, a row in Largest or Find, a duplicate (its drive
opens with the file outlined), or a Kinds row, which opens a tab running
that extension's search. Each tab remembers its drive, its position in
the tree, its panel and its search, and switching is instant because
the caches already hold every drive. The strip only appears once there
are two tabs to choose from.

![tabs](tabs.png)

## All drives

An "All drives" card at the bottom of the drive list opens every volume
at once, under one view. The map shows each drive as a top-level block
and descends into folders across the machine; the list shows the drives
as rows; and Find, Kinds and Largest all span every drive, so a search
there covers the whole computer rather than one volume. Cached drives
appear at once, a fixed drive with no cache yet is walked in the
background, removable drives are left out, an external disk that Windows
reports as fixed is walked but never cached, and a network drive joins
only if you have already allowed that share.

Filenames are treated as untrusted input; see [SECURITY.md](../SECURITY.md).

## Force removal

Force removal handles the things that will not delete normally: locked
files, folders with broken ACLs, leftovers from an uninstaller that no
longer exists. Right-click, then Force remove. It deletes permanently,
takes ownership when access is denied, and ends the processes holding the
target open, found through the Restart Manager the same way Windows finds
them. No injection, no handle-table tricks.

Force applies to locks and permissions, not to the confirmations. It is a
separate menu item below the reversible delete, it is never the default,
and it has no keyboard shortcut. It refuses protected paths outright,
then asks before the permanent deletion, stating size and file count,
and asks a second time, naming the processes it would end, whenever
something has the target open. No is the default answer each time. Drive roots, `\Windows`, `System Volume
Information`, the boot files and the `Program Files`, `ProgramData` and
`Users` roots are refused in the removal code itself, not just in the
dialog. It never terminates a system process and never touches the
registry.

## Find

The Find panel takes a small query language. Bare words match the name,
prefixed terms narrow by kind, extension, size or type, and every term
must match.

![Find with a path term, results carrying their folders](find.png)

| | |
|---|---|
| `pak` | name contains "pak" |
| `"two words"` | quoted phrase |
| `kind:media` | one of the categories from the Kinds panel |
| `ext:vmdk` | exact extension |
| `>500mb` `<2gb` | size bounds in b/kb/mb/gb/tb, or bare bytes |
| `size:>1.5gb` | same, spelled out; fractions accepted |
| `is:file` `is:folder` | restrict to one or the other |
| `-temp` | name must not contain "temp" |

`kind:media >500mb -temp` reads as "large media files that aren't
temporary", which is usually the real question when a drive fills up, and
typing it is faster than a row of dropdowns would be to operate.

A term containing a backslash matches the full path instead of the name,
so pasting a folder lists everything under it and pasting a file's path
finds that file. A query that is entirely a path is taken whole, spaces
included.

Size bounds apply to a folder's rolled-up total, so `is:folder >10gb`
finds the directories worth opening. Kind tokens accept several spellings
(`vm`, `vms`, `disk` and `iso` all reach the disk-image category) because
people type what they mean rather than what the enum is called.

Clicking a row in the Kinds panel searches for that extension, so the
breakdown doubles as a way into the results. The Find and Largest lists
scroll with the mouse wheel when there is more than fits, with a slim
thumb marking the position. An unrecognised token
becomes a name term rather than an error.

## Duplicates

![the Dupes panel, pooled across drives](duplicates.png)

Finding duplicates means reading file contents, which the scanner
otherwise never does, so it only happens when you press the button in the
Dupes panel, when a duplicate is verified before recycling, or when
`--duplicates` is given on the command line. That is, from the button in the
Dupes panel. The search runs in tiers so that very little is read in full:

1. Same size. Files of different lengths cannot be identical, so this
   free test eliminates almost everything.
2. First 16 KB, then last 16 KB, and for files past 8 MB another 16 KB
   at the quarter, middle and three-quarter marks. Most same-size files
   differ within the first few kilobytes. Disk images are the awkward
   case: two different fixed-size VM images can share the boot sector at
   the head and the footer at the tail, and the interior probes reject
   those for kilobytes where reading them out would cost gigabytes.
3. Full content, only for what survives. A group of exactly two, the
   common case, is confirmed by comparing the files byte for byte, which
   is exact and stops at the first difference. Groups of three or more
   are grouped by a 128-bit digest, which is O(n) reads instead of the
   O(n squared) of comparing every pair.

Measured on twenty 100 MB files that share a size and differ at the first
byte, the worst case for tools that hash everything, Spindle reads 320 KB
where hashing each file in full would read 2 GB.

Cloud placeholders are never downloaded for comparison. OneDrive files
are recognised and skipped, and the read path refuses a file that became
a placeholder after the scan rather than fetching it. Hardlinks are
excluded because they are already the same bytes and deleting one frees
nothing.

Click a result and Spindle shows the file on its own map: it switches to
the right drive, walks the tree to the file and outlines its cell, and
while the Dupes tab is open every file in the report carries a thin
outline so the duplicates are visible in place. Right-click a result for
Explorer, or to recycle that one copy.

Recycling in bulk is a single button: "Recycle every extra copy" keeps
the first copy of each set and sends the rest to the Recycle Bin, after
one confirmation that states the file count, the set count and the
total size, with No as the default. The run shows its progress, can be
stopped by Esc or a click, and ends with an exact accounting of what
moved and what was skipped.

Either way, a copy is recycled only after Spindle has re-verified byte
for byte that a different, identical file still exists, so a hash match
alone never deletes anything, the last copy can never be the one
removed, and a stale result cannot delete the wrong thing. A set that
no longer matches is skipped whole and said so. Deletion always goes to
the Recycle Bin and always asks first.

The report also exports: "Export duplicates to CSV" in the menu writes
one row per file with its set number, copy count, sizes and full path,
so a spreadsheet can take the decision over.

The second button, "Across every scanned drive", pools candidates from
every drive's remembered scan. That finds the file that exists once on
`C:` and once on `D:`, which a single-folder search cannot see. Each row
shows which drive it is on. The reading fans out one worker per volume,
so three drives read at three drives' speed; only files whose size class
spans volumes are checked in a shared batch, which is what keeps the
grouping global.

## What's really reclaimable

Sizes are logical file length, which overstates what deleting would
actually free. The status bar reports the two cases that matter:

Hardlinked bytes exist once no matter how many names point at them.
WinSxS is built this way and most of its apparent size is not
recoverable. On the MFT path, where the link count comes for free,
hardlinked files are marked and their bytes totalled separately.

Cloud-only bytes are not on the disk at all. A OneDrive placeholder has a
nominal size but occupies little or no local space, so it is counted
separately instead of being folded into a figure that claims local space.

## Comparing scans

The cache keeps the last finished scan of each drive, so answering "what
ate my disk this week" needs nothing extra. Compare with the cached scan,
in the `···` menu, diffs the current tree against the cache: what grew,
shrank, appeared and vanished, largest movement first, with a one-megabyte
floor to keep the list short. A folder that appeared is reported once at
its root rather than as thousands of files inside it.

## Command line

The command line lets Task Scheduler handle scheduling. Nothing can ask
for permission there, so `--csv` refuses a network location, exit code 3,
unless that share was remembered in the window or `--allow-network` is on
the command line in as many words. The sidebar lists lettered volumes
only, so a share without a letter is opened by pasting its path into the
address bar or by giving it here.

```
spindle.exe [path]                 open the window on a volume, folder or
                                     \\server\share
spindle.exe --csv <file> <path>    scan, write CSV, exit; no window
spindle.exe --duplicates [bytes] <path>   also find duplicates; prints a one-line summary
spindle.exe --version | --help
```

With `--csv` nothing is shown and the exit code is 0 on success, 1 when
the scan found nothing or the file could not be written, 2 for a command
line that could not be parsed, and 3 for a network location it may not
read. The path must be a full one: a drive letter or `\\server\share`.

The `···` menu can remember where you were: turn on "Remember
where I was" and the next launch reopens the same drive and folder in the
same map or list view. It is off by default, so a normal launch still
opens the drive with the freshest scan.

Spindle can also add a "Scan with Spindle" entry to folder right-click
menus, from the `···` menu. It is off by default and is the only registry
write in the program: per-user under `HKCU`, no elevation, and unticking
removes exactly the keys that ticking created.

## Known limits

A DFS namespace is remembered as one share, its root, so allowing it
allows every link beneath it. A user-mode filesystem that presents itself
as a fixed disk (some cloud and archive mounts) is treated as one. A disk
in a Thunderbolt enclosure may report as internal and be cached.

Sizes are logical file length, so compressed and sparse files read larger
than their footprint on disk. Hardlinked bytes are measured on the MFT
path and cloud-only bytes on the directory walk, so an elevated NTFS scan
reports hardlinks but not placeholders and an unelevated or non-NTFS
scan the reverse. Hardlinked and cloud-only bytes are the two
cases large enough to matter and both are measured and reported;
compression is not.
