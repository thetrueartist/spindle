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

Release builds check for updates at launch, which the menu can turn off.
An update is accepted only if a manifest signed with the maintainer's
ECDSA P-256 key vouches for the exact SHA-256 and size of the downloaded
file, names the release's own tag, and carries a serial newer than the
last one this copy accepted, so a genuine but superseded manifest cannot
be replayed to hold users on an old version. The updater talks only to
GitHub's API and release hosts, follows at most one redirect between
them, and caps the download at the signed size. A build with the public
key constant emptied never opens a socket at all.

The private key is a GitHub environment secret that is released only to
the signing job, and that job cannot start until the maintainer approves
the run. Nothing unattended can sign, and neither can anything that
merely has write access to this repository, its Actions or its releases:
a compromise there can publish a release, but every installed copy keeps
refusing it until a person approves signing it, and the approval screen
shows exactly which commit and tag are being signed. What this does not
defend against is a compromise of the maintainer's own GitHub account,
which could approve a run. That is the trade made for signing being a
click rather than a ritual, and it is why that account is expected to
carry strong two-factor authentication. Signing can also be done offline
with `tools/sign-release.ps1` should the hosted path ever be in doubt.

The updater fails closed on anything unsigned, altered, replayed or
unreachable, and it never installs anything without being asked.

## Build integrity

Releases are built by GitHub Actions from a tagged commit and carry a
`SHA256SUMS` file. The release workflow and its signing job hold the only
write permission in CI, the signing job refuses an asset that is not byte
for byte what the same run built, and the third-party action the
workflow uses is pinned to a commit rather than a movable tag.

The binary is not code-signed with an Authenticode certificate, so
SmartScreen will warn on a fresh download until the file earns
reputation. That is a known gap, not an oversight.
