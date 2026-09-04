// Spindle - shared by the fuzz targets.
//
// Every target is a libFuzzer entry point, LLVMFuzzerTestOneInput, plus
// FuzzSeeds, which builds a few well-formed inputs from the same fixtures
// the unit tests use so the fuzzer starts from structure rather than from
// nothing. With clang and its runtime (libclang-rt-dev) `make fuzz-lib`
// runs them under libFuzzer; without, `make fuzz` links tests/fuzz_main.cpp,
// a standalone driver that replays the seeds and mutates them at random
// under the sanitizers. The targets never know which.
#pragma once

#include "../src/spindle.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
void FuzzSeeds(std::vector<std::vector<uint8_t>>& out);

// core.cpp's CSV writer defers the actual file write to the platform.
// Nothing here exports, so the host build supplies a stub.
namespace spindle {
bool WriteTextFileUtf8(const std::wstring&, const std::wstring&) { return false; }
}  // namespace spindle

// A property the parser must keep whatever it was fed. Trapping, rather
// than returning, is what makes the fuzzer treat it as a finding.
#define FUZZ_REQUIRE(cond)                                                  \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::fprintf(stderr, "FUZZ_REQUIRE failed: %s (%s:%d)\n", #cond, \
                         __FILE__, __LINE__);                               \
            __builtin_trap();                                               \
        }                                                                   \
    } while (0)
