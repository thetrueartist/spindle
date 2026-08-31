// Stress harness: mirrors src/scan.cpp's concurrency structure exactly --
// same Queue, same reserve-then-queue pointer-stability trick, same worker
// loop -- but walks a POSIX tree so it can run under ThreadSanitizer here.
//
// If the Windows scanner has a race or a lifetime bug, this has it too.

#include "../src/spindle.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace spindle;

// core.cpp's CSV writer defers the actual file write to the platform. The
// stress harness never exports CSV; this exists only so core.cpp links here.
namespace spindle {
bool WriteTextFileUtf8(const std::wstring&, const std::wstring&) {
    return false;
}
}  // namespace spindle

namespace {

constexpr int kMaxDepth = 512;

std::wstring Widen(const char* s) {
    std::wstring w;
    while (*s) w.push_back(static_cast<wchar_t>(*s++));
    return w;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(static_cast<char>(c));
    return s;
}

std::string JoinPath(const std::string& dir, const std::string& leaf) {
    if (dir.empty()) return leaf;
    if (dir.back() == '/') return dir + leaf;
    return dir + '/' + leaf;
}

struct Task {
    Node*       node = nullptr;
    std::string path;
    int         depth = 0;
};

// ---- verbatim copy of the Queue from src/scan.cpp ----
class Queue {
public:
    void Push(Task t) {
        {
            std::lock_guard<std::mutex> lk(m_);
            q_.push_back(std::move(t));
        }
        cv_.notify_one();
    }

    void PushBatch(std::vector<Task>& batch) {
        if (batch.empty()) return;
        {
            std::lock_guard<std::mutex> lk(m_);
            for (Task& t : batch) q_.push_back(std::move(t));
        }
        cv_.notify_all();
        batch.clear();
    }

    bool Pop(Task& out) {
        std::unique_lock<std::mutex> lk(m_);
        for (;;) {
            if (!q_.empty()) {
                out = std::move(q_.back());
                q_.pop_back();
                ++busy_;
                return true;
            }
            if (busy_ == 0 || stop_) return false;
            cv_.wait(lk);
        }
    }

    void Done() {
        bool wake = false;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (busy_ > 0) --busy_;
            wake = (busy_ == 0 && q_.empty());
        }
        if (wake) cv_.notify_all();
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
            q_.clear();
        }
        cv_.notify_all();
    }

private:
    std::mutex              m_;
    std::condition_variable cv_;
    std::vector<Task>       q_;
    int                     busy_ = 0;
    bool                    stop_ = false;
};

struct Shared {
    Queue                 queue;
    Progress*             progress = nullptr;
    std::mutex            deniedMutex;
    std::vector<std::wstring> denied;
    std::atomic<uint64_t> deniedCount{0};
    std::atomic<uint64_t> dirCount{0};
    std::atomic<uint64_t> fileCount{0};
    std::atomic<uint64_t> byteCount{0};
};

void ScanOne(const Task& task, Shared& sh) {
    DIR* d = opendir(task.path.c_str());
    if (!d) {
        sh.deniedCount.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(sh.deniedMutex);
        if (sh.denied.size() < kMaxDeniedRecorded) {
            sh.denied.push_back(Widen(task.path.c_str()));
        }
        return;
    }

    struct Entry {
        std::string name;
        uint64_t    size = 0;
        bool        dir  = false;
    };
    std::vector<Entry> entries;
    entries.reserve(64);

    uint64_t localFiles = 0, localBytes = 0;

    while (struct dirent* de = readdir(d)) {
        if (!std::strcmp(de->d_name, ".") || !std::strcmp(de->d_name, "..")) {
            continue;
        }
        const std::string full = JoinPath(task.path, de->d_name);

        struct stat st{};
        if (lstat(full.c_str(), &st) != 0) continue;
        if (S_ISLNK(st.st_mode)) continue;          // mirrors reparse skip

        Entry e;
        e.name = de->d_name;
        e.dir  = S_ISDIR(st.st_mode);
        if (!e.dir) {
            e.size = static_cast<uint64_t>(st.st_size);
            localBytes = SatAdd(localBytes, e.size);
            ++localFiles;
        }
        entries.push_back(std::move(e));
    }
    closedir(d);

    if (entries.empty()) return;

    Node& parent = *task.node;
    parent.children.reserve(entries.size());

    std::vector<Task> subdirs;
    subdirs.reserve(8);

    for (Entry& e : entries) {
        Node child(Widen(e.name.c_str()), e.dir);
        if (e.dir) {
            child.cat = Cat::Directory;
        } else {
            child.size  = e.size;
            child.files = 1;
            child.cat   = CategoryForFile(child.name);
        }
        parent.children.push_back(std::move(child));
    }

    if (task.depth < kMaxDepth) {
        for (Node& c : parent.children) {
            if (!c.dir) continue;
            subdirs.push_back(
                Task{&c, JoinPath(task.path, Narrow(c.name)), task.depth + 1});
        }
    }

    sh.fileCount.fetch_add(localFiles, std::memory_order_relaxed);
    sh.byteCount.fetch_add(localBytes, std::memory_order_relaxed);
    sh.dirCount.fetch_add(1, std::memory_order_relaxed);

    if (sh.progress) {
        sh.progress->files.store(sh.fileCount.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
        sh.progress->dirs.store(sh.dirCount.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
        sh.progress->bytes.store(sh.byteCount.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
    }

    sh.queue.PushBatch(subdirs);
}

void Worker(Shared* sh) {
    Task t;
    while (sh->queue.Pop(t)) {
        if (sh->progress &&
            sh->progress->cancel.load(std::memory_order_relaxed)) {
            sh->queue.Done();
            sh->queue.Stop();
            return;
        }
        ScanOne(t, *sh);
        sh->queue.Done();
    }
}

void RollUp(Node& root) {
    struct Frame { Node* node; size_t next; };
    std::vector<Frame> stack;
    stack.push_back(Frame{&root, 0});

    while (!stack.empty()) {
        Frame& f = stack.back();
        if (f.next < f.node->children.size()) {
            Node* child = &f.node->children[f.next];
            ++f.next;
            if (child->dir) stack.push_back(Frame{child, 0});
            continue;
        }
        Node* n = f.node;
        if (n->dir) {
            uint64_t total = 0;
            uint32_t files = 0;
            for (const Node& c : n->children) {
                total = SatAdd(total, c.size);
                files += c.files;
            }
            n->size = total;
            n->files = files;
        }
        stack.pop_back();
    }
}

void SortTree(Node& root) {
    struct Frame { Node* node; size_t next; };
    std::vector<Frame> stack;
    stack.push_back(Frame{&root, 0});

    while (!stack.empty()) {
        Frame& f = stack.back();
        if (f.next == 0) {
            std::sort(f.node->children.begin(), f.node->children.end(),
                      [](const Node& a, const Node& b) {
                          return a.size > b.size;
                      });
        }
        if (f.next < f.node->children.size()) {
            Node* child = &f.node->children[f.next];
            ++f.next;
            if (child->dir && !child->children.empty()) {
                stack.push_back(Frame{child, 0});
            }
            continue;
        }
        stack.pop_back();
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string root = (argc > 1) ? argv[1] : "/usr";
    const unsigned threads = (argc > 2)
        ? static_cast<unsigned>(std::atoi(argv[2]))
        : std::thread::hardware_concurrency();
    const int iterations = (argc > 3) ? std::atoi(argv[3]) : 1;
    const bool cancelTest = (argc > 4);

    for (int it = 0; it < iterations; ++it) {
        Progress progress;
        Node treeRoot(Widen(root.c_str()), true);
        treeRoot.cat = Cat::Directory;

        Shared sh;
        sh.progress = &progress;
        sh.queue.Push(Task{&treeRoot, root, 0});

        std::vector<std::thread> pool;
        pool.reserve(threads);
        for (unsigned i = 0; i < threads; ++i) pool.emplace_back(Worker, &sh);

        // Cancel mid-flight to exercise the shutdown path the UI uses when
        // the user clicks another drive during a scan.
        if (cancelTest) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2 + it % 7));
            progress.cancel.store(true, std::memory_order_relaxed);
        }

        for (std::thread& t : pool) t.join();

        RollUp(treeRoot);
        SortTree(treeRoot);

        std::vector<Cell> cells;
        BuildTreemap(treeRoot, Rect{0, 0, 1600, 900}, 5, 34.0f, cells);

        const std::wstring w = FormatSize(treeRoot.size);
        std::printf("iter %2d: %llu files, %llu dirs, %s, %zu cells%s\n", it,
                    static_cast<unsigned long long>(sh.fileCount.load()),
                    static_cast<unsigned long long>(sh.dirCount.load()),
                    Narrow(w).c_str(), cells.size(),
                    cancelTest ? " (cancelled)" : "");
    }
    return 0;
}
