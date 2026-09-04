// Fuzz target: settings.txt. Plain text anything running as the user can
// write, so every field is bounded and a parse must round-trip through the
// serialiser without change.
#include "fuzz_common.h"

using namespace spindle;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const Settings s = ParseSettings(data, size);
    FUZZ_REQUIRE(s.trustedShares.size() <= kMaxTrustedShares);
    for (const std::string& share : s.trustedShares) {
        FUZZ_REQUIRE(!share.empty());
        FUZZ_REQUIRE(NormalizeShareKey(Utf8ToWide(share)) == Utf8ToWide(share));
    }
    for (const char c : s.lastPath) FUZZ_REQUIRE(static_cast<unsigned char>(c) >= 0x20);

    std::vector<uint8_t> out;
    SerializeSettings(s, out);
    FUZZ_REQUIRE(out.size() <= kMaxSettingsBytes);
    const Settings back = ParseSettings(out.data(), out.size());
    FUZZ_REQUIRE(back.trustedShares == s.trustedShares);
    FUZZ_REQUIRE(back.updateSerial == s.updateSerial);
    FUZZ_REQUIRE(back.keepCaches == s.keepCaches);
    FUZZ_REQUIRE(back.checkUpdates == s.checkUpdates);
    FUZZ_REQUIRE(back.rememberView == s.rememberView);
    // The last place is written only while "remember where I was" is on,
    // so it survives a round trip exactly then and is dropped otherwise.
    if (s.rememberView) FUZZ_REQUIRE(back.lastPath == s.lastPath);
    else FUZZ_REQUIRE(back.lastPath.empty());
    FUZZ_REQUIRE(back.lastPanel == s.lastPanel);
    FUZZ_REQUIRE(back.cleanupOld == s.cleanupOld);
    return 0;
}

void FuzzSeeds(std::vector<std::vector<uint8_t>>& out) {
    const char* text =
        "keep_caches=1\nresume_on_launch=1\nprefetch_all=0\ncheck_updates=1\n"
        "remember_view=1\nlast_browse=0\nlast_panel=2\n"
        "trusted_share=\\\\nas\\share\ntrusted_share=\\\\fileserver\\public\n"
        "cleanup_old=0\nupdate_serial=1756742400\n"
        "last_path=D:\\Games\\Northwind Online\n";
    out.emplace_back(text, text + std::char_traits<char>::length(text));
    Settings s;
    s.trustedShares = {"\\\\nas\\share"};
    s.rememberView = true;
    s.lastPath = "C:\\Users\\Public";
    s.updateSerial = 42;
    std::vector<uint8_t> bytes;
    SerializeSettings(s, bytes);
    out.push_back(bytes);
    out.push_back({0xEF, 0xBB, 0xBF, 'k', 'e', 'e', 'p', '_', 'c', 'a', 'c',
                   'h', 'e', 's', '=', '0', '\n'});
}
