// Windows-side checks of the pieces that only exist on Windows: where a
// path is judged to live, what may be cached, and the sealed cache round
// trip. Built with the same toolchain as the program and run natively on
// the Windows CI job, or under Wine on a developer box. Every check is
// about a promise made to a deployment reviewer, so each one names it.
#include "spindle.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace spindle;

#include "check.h"

SUITE(TestClassify, "Classify") {
    // A UNC path is a network location by spelling and its identity is the
    // share, however deep the path or how the prefix is spelled.
    NetPlace a = ClassifyPath(L"\\\\nas\\share\\deep", true);
    CHECK(a.network && a.key == L"\\\\nas\\share", "UNC path: network, share identity");
    a = ClassifyPath(L"\\\\?\\UNC\\nas\\share\\x", false);
    CHECK(a.network && a.key == L"\\\\nas\\share", "long-path UNC folds to the share");
    a = ClassifyPath(L"//nas/share", true);
    CHECK(a.network, "forward-slash UNC is still network");

    // Spellings that could reach a share without naming one are network
    // locations with no identity: asked every time, never remembered.
    for (const wchar_t* odd : {L"UNC\\nas\\share", L"GLOBALROOT\\Device\\Mup\\a\\b",
                               L"\\\\?\\GLOBALROOT\\Device\\Mup\\a\\b",
                               L"\\\\.\\UNC\\a\\b", L"folder", L"\\\\nas"}) {
        const NetPlace o = ClassifyPath(odd, true);
        CHECK(o.network && o.key.empty(), "odd spelling fails closed with no identity");
    }
    // A letter the system does not have is not vouched for either.
    DWORD mask = GetLogicalDrives();
    wchar_t missing = 0;
    for (int i = 25; i >= 2; --i) {
        if ((mask & (1u << i)) == 0) { missing = static_cast<wchar_t>(L'A' + i); break; }
    }
    if (missing != 0) {
        const std::wstring root = std::wstring(1, missing) + L":\\";
        const NetPlace m = ClassifyPath(root, true);
        CHECK(m.network && m.key.empty(), "an absent letter is treated as network");
        CHECK(!VolumeCacheable(root), "and is never cacheable");
    }
    // The system drive is local, and judged local without any link to walk.
    wchar_t sys[MAX_PATH] = {};
    if (GetWindowsDirectoryW(sys, MAX_PATH) > 2) {
        const std::wstring root = std::wstring(sys).substr(0, 3);
        CHECK(!ClassifyPath(root, true).network, "the system drive is local");
        CHECK(!ClassifyPath(sys, true).network, "a folder on it is local");
        CHECK(!VolumeCacheable(sys), "only a volume root can be cached");
    }
}

SUITE(TestSealedCache, "Sealed cache") {
    wchar_t sys[MAX_PATH] = {};
    if (GetWindowsDirectoryW(sys, MAX_PATH) <= 2) return;
    const std::wstring root = std::wstring(sys).substr(0, 3);
    if (!VolumeCacheable(root)) {
        std::printf("  (system drive not cacheable here; skipping)\n");
        return;
    }
    const std::wstring path = CachePathForVolume(root);
    if (path.empty()) return;

    ScanResult res;
    res.root.name = root;
    res.root.dir  = true;
    res.root.cat  = Cat::Directory;
    Node child;
    child.name = L"secret-project-name";
    child.size = 12345;
    child.cat  = Cat::Document;
    res.root.children.push_back(std::move(child));
    res.root.size        = 12345;
    res.root.files       = 1;
    res.stats.bytes      = 12345;
    res.stats.fileCount  = 1;
    CHECK(SaveScanCache(root, res), "a cache for the system drive saves");

    // On disk: sealed frame, and the name is not there in the clear.
    std::vector<uint8_t> bytes;
    {
        const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE, "cache file exists");
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz{};
            GetFileSizeEx(h, &sz);
            bytes.resize(static_cast<size_t>(sz.QuadPart));
            DWORD got = 0;
            ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr);
            CloseHandle(h);
        }
    }
    size_t off = 0;
    CHECK(SealedCachePayload(bytes.data(), bytes.size(), off), "the file is a sealed frame");
    const std::wstring needle = L"secret-project-name";
    bool clear = false;
    for (size_t i = 0; i + needle.size() * 2 <= bytes.size(); ++i) {
        if (std::memcmp(bytes.data() + i, needle.c_str(), needle.size() * 2) == 0) {
            clear = true;
            break;
        }
    }
    CHECK(!clear, "no folder name is readable in the file");

    ScanResult back;
    CacheMeta  meta;
    CHECK(LoadScanCache(root, back, meta, nullptr), "the cache unseals and loads");
    CHECK(back.root.children.size() == 1 &&
              back.root.children[0].name == L"secret-project-name",
          "and gives the tree back");

    // A plain (older) cache at the same slot is deleted, never served.
    {
        const HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, 0, nullptr);
        const char plain[] = "SPNC\x02\x00\x00\x00junk";
        DWORD wrote = 0;
        if (h != INVALID_HANDLE_VALUE) {
            WriteFile(h, plain, sizeof(plain), &wrote, nullptr);
            CloseHandle(h);
        }
    }
    CHECK(!LoadScanCache(root, back, meta, nullptr), "a plain cache is refused");
    CHECK(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES,
          "and removed");

    // A stray temp file is swept.
    {
        const HANDLE h = CreateFileW((path + L".tmp").c_str(), GENERIC_WRITE, 0,
                                     nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    PruneStaleCaches();
    CHECK(GetFileAttributesW((path + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES,
          "a leftover temporary is swept at launch");
}

int wmain(int argc, wchar_t** argv) {
    return spindle::testing::Main("Spindle Windows-side tests", argc, argv);
}
