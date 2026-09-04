// Standalone driver for the fuzz targets, for a box without libFuzzer.
//
//   fuzz_x FILE...           replay inputs
//   fuzz_x --seeds DIR       write the target's seeds into DIR, one file
//                            each, as a corpus for libFuzzer
//   fuzz_x --random N [--seed S]
//                            N rounds of random mutation over the seeds,
//                            under whatever sanitizers the binary carries
//
// Not a substitute for coverage-guided fuzzing, but enough to keep every
// target buildable and runnable everywhere, and to replay a crash file.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
void FuzzSeeds(std::vector<std::vector<uint8_t>>& out);

namespace {

constexpr size_t kMaxInput = 1u << 16;

bool ReadFile(const char* path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        out.insert(out.end(), buf, buf + n);
    }
    std::fclose(f);
    return true;
}

void Mutate(std::vector<uint8_t>& v, std::mt19937& rng) {
    std::uniform_int_distribution<int> ops(0, 6);
    const int rounds = 1 + static_cast<int>(rng() % 8);
    for (int r = 0; r < rounds; ++r) {
        if (v.empty()) v.push_back(static_cast<uint8_t>(rng()));
        const size_t at = rng() % v.size();
        switch (ops(rng)) {
            case 0: v[at] = static_cast<uint8_t>(rng()); break;
            case 1: v[at] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;
            case 2: v[at] = (rng() & 1) ? 0xFF : 0x00; break;
            case 3: {   // insert a few random bytes
                const size_t n = 1 + rng() % 16;
                if (v.size() + n > kMaxInput) break;
                std::vector<uint8_t> ins(n);
                for (uint8_t& b : ins) b = static_cast<uint8_t>(rng());
                v.insert(v.begin() + static_cast<std::ptrdiff_t>(at), ins.begin(), ins.end());
                break;
            }
            case 4: {   // delete a range
                const size_t n = 1 + rng() % 16;
                v.erase(v.begin() + static_cast<std::ptrdiff_t>(at),
                        v.begin() + static_cast<std::ptrdiff_t>(std::min(v.size(), at + n)));
                break;
            }
            case 5: {   // duplicate a range
                const size_t n = 1 + rng() % 64;
                const size_t end = std::min(v.size(), at + n);
                if (v.size() + (end - at) > kMaxInput) break;
                std::vector<uint8_t> dup(v.begin() + static_cast<std::ptrdiff_t>(at),
                                         v.begin() + static_cast<std::ptrdiff_t>(end));
                v.insert(v.begin() + static_cast<std::ptrdiff_t>(end), dup.begin(), dup.end());
                break;
            }
            case 6: v.resize(at); break;   // truncate
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::vector<uint8_t>> seeds;
    FuzzSeeds(seeds);

    long rounds = 0;
    unsigned seed = 1;
    const char* seedDir = nullptr;
    std::vector<const char*> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--random" && i + 1 < argc) rounds = std::atol(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = static_cast<unsigned>(std::atol(argv[++i]));
        else if (a == "--seeds" && i + 1 < argc) seedDir = argv[++i];
        else files.push_back(argv[i]);
    }

    if (seedDir) {
        for (size_t i = 0; i < seeds.size(); ++i) {
            const std::string path = std::string(seedDir) + "/seed-" + std::to_string(i);
            FILE* f = std::fopen(path.c_str(), "wb");
            if (!f) { std::printf("cannot write %s\n", path.c_str()); return 1; }
            std::fwrite(seeds[i].data(), 1, seeds[i].size(), f);
            std::fclose(f);
        }
        std::printf("%zu seeds written to %s\n", seeds.size(), seedDir);
        return 0;
    }

    for (const char* path : files) {
        std::vector<uint8_t> in;
        if (!ReadFile(path, in)) { std::printf("cannot read %s\n", path); return 1; }
        LLVMFuzzerTestOneInput(in.data(), in.size());
        std::printf("replayed %s (%zu bytes)\n", path, in.size());
    }

    // The seeds themselves, then mutations of them.
    for (const auto& s : seeds) LLVMFuzzerTestOneInput(s.data(), s.size());
    if (rounds > 0) {
        std::mt19937 rng(seed);
        for (long r = 0; r < rounds; ++r) {
            std::vector<uint8_t> in = seeds.empty() ? std::vector<uint8_t>()
                                                    : seeds[rng() % seeds.size()];
            Mutate(in, rng);
            LLVMFuzzerTestOneInput(in.data(), in.size());
        }
        std::printf("%ld mutations of %zu seeds: no fault\n", rounds, seeds.size());
    }
    return 0;
}
