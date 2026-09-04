# Changelog

Newest first. Every version is a GitHub release carrying `spindle.exe`
and `SHA256SUMS`, built by CI from the tagged commit.

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
