# Changelog

Newest first. Every version is a GitHub release carrying `spindle.exe`
and `SHA256SUMS`, built by CI from the tagged commit.

## 2.6.2

- "Open in a new tab" opens where you right-clicked. Inside All drives it
  opened the aggregate's root instead, from the map and from the Largest
  and Find lists alike. A tab opened while a stale cache was being
  revalidated went back to the root when the walk finished, and so did a
  remembered place; a tab switched to while another drive was being read
  landed on its drive's root; and a duplicate's tab, on a drive not on
  screen, lost the file it was opened for.
- Clicking a drive or a tab while another drive was loading from its
  cache, or while All drives was being gathered, could leave the window
  on "Stopping the current scan" for good: the stopped load never said
  it had stopped, so the click waited forever. Every stop now reports in.
- "Show in Explorer" on a folder opens that folder, rather than its
  parent with the folder selected, which for anything at the top of a
  drive looked like Explorer had opened the drive's root. A file still
  opens its folder with the file selected.
- Security: "Show in Explorer" never follows a link and never runs a
  file. A junction or directory symlink, which an elevated scan lists as
  an empty folder, is selected where it sits rather than opened, since it
  can point anywhere, a server included. And a path the shell could not
  resolve fell back to opening its folder with the "open" verb, which on
  a folder since replaced by a script would have run it; it now uses
  "explore", which no file has. The only new import is `ShellExecuteExW`.
- Paths listed under All drives no longer carry a doubled separator after
  the drive (`C:\\Users`), so Copy path gives the path as it is.
- Under the hood: `tools/win-newtab-check.ps1` checks these on a real
  Windows desktop in CI, and `tools/wine-newtab-check.sh` the All drives
  ones under Wine.

## 2.6.1

- "Remember where I was" brings back the All drives view. It remembered
  the place but not the view, so a launch from the aggregate's root fell
  back to the freshest drive, and one from a folder inside it reopened
  that folder on its own drive instead.

## 2.6.0

- The tree is built from the Master File Table faster: the category
  lookup is a compile-time hash table, the record parser builds each
  name once and the build reuses its scratch buffer, which took 400,000
  records from 506 ms to 395 ms on the benchmark volume; a cached map
  reads back a quarter faster.
- The volume's own metadata files ($MFT, $Extend and the rest) are never
  offered as duplicate candidates, so a hunt no longer reports two
  unreadable files on every fresh NTFS volume.
- An extension rule written in mixed case (resS) matches.
- A remembered path longer than the settings writer will ever emit is
  refused on read as well as on write.
- Under the hood: the MFT assembly is a portable unit proven against a
  real NTFS image in CI, every parser has a fuzz target run on every
  push, one harness runs every test binary, `make bench` times the hot
  paths, and the README images are drawn on a real Windows desktop.

## 2.5.14

- The cache is sealed in an envelope: a random key per file protected
  by DPAPI, the tree under AES-256-GCM in the same process. Opening a
  cached drive is instant again. Caches sealed by 2.5.7 to 2.5.13 still
  open.
- Clicking the drive already on screen returns to its root; F5 rescans.

## 2.5.13

- The status bar says what the worker is doing when the counters
  cannot: reading the file table, building the map, loading the cached
  map.

## 2.5.12

- The last two heavy reads moved off the interface thread, and the side
  panel survives a rescan.
- Starting a scan never waits for the background walk to stop.

## 2.5.11

- All drives says what it is doing and never waits on a running scan to
  start another.
- Every screenshot and the walkthrough refreshed.

## 2.5.10

- A path is judged from local tables only, the device table and stored
  link targets, and fails closed; the docs say exactly what is written
  to disk.
- The network-policy acceptance run under Wine, and share-key fuzzing.

## 2.5.9

- Closed the ways around the network permission: headless mode, Tab
  completion, symbolic links, SUBST and GLOBALROOT spellings.

## 2.5.8

- A UNC path in the address bar opens a share that has no drive letter,
  with permission granted per share.

## 2.5.7

- Every scan cache sealed to the Windows account that wrote it.

## 2.5.6

- Only internal disks are cached and no listing outlives its media;
  turning caching off deletes every cache.

## 2.5.5

- Ask before reading a network drive, and remember the answer per share.

## 2.5.4

- Fixed the address bar swallowing every keystroke, a 2.5.3 regression,
  and made interface testing under Wine reliable.

## 2.5.3

- Tab completion in the address bar against the real filesystem.
- The Find and Largest result lists scroll.

## 2.5.2

- Text boxes use the window's own font, size and line.

## 2.5.1

- Themed address and rename boxes, All drives at the top of the list, a
  format specifier fixed.

## 2.5.0

- All drives: the whole machine under one root, in map, list and search.

## 2.4.0

- The breadcrumb doubles as an address bar; path terms in Find; the list
  stops above the status bar.
- Remember where I was: reopen the last drive, folder and view.
- Recycling and cache loading off the interface thread; Find no longer
  freezes per keystroke; a drive switch interrupts at once.
- Releases signed in CI behind a manual approval gate; CodeQL analysis.

## 2.3.1

- Fixed the MFT scan returning an empty tree on every real volume.
- Signing a release is one command.

## 2.3.0

- The security release: parsers hardened against crafted volumes and
  cache files, the findings of four reviews fixed, and the repository
  prepared for going public.

## 2.2.0

- The updater is live: builds carry the release key's public half.

## 2.1.0

- Secure auto-update, dormant until keyed.

## 2.0.0

- Browse mode: a details list where every folder already has a size,
  multi-select, selection actions, inline rename and a way back up.
- The duplicate report exports to CSV. MIT licence.

## 1.9

- 1.9.0: tabs, so folders and duplicates open in their own views.
- 1.9.1: every sidebar row opens in a new tab.
- 1.9.2: pooled duplicate hunts read every volume in parallel. 1.9.3 is
  the same build, re-released.

## 1.8

- 1.8.0: every fixed drive is walked at launch so clicking one is
  always instant; the README rewritten in plain prose.
- 1.8.1: a rescan crash fixed, duplicate progress kept moving,
  duplicates shown on the map.
- 1.8.2: hunts survive drive switches and outrank the freshness rescan;
  the mouse stops a hunt.
- 1.8.3: recycle every extra copy in one run; the folder-scan cache
  mix-up fixed.

## 1.7.0

- Recycle a duplicate copy, gated on proving a copy remains.

## 1.6.0

- A duplicate pair is confirmed by exact comparison, not by hashing.

## 1.5

- 1.5.0: duplicates pooled across drives, results kept, opened from the
  panel.
- 1.5.1: the hash loads its words with memcpy.

## 1.4.0

- Duplicates compared in tiers instead of reading every candidate whole.

## 1.3

- 1.3.1: hardlinks, cloud placeholders, duplicates, scan comparison, a
  command line, the Explorer entry, the ARM64 target, and the security
  review's findings fixed.
- 1.3.2: the duplicate hunt runs off the interface thread.

## 1.2.0

- Force removal: permanent delete, ownership, lock breaking.

## 1.1

- 1.1.0: first release on GitHub.
- 1.1.1: reveal-in-Explorer fixed, search editing, calmer crash
  handling, settings kept beside the caches, MFT reads overlapped with
  parsing.
