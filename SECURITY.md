# Security

Spindle runs with more trust than most desktop software: it reads raw
volume structures, it can be run elevated, and it can delete files. That
is worth being explicit about.

## Reporting a vulnerability

Use GitHub's private vulnerability reporting on this repository
(Security tab, "Report a vulnerability"). That opens a channel visible
only to the maintainer. Please do not open a public issue for anything
exploitable.

Include what you did, what happened, and what you expected. A crafted
volume image, cache file or directory layout that reproduces the problem
is worth more than a description of it.

## What the program does

- **Scanning never opens the files it scans.** Sizes come from directory
  entries, or from the NTFS Master File Table when the process is
  elevated. Reparse points are recorded but never traversed.
- **Reading file contents happens in exactly one feature**, the duplicate
  finder, and only when the button is pressed. Cloud placeholders are
  excluded and the handle is opened so that a placeholder cannot be
  silently downloaded.
- **Deletion is always confirmed**, defaults to No, has no keyboard
  shortcut, and refuses drive roots, `\Windows`, the boot files and the
  other protected locations in `IsProtectedSystemPath`. That refusal
  lives in the removal code, not in the dialog, so no caller can route
  around it.
- **Force removal** is the one genuinely destructive feature: permanent
  deletion, taking ownership when an ACL refuses, and ending processes
  that hold the target open through the documented Restart Manager. It
  never touches the registry, never terminates a system process, and
  asks three separate times.
- **The registry is written by one optional feature**, the Explorer
  folder-menu entry, per user under `HKCU`, and unticking it removes
  exactly the keys ticking it created.

## What the program treats as hostile

Every byte that comes off a disk or a network: filenames, sizes, NTFS
structures, the scan cache, and the update manifest. Parsers validate
every length, offset and count, bound their allocations and their tree
depth, and are fuzzed as part of `make test`. Filenames are sanitised
before display and before CSV export, and are refused as path components
if they contain separators, reserved characters or trailing dots and
spaces.

## Updates

The updater is dormant unless a release signing key is embedded in the
build. When it is enabled, an update is accepted only if a manifest
signed with the maintainer's ECDSA P-256 key vouches for the exact
SHA-256 of the downloaded file, and the manifest names the same tag the
release does.

The private key is offline and exists on no build machine, no CI runner
and nothing GitHub can reach. A full compromise of this repository, its
Actions, or the maintainer's GitHub account therefore still cannot
deliver an update to anybody: the attacker can publish a release, but
they cannot sign one. The updater fails closed on anything unsigned,
altered, replayed or unreachable, and it never installs anything without
being asked.

## Build integrity

Releases are built by GitHub Actions from a tagged commit and carry a
`SHA256SUMS` file. The workflow that publishes them holds the only write
permission in CI, and the third-party action it uses is pinned to a
commit rather than a movable tag.

The binary is not code-signed with an Authenticode certificate, so
SmartScreen will warn on a fresh download until the file earns
reputation. That is a known gap, not an oversight.
