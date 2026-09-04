// Spindle - NTFS Master File Table scanner.
//
// The fast path. Instead of a syscall round trip per directory, this reads the
// volume's own index in a handful of large sequential reads and reconstructs
// the tree from it. On a volume with a million-plus files that is the
// difference between several seconds and well under one.
//
// It needs three things, and falls back to the directory walk without
// complaint if any is missing: an NTFS volume, a local disk, and elevation
// (raw volume access is an administrator privilege).
//
// This file does the I/O only. Every byte that comes off the disk is
// interpreted by src/ntfs.cpp and turned into the tree by src/mfttree.cpp,
// both of which are tested on the host, against tables built to be hostile
// and against a real image, because those are the parts that are dangerous
// and the parts worth isolating.

#include "spindle.h"
#include "mfttree.h"
#include "ntfs.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace spindle {
namespace {

// Read the MFT in chunks rather than whole: a large volume's MFT can run to
// hundreds of megabytes, and a single allocation that size is both a needless
// spike and an easy way for a hostile volume to induce one.
constexpr uint32_t kChunkBytes = 8u << 20;   // 8 MB

class Handle {
public:
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { if (h_ != INVALID_HANDLE_VALUE && h_) CloseHandle(h_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return h_; }
    bool valid() const { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }

private:
    HANDLE h_;
};

// Synchronous read on the overlapped volume handle, for the small one-off
// reads (boot sector, run list). Its own event, so it can never mistake a
// pipelined read's completion for its own. Volume reads are returned in
// whole sectors, so a short read is a genuine failure, not a retry.
bool ReadAt(HANDLE h, uint64_t offset, void* buf, uint32_t bytes) {
    const HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) return false;

    OVERLAPPED ov{};
    ov.Offset     = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    ov.hEvent     = ev;

    DWORD got = 0;
    bool ok = true;
    if (!ReadFile(h, buf, bytes, nullptr, &ov)) {
        ok = (GetLastError() == ERROR_IO_PENDING);
    }
    if (ok) ok = GetOverlappedResult(h, &ov, &got, TRUE) != 0;
    CloseHandle(ev);
    return ok && got == bytes;
}

// One in-flight overlapped read with its own buffer and event. Two of these
// alternate in the MFT loop, so the disk is filling the next chunk while the
// CPU parses this one; the read and the parse each cost real time, and there
// is no reason to pay for them in sequence.
class AsyncRead {
public:
    AsyncRead(HANDLE vol, uint32_t bufBytes)
        : vol_(vol), event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        try {
            buf_.resize(bufBytes);
        } catch (...) {
            buf_.clear();   // Ready() reports it; the caller falls back
        }
    }
    ~AsyncRead() {
        // An in-flight read writes into buf_; both must outlive it.
        Drain();
        if (event_) CloseHandle(event_);
    }
    AsyncRead(const AsyncRead&) = delete;
    AsyncRead& operator=(const AsyncRead&) = delete;

    bool Ready() const { return event_ != nullptr && !buf_.empty(); }
    uint8_t* Data() { return buf_.data(); }   // fixups patch in place

    bool Issue(uint64_t offset, uint32_t bytes) {
        if (!Ready() || inFlight_ || bytes > buf_.size()) return false;
        ResetEvent(event_);
        ov_ = OVERLAPPED{};
        ov_.Offset     = static_cast<DWORD>(offset);
        ov_.OffsetHigh = static_cast<DWORD>(offset >> 32);
        ov_.hEvent     = event_;
        want_ = bytes;
        if (!ReadFile(vol_, buf_.data(), bytes, nullptr, &ov_) &&
            GetLastError() != ERROR_IO_PENDING) {
            return false;
        }
        inFlight_ = true;
        return true;
    }

    // Wait out the issued read; true only if every requested byte arrived.
    bool Wait() {
        if (!inFlight_) return false;
        inFlight_ = false;
        DWORD got = 0;
        if (!GetOverlappedResult(vol_, &ov_, &got, TRUE)) return false;
        return got == want_;
    }

private:
    void Drain() {
        if (!inFlight_) return;
        CancelIoEx(vol_, &ov_);
        DWORD got = 0;
        GetOverlappedResult(vol_, &ov_, &got, TRUE);
        inFlight_ = false;
    }

    HANDLE               vol_;
    HANDLE               event_;
    std::vector<uint8_t> buf_;
    OVERLAPPED           ov_{};
    uint32_t             want_ = 0;
    bool                 inFlight_ = false;
};

bool IsElevated() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
    Handle t(raw);

    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    if (!GetTokenInformation(t.get(), TokenElevation, &elevation,
                             sizeof(elevation), &size)) {
        return false;
    }
    return elevation.TokenIsElevated != 0;
}

// Walk the $MFT record itself to find where the rest of the table lives.
bool ReadMftRunList(HANDLE vol, const ntfs::BootInfo& bi,
                    std::vector<ntfs::DataRun>& runs, uint64_t& mftBytes) {
    std::vector<uint8_t> rec(bi.bytesPerRecord);
    const uint64_t mftOffset = bi.mftStartCluster * bi.bytesPerCluster;

    if (!ReadAt(vol, mftOffset, rec.data(), bi.bytesPerRecord)) return false;
    if (!ntfs::ApplyFixups(rec.data(), rec.size(), bi.bytesPerSector)) {
        return false;
    }
    return ntfs::ParseMftDataRuns(rec.data(), rec.size(), runs, mftBytes);
}

}  // namespace

// Attempts an MFT scan. Returns false -- having changed nothing -- whenever
// the fast path is unavailable, so the caller falls back to the directory
// walk. There is deliberately no error surfaced to the user for this: the
// slower path produces the same answer.
bool ScanMft(const std::wstring& root, Progress* progress, ScanResult& out) {
    if (root.size() < 2 || root[1] != L':') return false;
    if (!IsElevated()) return false;

    // MFT reading only makes sense for a local NTFS volume.
    {
        wchar_t rootPath[4] = {root[0], L':', L'\\', 0};
        if (GetDriveTypeW(rootPath) != DRIVE_FIXED) return false;

        wchar_t fs[16] = {};
        if (!GetVolumeInformationW(rootPath, nullptr, 0, nullptr, nullptr,
                                   nullptr, fs, 15)) {
            return false;
        }
        if (wcscmp(fs, L"NTFS") != 0) return false;
    }

    const std::wstring volPath = std::wstring(L"\\\\.\\") + root[0] + L':';

    // FILE_READ_DATA only, and sharing everything: this must never interfere
    // with a volume that is in active use.
    // Overlapped, so the record loop below can keep one chunk read in
    // flight while it parses the previous one.
    Handle vol(CreateFileW(volPath.c_str(), FILE_READ_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                           nullptr));
    if (!vol.valid()) return false;

    std::vector<uint8_t> boot(512);
    if (!ReadAt(vol.get(), 0, boot.data(), 512)) return false;

    const ntfs::BootInfo bi = ntfs::ParseBootSector(boot.data(), boot.size());
    if (!bi.valid) return false;

    std::vector<ntfs::DataRun> runs;
    uint64_t mftBytes = 0;
    if (!ReadMftRunList(vol.get(), bi, runs, mftBytes) || runs.empty()) {
        return false;
    }

    const uint64_t recordCount = mftBytes / bi.bytesPerRecord;
    mft::Assembler assembler;
    if (!assembler.Begin(recordCount)) return false;

    // ---- read the table, handing every chunk to the assembler ------------
    //
    // The read plan comes first: every non-sparse run split into
    // record-aligned chunks, each knowing which record number it starts at.
    // Laying the plan out up front is what lets the loop keep the next
    // chunk's read in flight while this one is parsed - the disk and the
    // CPU work at the same time instead of taking turns.

    struct Segment {
        uint64_t offset;
        uint32_t bytes;
        uint64_t firstRecord;
    };
    std::vector<Segment> segments;
    {
        uint64_t rec = 0;
        for (const ntfs::DataRun& run : runs) {
            if (run.sparse) {
                // Unallocated stretch of the MFT: skip the record numbers
                // it covers rather than misaligning everything after it.
                rec += SatMul(run.clusters, bi.bytesPerCluster) /
                       bi.bytesPerRecord;
                if (rec >= recordCount) break;
                continue;
            }
            // Both fields come off the disk and are bounded only by
            // their width, so the products wrap without this. A wrapped
            // plan reads the right volume at the wrong offsets and files
            // records under the wrong numbers: a silently wrong tree.
            if (bi.bytesPerCluster != 0 &&
                (run.clusters > UINT64_MAX / bi.bytesPerCluster ||
                 static_cast<uint64_t>(run.lcn) >
                     UINT64_MAX / bi.bytesPerCluster)) {
                continue;   // implausible run: skip it, keep the rest
            }
            uint64_t remaining = run.clusters * bi.bytesPerCluster;
            uint64_t off = static_cast<uint64_t>(run.lcn) * bi.bytesPerCluster;
            while (remaining > 0 && rec < recordCount) {
                const uint32_t want = static_cast<uint32_t>(
                    std::min<uint64_t>(remaining, kChunkBytes));
                const uint32_t aligned = want - (want % bi.bytesPerRecord);
                if (aligned == 0) break;
                segments.push_back(Segment{off, aligned, rec});
                off += aligned;
                remaining -= aligned;
                rec += aligned / bi.bytesPerRecord;
            }
        }
    }

    // Declared after `vol` on purpose: destruction runs in reverse, so an
    // in-flight read is drained before the handle it targets closes.
    AsyncRead ioA(vol.get(), kChunkBytes);
    AsyncRead ioB(vol.get(), kChunkBytes);
    AsyncRead* io[2] = {&ioA, &ioB};
    if (!ioA.Ready() || !ioB.Ready()) return false;

    size_t issued = 0;   // segments handed to the disk so far

    // The window can only report what it is told: the table is read whole
    // before any file is counted, so say so, with the bytes as they land.
    if (progress) {
        progress->tableBytes.store(0, std::memory_order_relaxed);
        progress->phase.store(1, std::memory_order_relaxed);
    }

    for (size_t s = 0; s < segments.size(); ++s) {
        if (progress && progress->cancel.load(std::memory_order_relaxed)) {
            return false;   // AsyncRead destructors drain in-flight I/O
        }

        // Normally segment s went out an iteration ago; the first segment,
        // or any whose Issue failed, is picked up synchronously here.
        if (issued == s) {
            if (io[s & 1]->Issue(segments[s].offset, segments[s].bytes)) {
                ++issued;
            }
        }

        AsyncRead& cur = *io[s & 1];
        const bool ok = (issued > s) && cur.Wait();

        // Fill the other buffer before touching this one.
        if (issued == s + 1 && issued < segments.size()) {
            if (io[issued & 1]->Issue(segments[issued].offset,
                                      segments[issued].bytes)) {
                ++issued;
            }
        }

        const Segment& seg = segments[s];
        if (!ok) {
            // A bad sector mid-table is not fatal: skip the chunk and keep
            // going. The result is incomplete, not wrong.
            continue;
        }
        if (progress) {
            progress->tableBytes.fetch_add(seg.bytes,
                                           std::memory_order_relaxed);
        }

        assembler.FeedChunk(seg.firstRecord, cur.Data(), seg.bytes,
                            bi.bytesPerRecord, bi.bytesPerSector);

        if (progress) {
            progress->files.store(assembler.Files(), std::memory_order_relaxed);
            progress->dirs.store(assembler.Dirs(), std::memory_order_relaxed);
            progress->bytes.store(assembler.Bytes(), std::memory_order_relaxed);
            progress->phase.store(2, std::memory_order_relaxed);
        }
    }

    // Checked here, after the reads have drained, rather than inside the
    // loop where a read may still be in flight.
    if (!assembler.Finish(root, out, progress ? &progress->cancel : nullptr)) {
        return false;
    }
    if (progress) progress->phase.store(0, std::memory_order_relaxed);
    return true;
}

}  // namespace spindle
