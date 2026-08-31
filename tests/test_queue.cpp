// Concurrency tests for WorkQueue -- the exact code the scanner runs, built
// here against the POSIX branch of sync.h so ThreadSanitizer can see it.
//
// The scanner's failure mode was in this layer, so these focus on the state
// machine rather than the filesystem: termination, cancellation, and the
// self-feeding case where processing an item enqueues more.

#include "../src/workqueue.h"

#include <atomic>
#include <cstdio>
#include <pthread.h>
#include <unistd.h>
#include <vector>

using namespace spindle;

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { ++g_pass; }                                             \
        else { ++g_fail;                                                    \
               std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); }\
    } while (0)

namespace {

struct Item {
    int depth = 0;
    int id = 0;
};

struct Ctx {
    WorkQueue<Item>       q;
    std::atomic<long>     processed{0};
    std::atomic<long>     popped{0};
    std::atomic<bool>     cancel{false};
    std::atomic<int>      liveWorkers{0};
    int                   fanout = 3;
    int                   maxDepth = 6;
    bool                  cancelMode = false;
};

// Mirrors the scanner's worker: pop, maybe bail on cancel, process (which
// enqueues more), then Done exactly once per successful Pop.
void* Worker(void* p) {
    auto* c = static_cast<Ctx*>(p);
    c->liveWorkers.fetch_add(1);

    Item item;
    while (c->q.Pop(item)) {
        c->popped.fetch_add(1);

        if (c->cancelMode && c->cancel.load()) {
            c->q.Done();
            c->q.Stop();
            break;
        }

        if (item.depth < c->maxDepth) {
            std::vector<Item> batch;
            batch.reserve(static_cast<size_t>(c->fanout));
            for (int i = 0; i < c->fanout; ++i) {
                batch.push_back(Item{item.depth + 1, i});
            }
            c->q.PushBatch(batch);
        }
        c->processed.fetch_add(1);
        c->q.Done();
    }
    c->liveWorkers.fetch_sub(1);
    return nullptr;
}

long RunPool(Ctx& c, int threads) {
    std::vector<pthread_t> ids(static_cast<size_t>(threads));
    for (int i = 0; i < threads; ++i) {
        pthread_create(&ids[static_cast<size_t>(i)], nullptr, Worker, &c);
    }
    for (pthread_t t : ids) pthread_join(t, nullptr);
    return c.processed.load();
}

}  // namespace

// Every enqueued item must be processed exactly once and every worker must
// exit. A hang here is the classic termination bug: workers sleeping while
// work is still in flight, or exiting while the queue still has items.
static void TestDrainsCompletely() {
    std::printf("Queue drains and terminates\n");

    // 3-way fanout to depth 6 = 1 + 3 + 9 + ... + 729 = 1093 items.
    long expected = 0;
    long level = 1;
    for (int d = 0; d <= 6; ++d) { expected += level; level *= 3; }

    for (int threads : {1, 2, 4, 8, 16, 32}) {
        Ctx c;
        c.fanout = 3;
        c.maxDepth = 6;
        c.q.Push(Item{0, 0});

        const long done = RunPool(c, threads);
        std::printf("    %2d threads: %ld processed (expected %ld), "
                    "%zu left queued, busy=%d, live=%d\n",
                    threads, done, expected, c.q.SizeForTest(),
                    c.q.BusyForTest(), c.liveWorkers.load());

        CHECK(done == expected, "every item processed exactly once");
        CHECK(c.q.SizeForTest() == 0, "queue fully drained");
        CHECK(c.q.BusyForTest() == 0, "busy count returned to zero");
        CHECK(c.liveWorkers.load() == 0, "all workers exited");
    }
}

// The scanner starts every thread before any work exists beyond a single root
// item, so most workers begin by waiting. They must not exit early.
static void TestNoEarlyExit() {
    std::printf("Workers do not exit before work arrives\n");

    for (int trial = 0; trial < 40; ++trial) {
        Ctx c;
        c.fanout = 4;
        c.maxDepth = 4;
        c.q.Push(Item{0, 0});   // one item, 32 threads racing for it

        long expected = 0, level = 1;
        for (int d = 0; d <= 4; ++d) { expected += level; level *= 4; }

        const long done = RunPool(c, 32);
        if (done != expected) {
            std::printf("    trial %d: %ld processed, expected %ld\n",
                        trial, done, expected);
        }
        CHECK(done == expected, "no worker exited before the tree expanded");
    }
    std::printf("    40 trials at 32 threads on a single seed item\n");
}

// Cancellation must stop promptly and leave the queue consistent, with Done
// still balanced against Pop.
static void TestCancellation() {
    std::printf("Cancellation is clean\n");

    for (int trial = 0; trial < 60; ++trial) {
        Ctx c;
        c.fanout = 3;
        c.maxDepth = 9;         // large enough to still be running
        c.cancelMode = true;
        c.q.Push(Item{0, 0});

        std::vector<pthread_t> ids(16);
        for (auto& t : ids) pthread_create(&t, nullptr, Worker, &c);

        usleep(200 + static_cast<useconds_t>(trial % 11) * 60);
        c.cancel.store(true);

        for (pthread_t t : ids) pthread_join(t, nullptr);

        CHECK(c.q.BusyForTest() == 0, "busy count balanced after cancel");
        CHECK(c.liveWorkers.load() == 0, "all workers exited after cancel");
    }
    std::printf("    60 trials, cancelled mid-flight at 16 threads\n");
}

// Stop before any worker starts: nothing should be handed out, and nothing
// should block.
static void TestStopBeforeStart() {
    std::printf("Stop before workers start\n");

    Ctx c;
    c.maxDepth = 0;
    for (int i = 0; i < 100; ++i) c.q.Push(Item{0, i});
    c.q.Stop();

    const long done = RunPool(c, 8);
    std::printf("    %ld processed after pre-emptive stop\n", done);
    CHECK(done == 0, "a stopped queue hands out nothing");
    CHECK(c.liveWorkers.load() == 0, "workers exited immediately");
}

// An empty queue with no workers busy must return immediately rather than
// blocking forever.
static void TestEmptyQueueReturns() {
    std::printf("Empty queue returns immediately\n");

    Ctx c;
    c.maxDepth = 0;
    const long done = RunPool(c, 8);
    CHECK(done == 0, "nothing processed");
    CHECK(c.liveWorkers.load() == 0, "workers exited rather than hanging");
    std::printf("    8 workers exited on an empty queue\n");
}

// Heavy contention: many threads, tiny units of work, repeated. This is the
// shape that was killing the win32 std::condition_variable build.
static void TestHeavyContention() {
    std::printf("Heavy contention\n");

    long total = 0;
    for (int round = 0; round < 12; ++round) {
        Ctx c;
        c.fanout = 2;
        c.maxDepth = 11;        // 4095 items per round
        c.q.Push(Item{0, 0});
        total += RunPool(c, 24);
        CHECK(c.q.BusyForTest() == 0, "busy balanced under contention");
        CHECK(c.q.SizeForTest() == 0, "queue drained under contention");
    }
    std::printf("    %ld items across 12 rounds at 24 threads\n", total);
    CHECK(total == 4095L * 12, "exact item count across all rounds");
}

int main() {
    std::printf("\n=== Spindle work queue concurrency tests ===\n\n");

    TestEmptyQueueReturns();
    TestStopBeforeStart();
    TestDrainsCompletely();
    TestNoEarlyExit();
    TestCancellation();
    TestHeavyContention();

    std::printf("\n=== %d passed, %d failed ===\n\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
