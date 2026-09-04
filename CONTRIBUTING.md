# Contributing

Bug reports and pull requests are welcome. A few things make them easy
to take.

## Before you start

- Read [HACKING.md](HACKING.md): the layout, the invariants (each one
  encodes a bug that was actually hit) and the conventions.
- No third-party code. Spindle links only Windows system DLLs and ships
  as one file. A dependency is a design change to discuss first, not a
  contribution to send.
- Logic goes in the portable files (`core.cpp`, `ntfs.cpp`,
  `workqueue.h`) with a host test; the interface only wires it up.
  Anything that needs `windows.h` belongs in `scan.cpp`, `mft.cpp` or
  `ui.cpp`, and its checks in `tests/test_win.cpp`.

## Building and testing

```
make              # cross-compile build/spindle.exe (MinGW-w64)
make test         # host tests under ASan, UBSan and ThreadSanitizer
make test-image   # the MFT assembler against a real NTFS image (ntfs-3g, root)
make test-win     # Windows-side tests; run with wine build/test_win.exe
make stress       # the scanner's concurrency structure over a real tree
make analyze      # cppcheck + clang-tidy
make hooks        # run the hygiene check on every commit
```

A single suite runs with `make test-core ARGS='--filter cache'`; HACKING.md
describes the harness.

`tools/wine-ui-test.sh` drives the real window under Wine for
interface changes, and `tools/wine-corpo-check.sh` runs the
network-permission and cache-policy acceptance checks end to end.
HACKING.md describes the Wine prefix they want.

## Pull requests

- One change per pull request, with the reason in the description.
- A fix comes with the test that would have caught it. Tests assert
  invariants, not implementation.
- Comments explain why, not what. Every non-obvious constant gets a
  sentence on where the number came from.
- Prose in British spelling; identifiers that mirror Win32 keep theirs.
- Run `tools/hygiene.sh`, or `make hooks` once, before committing. It
  refuses personal paths, secrets and typographic dashes in the tree
  and in commit messages.

## Security problems

Use private vulnerability reporting rather than an issue; see
[SECURITY.md](SECURITY.md).
