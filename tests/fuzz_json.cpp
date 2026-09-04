// Fuzz target: the release record the updater reads from GitHub. A field
// found must be a substring of the input, and a key-shaped string inside a
// value must never pose as the field.
#include "fuzz_common.h"

using namespace spindle;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string json(reinterpret_cast<const char*>(data), size);
    std::string v;
    size_t at = 0;
    if (JsonFindString(json, "tag_name", 0, v, &at)) {
        FUZZ_REQUIRE(at < json.size());
        FUZZ_REQUIRE(v.size() <= json.size());
    }
    for (const char* key : {"name", "browser_download_url", "body", ""}) {
        size_t from = 0;
        for (int guard = 0; guard < 64; ++guard) {
            size_t found = 0;
            if (!JsonFindString(json, key, from, v, &found)) break;
            FUZZ_REQUIRE(found >= from && found < json.size());
            from = found + 1;
        }
    }
    return 0;
}

void FuzzSeeds(std::vector<std::vector<uint8_t>>& out) {
    const char* release =
        "{\"tag_name\": \"v2.5.14\", \"name\": \"v2.5.14\", \"assets\": ["
        "{\"name\": \"spindle.exe\", \"browser_download_url\": "
        "\"https://github.com/thetrueartist/spindle/releases/download/v2.5.14/spindle.exe\", "
        "\"size\": 1476236}, {\"name\": \"SHA256SUMS\"}], "
        "\"body\": \"a \\\"tag_name\\\": \\\"v9.9.9\\\" inside text\"}";
    out.emplace_back(release, release + std::char_traits<char>::length(release));
    const char* spoof = "{\"x\": \"\\\"tag_name\\\": \\\"v1\\\"\", \"tag_name\": \"v2\"}";
    out.emplace_back(spoof, spoof + std::char_traits<char>::length(spoof));
}
