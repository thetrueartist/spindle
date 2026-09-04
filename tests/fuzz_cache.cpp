// Fuzz target: the scan cache. A cache file is input, not state: parsed
// with the same posture as the NTFS code, and a tree it accepts must
// survive a round trip through the serialiser.
#include "fuzz_common.h"

using namespace spindle;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    const uint8_t which = data[0] % 3;
    const uint8_t* d = data + 1;
    const size_t n = size - 1;
    switch (which) {
        case 0: {
            ScanResult out;
            CacheMeta meta;
            if (DeserializeScan(d, n, out, meta, nullptr)) {
                RollUp(out.root);
                std::vector<uint8_t> again;
                SerializeScan(out, meta, again);
                ScanResult back;
                CacheMeta metaBack;
                FUZZ_REQUIRE(DeserializeScan(again.data(), again.size(), back,
                                             metaBack, nullptr));
                FUZZ_REQUIRE(back.root.name == out.root.name);
                FUZZ_REQUIRE(metaBack.volumeSerial == meta.volumeSerial);
            }
            break;
        }
        case 1: {
            size_t off = 0;
            if (SealedCachePayload(d, n, off)) FUZZ_REQUIRE(off < n);
            break;
        }
        case 2: {
            SealedFrame f;
            if (SealedCacheFrame(d, n, f)) {
                FUZZ_REQUIRE(f.keyLen > 0 && f.keyLen <= kCacheSealKeyMax);
                FUZZ_REQUIRE(f.keyOff + f.keyLen <= n);
                FUZZ_REQUIRE(f.nonceOff + kCacheSealNonce <= n);
                FUZZ_REQUIRE(f.tagOff + kCacheSealTag <= n);
                FUZZ_REQUIRE(f.cipherOff <= n);
            }
            break;
        }
    }
    return 0;
}

void FuzzSeeds(std::vector<std::vector<uint8_t>>& out) {
    auto add = [&](uint8_t which, const std::vector<uint8_t>& body) {
        std::vector<uint8_t> s{which};
        s.insert(s.end(), body.begin(), body.end());
        out.push_back(s);
    };
    {
        ScanResult res;
        res.root = Node(L"D:\\", true);
        res.root.cat = Cat::Directory;
        Node games(L"Games", true);
        games.cat = Cat::Directory;
        Node pak(L"pakchunk0.pak", false);
        pak.size = 1500000000ull;
        pak.files = 1;
        pak.cat = CategoryForFile(pak.name);
        games.children.push_back(std::move(pak));
        Node note(L"notes.md", false);
        note.size = 5000;
        note.files = 1;
        note.cat = CategoryForFile(note.name);
        res.root.children.push_back(std::move(games));
        res.root.children.push_back(std::move(note));
        RollUp(res.root);
        res.stats.fileCount = 2;
        res.stats.dirCount = 1;
        res.stats.bytes = res.root.size;
        CacheMeta meta;
        meta.savedUnixMs = 1756742400000ull;
        meta.volumeSerial = 0x1234ABCD;
        std::vector<uint8_t> bytes;
        SerializeScan(res, meta, bytes);
        add(0, bytes);
    }
    {
        // A version 2 frame: magic, version, a 300-byte key, nonce, tag, body.
        std::vector<uint8_t> f;
        auto put32 = [&](uint32_t v) {
            for (int i = 0; i < 4; ++i) f.push_back(static_cast<uint8_t>(v >> (8 * i)));
        };
        put32(kCacheSealMagic);
        put32(kCacheSealVersion);
        put32(300);
        f.resize(f.size() + 300 + kCacheSealNonce + kCacheSealTag, 0x5A);
        f.resize(f.size() + 64, 0xC3);
        add(1, f);
        add(2, f);
        std::vector<uint8_t> legacy;
        f.swap(legacy);
        put32(kCacheSealMagic);
        put32(kCacheSealVersionLegacy);
        f.resize(f.size() + 40, 0x11);
        add(1, f);
    }
}
