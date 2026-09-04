**What changed, and why**

**Checks**

- [ ] `make test` passes
- [ ] `make test-win` builds, and passes under Wine or on Windows where it applies
- [ ] `tools/hygiene.sh` is clean
- [ ] No third-party code
- [ ] Logic lives in the portable files with a host test; the interface only wires it up
