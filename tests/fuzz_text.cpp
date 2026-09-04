// Fuzz target: the parsers that take text a person typed or pasted (the
// Find query, a share name, a command line, a path for completion) and the
// UTF-8 conversion underneath them. The input is UTF-8; what it decodes to
// is what a window would hand over.
#include "fuzz_common.h"

using namespace spindle;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    const uint8_t which = data[0] % 4;
    const std::string text(reinterpret_cast<const char*>(data + 1), size - 1);
    const std::wstring wide = Utf8ToWide(text);
    switch (which) {
        case 0: {
            const Query q = ParseQuery(wide);
            (void)q;
            break;
        }
        case 1: {
            const std::wstring key = NormalizeShareKey(wide);
            if (!key.empty()) {
                FUZZ_REQUIRE(key.size() <= kMaxShareKeyChars);
                FUZZ_REQUIRE(NormalizeShareKey(key) == key);
                for (const wchar_t c : key) FUZZ_REQUIRE(c >= 0x20);
            }
            break;
        }
        case 2: {
            std::vector<std::wstring> args;
            size_t at = 0;
            while (at <= wide.size() && args.size() < 16) {
                const size_t nl = wide.find(L'\n', at);
                args.push_back(wide.substr(at, nl == std::wstring::npos
                                                   ? std::wstring::npos : nl - at));
                if (nl == std::wstring::npos) break;
                at = nl + 1;
            }
            const CommandLine cl = ParseCommandLine(args);
            if (!cl.valid) FUZZ_REQUIRE(!cl.error.empty());
            break;
        }
        case 3: {
            const std::string back = WideToUtf8(wide);
            FUZZ_REQUIRE(Utf8ToWide(back) == wide);
            const std::wstring shown = SanitizeForDisplay(wide);
            FUZZ_REQUIRE(shown.size() <= wide.size());
            break;
        }
    }
    return 0;
}

void FuzzSeeds(std::vector<std::vector<uint8_t>>& out) {
    auto add = [&](uint8_t which, const char* text) {
        std::vector<uint8_t> s{which};
        s.insert(s.end(), text, text + std::char_traits<char>::length(text));
        out.push_back(s);
    };
    add(0, "kind:media >500mb -temp \"two words\" ext:vmdk is:folder size:>1.5gb");
    add(0, "D:\\Games\\Northwind Online\\Content");
    add(1, "\\\\NAS\\Share\\deep\\folder");
    add(1, "\\\\?\\UNC\\nas\\share");
    add(2, "--csv\nout.csv\n--allow-network\n\\\\nas\\share");
    add(2, "--duplicates\n1048576\nD:\\");
    add(3, "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E \xE2\x80\xAE" "gpj.exe");
}
