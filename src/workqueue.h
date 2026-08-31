// Spindle - the scanner's work queue.
//
// This is where the original crash lived, so it no longer lives inside
// scan.cpp as untestable Windows-only code: the queue is header-only over the
// primitives in sync.h, the Windows build compiles it against SRWLOCK, and
// the tests compile the identical code against pthreads and run it under
// ThreadSanitizer and AddressSanitizer.
//
// The shape it serves: workers pop a directory, enumerate it, push the
// subdirectories they find, and only then mark the popped unit finished. So
// "queue empty" does not mean "work finished" - a worker holding the last
// item may be about to push twenty more. The queue therefore counts units
// from Push until Done, and a Pop only reports completion when the queue is
// empty AND nothing popped is still being processed.

#ifndef SPINDLE_WORKQUEUE_H_
#define SPINDLE_WORKQUEUE_H_

#include <cstddef>
#include <deque>
#include <utility>

#include "sync.h"

namespace spindle {

template <typename T>
class WorkQueue {
public:
    WorkQueue() = default;
    WorkQueue(const WorkQueue&) = delete;
    WorkQueue& operator=(const WorkQueue&) = delete;

    // Enqueues one unit of work. Callable from inside a worker (that is the
    // whole point) or from outside before the workers start.
    void Push(T item) {
        {
            LockGuard g(lock_);
            items_.push_back(std::move(item));
            ++pending_;
        }
        cv_.NotifyOne();
    }

    // Blocks until an item is available, all work has finished, or the queue
    // is cancelled. Returns false only for the latter two, and every worker
    // sees the same false: there is no state in which one thread is handed an
    // item after another has been told the queue is finished, other than the
    // hand-off Done() below makes explicit.
    bool Pop(T& out) {
        LockGuard g(lock_);
        for (;;) {
            if (cancelled_) return false;
            if (!items_.empty()) {
                out = std::move(items_.front());
                items_.pop_front();
                return true;
            }
            if (pending_ == 0) return false;
            cv_.Wait(lock_);
        }
    }

    // The unit obtained from Pop is fully processed, including any Pushes it
    // performed. When the last unit finishes, every sleeping worker is woken
    // so it can observe completion and leave. pending_ counting queued items
    // too means pending_ == 0 already implies an empty queue.
    void Done() {
        bool finished = false;
        {
            LockGuard g(lock_);
            if (pending_ > 0) --pending_;
            finished = (pending_ == 0);
        }
        if (finished) cv_.NotifyAll();
    }

    // Abandons everything: current items are discarded and every Pop, present
    // and future, returns false. Safe to call from any thread at any time,
    // including before the first Push.
    void Cancel() {
        {
            LockGuard g(lock_);
            cancelled_ = true;
            items_.clear();
        }
        cv_.NotifyAll();
    }

    bool Cancelled() {
        LockGuard g(lock_);
        return cancelled_;
    }

private:
    Mutex         lock_;
    CondVar       cv_;
    std::deque<T> items_;
    // Units pushed but not yet Done()'d. This is what distinguishes "nothing
    // queued right now" from "nothing left to do".
    size_t        pending_   = 0;
    bool          cancelled_ = false;
};

}  // namespace spindle

#endif  // SPINDLE_WORKQUEUE_H_
