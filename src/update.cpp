// Auto-update, dormant until keyed. A release becomes an update only when
// a manifest signed by the maintainer's OFFLINE key says so, and every
// failure - unsigned, malformed, mismatched, unreachable - leaves the
// current install untouched. See HACKING.md for the design.

#include "spindle.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <string>
#include <vector>
#include <cstring>

namespace spindle {

// The maintainer's release-signing public key: base64 of a 64-byte ECDSA
// P-256 public point (X then Y, big-endian). EMPTY means the whole feature
// is off: no network, no menu entry, nothing. Enabling it is the owner's
// deliberate act: spindle.exe --gen-update-key, keep the private half
// offline, paste the public half here, rebuild.
static const char kUpdatePublicKeyB64[] =
    "QTPCug6/3n/9X4E3jp/FC8zMsPCkCmW+i46q3PsdfvhweKGqUyKXxOSVsgFxDhsy"
    "zSk590AXaNVcEsLVOsmNCQ==";

bool UpdateFeatureEnabled() { return kUpdatePublicKeyB64[0] != '\0'; }

// ------------------------------------------------------------------ base64

static bool B64Decode(const std::string& in, std::vector<uint8_t>& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\r' || c == '\n') continue;
        const int v = val(c);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

static std::string B64Encode(const uint8_t* p, size_t n) {
    static const char* tab =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = static_cast<uint32_t>(p[i]) << 16;
        if (i + 1 < n) v |= static_cast<uint32_t>(p[i + 1]) << 8;
        if (i + 2 < n) v |= p[i + 2];
        out.push_back(tab[(v >> 18) & 63]);
        out.push_back(tab[(v >> 12) & 63]);
        out.push_back(i + 1 < n ? tab[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < n ? tab[v & 63] : '=');
    }
    return out;
}

// ------------------------------------------------------------------- BCrypt

static bool Sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    0) != 0) {
        return false;
    }
    bool ok = false;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        if (BCryptHashData(h, const_cast<PUCHAR>(data),
                           static_cast<ULONG>(len), 0) == 0 &&
            BCryptFinishHash(h, out, 32, 0) == 0) {
            ok = true;
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static std::string HexOf(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(d[p[i] >> 4]);
        out.push_back(d[p[i] & 15]);
    }
    return out;
}

// Verify sig (raw r||s, 64 bytes) over data with an explicit public key.
// The embedded-key overload below is the production path; this one also
// serves --verify-update-manifest, which is how the sign-verify loop is
// exercised without rebuilding with a test key.
static bool VerifySignatureWith(const uint8_t* data, size_t len,
                                const std::vector<uint8_t>& sig,
                                const std::string& pubB64) {
    std::vector<uint8_t> pub;
    if (!B64Decode(pubB64, pub) || pub.size() != 64 || sig.size() != 64) {
        return false;
    }
    uint8_t digest[32];
    if (!Sha256(data, len, digest)) return false;

    std::vector<uint8_t> blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    hdr->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    hdr->cbKey   = 32;
    std::memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), pub.data(), 64);

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM,
                                    nullptr, 0) != 0) {
        return false;
    }
    bool ok = false;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                            blob.data(), static_cast<ULONG>(blob.size()),
                            0) == 0) {
        ok = BCryptVerifySignature(key, nullptr, digest, 32,
                                   const_cast<PUCHAR>(sig.data()),
                                   static_cast<ULONG>(sig.size()), 0) == 0;
        BCryptDestroyKey(key);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static bool VerifySignature(const uint8_t* data, size_t len,
                            const std::vector<uint8_t>& sig) {
    return VerifySignatureWith(data, len, sig, kUpdatePublicKeyB64);
}

bool VerifyManifestFile(const std::wstring& manifestPath,
                        const std::wstring& sigPath,
                        const std::wstring& pubB64) {
    auto readAll = [](const std::wstring& path,
                      std::vector<uint8_t>& out) {
        const HANDLE h =
            CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                        nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        bool ok = GetFileSizeEx(h, &size) != 0 && size.QuadPart > 0 &&
                  size.QuadPart < (1ll << 20);
        if (ok) {
            out.resize(static_cast<size_t>(size.QuadPart));
            DWORD got = 0;
            ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()),
                          &got, nullptr) != 0 &&
                 got == out.size();
        }
        CloseHandle(h);
        return ok;
    };
    std::vector<uint8_t> manifest, sigRaw;
    if (!readAll(manifestPath, manifest) || !readAll(sigPath, sigRaw)) {
        return false;
    }
    // The sig file may carry a BOM or trailing newline; strip to base64.
    std::string sigText;
    for (uint8_t b : sigRaw) {
        const char c = static_cast<char>(b);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=') {
            sigText.push_back(c);
        }
    }
    std::vector<uint8_t> sig;
    if (!B64Decode(sigText, sig)) return false;
    std::string pubUtf;
    for (wchar_t c : pubB64) pubUtf.push_back(static_cast<char>(c));
    // The manifest file was written as UTF-8 with a BOM; the signature
    // covers the bytes WITHOUT it, as the signer hashed them.
    size_t skip = 0;
    if (manifest.size() >= 3 && manifest[0] == 0xEF &&
        manifest[1] == 0xBB && manifest[2] == 0xBF) {
        skip = 3;
    }
    return VerifySignatureWith(manifest.data() + skip,
                               manifest.size() - skip, sig, pubUtf);
}

// ------------------------------------------------------------------ WinHTTP

// One host, HTTPS, default certificate validation, tight timeouts, and a
// hard cap on how much we will read. Anything else fails the update, not
// the program.
static bool HttpsGet(const std::wstring& host, const std::wstring& path,
                     std::vector<uint8_t>& out, size_t maxBytes) {
    out.clear();
    HINTERNET ses = WinHttpOpen(L"Spindle-Updater",
                                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, 8000, 8000, 8000, 15000);
    bool ok = false;
    HINTERNET con = WinHttpConnect(ses, host.c_str(),
                                   INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (con) {
        HINTERNET req = WinHttpOpenRequest(
            con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (req) {
            const wchar_t* hdrs =
                L"User-Agent: Spindle\r\nAccept: */*\r\n";
            if (WinHttpSendRequest(req, hdrs, static_cast<DWORD>(-1),
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0, cb = sizeof(status);
                WinHttpQueryHeaders(
                    req,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &cb,
                    WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    for (;;) {
                        DWORD avail = 0;
                        if (!WinHttpQueryDataAvailable(req, &avail) ||
                            avail == 0) {
                            ok = true;
                            break;
                        }
                        if (out.size() + avail > maxBytes) break;
                        const size_t at = out.size();
                        out.resize(at + avail);
                        DWORD got = 0;
                        if (!WinHttpReadData(req, out.data() + at, avail,
                                             &got)) {
                            break;
                        }
                        out.resize(at + got);
                    }
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(con);
    }
    WinHttpCloseHandle(ses);
    if (!ok) out.clear();
    return ok;
}

// GitHub asset downloads redirect to a storage host; follow exactly one
// hop, and only to an https URL.
static bool HttpsGetFollowingOneRedirect(const std::wstring& host,
                                         const std::wstring& path,
                                         std::vector<uint8_t>& out,
                                         size_t maxBytes);

static bool SplitHttpsUrl(const std::string& url, std::wstring& host,
                          std::wstring& path) {
    if (url.rfind("https://", 0) != 0) return false;
    const size_t hostAt = 8;
    const size_t slash = url.find('/', hostAt);
    if (slash == std::string::npos || slash == hostAt) return false;
    host.assign(url.begin() + static_cast<long>(hostAt),
                url.begin() + static_cast<long>(slash));
    path.assign(url.begin() + static_cast<long>(slash), url.end());
    return true;
}

// -------------------------------------------------------------- the check

struct UpdateInfo {
    std::string tag;        // e.g. "v2.1.0"
    std::string exeUrl;     // browser_download_url of spindle.exe
    std::string sha256;     // promised by the signed manifest
};

// Fetch the latest release, verify its signed manifest, and fill `info`.
// Every early return is the fail-closed path.
static bool FetchVerifiedUpdate(UpdateInfo& info) {
    if (!UpdateFeatureEnabled()) return false;

    std::vector<uint8_t> body;
    if (!HttpsGet(L"api.github.com",
                  L"/repos/thetrueartist/spindle/releases/latest", body,
                  1u << 20)) {
        return false;
    }
    const std::string json(body.begin(), body.end());

    std::string tag;
    if (!JsonFindString(json, "tag_name", 0, tag) || tag.empty() ||
        tag.size() > 64) {
        return false;
    }

    // Walk the assets by their repeated "name" keys; each name's URL is
    // the next "browser_download_url" after it.
    std::string manifestUrl, sigUrl, exeUrl;
    size_t at = 0;
    for (;;) {
        std::string name;
        size_t here = 0;
        if (!JsonFindString(json, "name", at, name, &here)) break;
        at = here + 1;
        std::string url;
        if (!JsonFindString(json, "browser_download_url", here, url)) {
            continue;
        }
        if (name == "manifest.json") manifestUrl = url;
        else if (name == "manifest.sig") sigUrl = url;
        else if (name == "spindle.exe") exeUrl = url;
    }
    if (manifestUrl.empty() || sigUrl.empty() || exeUrl.empty()) {
        return false;
    }

    std::wstring host, path;
    std::vector<uint8_t> manifest, sigRaw;
    if (!SplitHttpsUrl(manifestUrl, host, path) ||
        !HttpsGetFollowingOneRedirect(host, path, manifest, 64u << 10)) {
        return false;
    }
    if (!SplitHttpsUrl(sigUrl, host, path) ||
        !HttpsGetFollowingOneRedirect(host, path, sigRaw, 4u << 10)) {
        return false;
    }
    // The files were written as text: the manifest may carry a UTF-8 BOM
    // (the signature covers the bytes without it) and the sig file may
    // carry a BOM or newline around its base64.
    size_t skip = 0;
    if (manifest.size() >= 3 && manifest[0] == 0xEF &&
        manifest[1] == 0xBB && manifest[2] == 0xBF) {
        skip = 3;
    }
    std::string sigText;
    for (uint8_t b : sigRaw) {
        const char c = static_cast<char>(b);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=') {
            sigText.push_back(c);
        }
    }
    std::vector<uint8_t> sig;
    if (!B64Decode(sigText, sig) ||
        !VerifySignature(manifest.data() + skip, manifest.size() - skip,
                         sig)) {
        return false;
    }

    // Only now is the manifest trusted enough to read.
    const std::string mjson(manifest.begin() + static_cast<long>(skip),
                            manifest.end());
    std::string mtag, masset, msha;
    if (!JsonFindString(mjson, "tag", 0, mtag) ||
        !JsonFindString(mjson, "asset", 0, masset) ||
        !JsonFindString(mjson, "sha256", 0, msha)) {
        return false;
    }
    // The manifest must be about this release and this artifact, or a
    // valid old manifest could be replayed onto a newer tag.
    if (mtag != tag || masset != "spindle.exe" || msha.size() != 64) {
        return false;
    }

    info.tag    = tag;
    info.exeUrl = exeUrl;
    info.sha256 = msha;
    return true;
}

static bool HttpsGetFollowingOneRedirect(const std::wstring& host,
                                         const std::wstring& path,
                                         std::vector<uint8_t>& out,
                                         size_t maxBytes) {
    // First try the plain GET; WinHTTP follows same-scheme redirects on
    // its own by default, so this usually just works.
    return HttpsGet(host, path, out, maxBytes);
}

// ------------------------------------------------------------- public API

// Compare "v2.1.0"-style tags numerically, so v2.10 beats v2.9.
static bool TagIsNewer(const std::string& tag, const wchar_t* current) {
    auto parts = [](const std::string& t, unsigned v[3]) {
        v[0] = v[1] = v[2] = 0;
        size_t i = (!t.empty() && (t[0] == 'v' || t[0] == 'V')) ? 1 : 0;
        for (int p = 0; p < 3 && i < t.size(); ++p) {
            unsigned n = 0;
            while (i < t.size() && t[i] >= '0' && t[i] <= '9') {
                n = n * 10 + static_cast<unsigned>(t[i] - '0');
                ++i;
            }
            v[p] = n;
            if (i < t.size() && t[i] == '.') ++i;
        }
    };
    std::string cur;
    for (const wchar_t* p = current; *p; ++p) {
        cur.push_back(static_cast<char>(*p));
    }
    unsigned a[3], b[3];
    parts(tag, a);
    parts(cur, b);
    if (a[0] != b[0]) return a[0] > b[0];
    if (a[1] != b[1]) return a[1] > b[1];
    return a[2] > b[2];
}

bool CheckForUpdate(const wchar_t* currentVersion, std::wstring& tagOut) {
    UpdateInfo info;
    if (!FetchVerifiedUpdate(info)) return false;
    if (!TagIsNewer(info.tag, currentVersion)) return false;
    tagOut.assign(info.tag.begin(), info.tag.end());
    return true;
}

// Download, hash, require the promised hash, swap by rename. Returns a
// short status the caller can show; empty on success.
std::wstring ApplyUpdate(const wchar_t* currentVersion) {
    UpdateInfo info;
    if (!FetchVerifiedUpdate(info)) {
        return L"The update could not be verified, so nothing was changed.";
    }
    if (!TagIsNewer(info.tag, currentVersion)) {
        return L"This is already the newest version.";
    }
    std::wstring host, path;
    std::vector<uint8_t> exe;
    if (!SplitHttpsUrl(info.exeUrl, host, path) ||
        !HttpsGetFollowingOneRedirect(host, path, exe, 64u << 20) ||
        exe.empty()) {
        return L"The download failed, so nothing was changed.";
    }
    uint8_t digest[32];
    if (!Sha256(exe.data(), exe.size(), digest) ||
        HexOf(digest, 32) != info.sha256) {
        return L"The download did not match its signed manifest, so "
               L"nothing was changed.";
    }

    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) {
        return L"Could not locate the running program.";
    }
    const std::wstring cur(self);
    const std::wstring fresh = cur + L".new";
    const std::wstring old   = cur + L".old";

    const HANDLE h = CreateFileW(fresh.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return L"Could not write next to the program. Move it somewhere "
               L"writable to update.";
    }
    DWORD wrote = 0;
    const bool wok =
        WriteFile(h, exe.data(), static_cast<DWORD>(exe.size()), &wrote,
                  nullptr) != 0 &&
        wrote == exe.size();
    CloseHandle(h);
    if (!wok) {
        DeleteFileW(fresh.c_str());
        return L"Could not write the update file.";
    }

    // The rename dance: a running exe may be renamed, so the current one
    // steps aside and the verified download takes its name. Restarting
    // finishes it; failing here rolls back.
    DeleteFileW(old.c_str());
    if (!MoveFileW(cur.c_str(), old.c_str())) {
        DeleteFileW(fresh.c_str());
        return L"Could not stage the update (is the folder writable?).";
    }
    if (!MoveFileW(fresh.c_str(), cur.c_str())) {
        MoveFileW(old.c_str(), cur.c_str());   // roll back
        DeleteFileW(fresh.c_str());
        return L"Could not finish staging; the current version was "
               L"restored.";
    }
    return std::wstring();
}

// Best-effort startup cleanup of a finished update's leftover.
void CleanupOldUpdate() {
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) return;
    DeleteFileW((std::wstring(self) + L".old").c_str());
}

// ----------------------------------------------------------- the key CLI

bool GenerateUpdateKeypair(std::wstring& outText) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM,
                                    nullptr, 0) != 0) {
        return false;
    }
    bool ok = false;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptGenerateKeyPair(alg, &key, 256, 0) == 0 &&
        BCryptFinalizeKeyPair(key, 0) == 0) {
        ULONG cbPub = 0, cbPriv = 0;
        BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0,
                        &cbPub, 0);
        BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB, nullptr, 0,
                        &cbPriv, 0);
        std::vector<uint8_t> pub(cbPub), priv(cbPriv);
        if (cbPub > sizeof(BCRYPT_ECCKEY_BLOB) &&
            BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                            pub.data(), cbPub, &cbPub, 0) == 0 &&
            BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB,
                            priv.data(), cbPriv, &cbPriv, 0) == 0) {
            const std::string pubB64 =
                B64Encode(pub.data() + sizeof(BCRYPT_ECCKEY_BLOB),
                          cbPub - sizeof(BCRYPT_ECCKEY_BLOB));
            const std::string privB64 = B64Encode(priv.data(), cbPriv);
            outText = L"public  (embed in update.cpp): ";
            outText.append(pubB64.begin(), pubB64.end());
            outText += L"\r\nprivate (keep OFFLINE, this is the whole "
                       L"trust model): ";
            outText.append(privB64.begin(), privB64.end());
            outText += L"\r\n";
            ok = true;
        }
        BCryptDestroyKey(key);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// Sign a built exe: writes manifest.json and manifest.sig beside it.
bool SignReleaseFile(const std::wstring& exePath,
                     const std::wstring& privKeyB64,
                     const std::wstring& tag, std::wstring& err) {
    err.clear();
    const HANDLE h = CreateFileW(exePath.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = L"could not open the exe";
        return false;
    }
    LARGE_INTEGER size{};
    std::vector<uint8_t> exe;
    bool rok = GetFileSizeEx(h, &size) != 0 && size.QuadPart > 0 &&
               size.QuadPart < (64ll << 20);
    if (rok) {
        exe.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        rok = ReadFile(h, exe.data(), static_cast<DWORD>(exe.size()),
                       &got, nullptr) != 0 &&
              got == exe.size();
    }
    CloseHandle(h);
    if (!rok) {
        err = L"could not read the exe";
        return false;
    }

    uint8_t digest[32];
    if (!Sha256(exe.data(), exe.size(), digest)) {
        err = L"hashing failed";
        return false;
    }
    std::string tagUtf;
    for (wchar_t c : tag) tagUtf.push_back(static_cast<char>(c));
    const std::string manifest =
        std::string("{\"tag\":\"") + tagUtf +
        "\",\"asset\":\"spindle.exe\",\"sha256\":\"" +
        HexOf(digest, 32) + "\",\"size\":" +
        std::to_string(exe.size()) + "}";

    // Sign the exact manifest bytes.
    std::string privUtf;
    for (wchar_t c : privKeyB64) privUtf.push_back(static_cast<char>(c));
    std::vector<uint8_t> priv;
    if (!B64Decode(privUtf, priv)) {
        err = L"bad private key";
        return false;
    }
    uint8_t mdigest[32];
    if (!Sha256(reinterpret_cast<const uint8_t*>(manifest.data()),
                manifest.size(), mdigest)) {
        err = L"hashing failed";
        return false;
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM,
                                    nullptr, 0) != 0) {
        err = L"crypto unavailable";
        return false;
    }
    std::vector<uint8_t> sig;
    bool ok = false;
    BCRYPT_KEY_HANDLE key = nullptr;
    const NTSTATUS imp = BCryptImportKeyPair(
        alg, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key, priv.data(),
        static_cast<ULONG>(priv.size()), 0);
    if (imp == 0) {
        ULONG cb = 0;
        const NTSTATUS sz =
            BCryptSignHash(key, nullptr, mdigest, 32, nullptr, 0, &cb, 0);
        if (sz == 0) {
            sig.resize(cb);
            ok = BCryptSignHash(key, nullptr, mdigest, 32, sig.data(), cb,
                                &cb, 0) == 0;
            sig.resize(cb);
        } else {
            err = L"sign-size failed: " + std::to_wstring(sz);
        }
        BCryptDestroyKey(key);
    } else {
        err = L"key import failed: " + std::to_wstring(imp) +
              L" (blob " + std::to_wstring(priv.size()) + L" bytes)";
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) {
        if (err.empty()) err = L"signing failed";
        return false;
    }

    const size_t cut = exePath.find_last_of(L'\\');
    const std::wstring dir =
        (cut == std::wstring::npos) ? L"." : exePath.substr(0, cut);
    const std::string sigB64 = B64Encode(sig.data(), sig.size());
    std::wstring mW(manifest.begin(), manifest.end());
    std::wstring sW(sigB64.begin(), sigB64.end());
    if (!WriteTextFileUtf8(dir + L"\\manifest.json", mW) ||
        !WriteTextFileUtf8(dir + L"\\manifest.sig", sW)) {
        err = L"could not write the manifest files";
        return false;
    }
    return true;
}

}  // namespace spindle
