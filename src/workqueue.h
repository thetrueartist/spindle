// Spindle - work queue shared by the scanner's threads.
//
// Extracted into a header so the state machine can be exercised on a host
// machine under ThreadSanitizer. This is the code the scanner was crashing
// in, so it is the code that most needs to be testable.

#pragma once

#include "sync.h"

#include <utility>
#include <vector>

namespace spindle {

// A queue that also tracks how many items are being worked on, so it can tell
// the difference between "empty for now, work still in flight" and "finished".
//
// Termination: Pop blocks while the queue is empty and any worker is still
// busy, because a busy worker may yet push more. Once the queue is empty and
// nothing is in flight, Pop returns false and every worker exits.
template <typename T>
class WorkQueue {
public:
    void Push(T item) {
        {
            Held h(lock_);
            items_.push_back(std::move(item));
        }
        cv_.WakeOne();
    }

    // Pushes under a single lock acquisition. A scanned directory typically
    // yields several subdirectories at once, and taking the lock once per
    // batch rather than once per item measurably reduces contention with
    // sixteen threads running.
    void PushBatch(std::vector<T>& batch) {
        if (batch.empty()) return;
        {
            Held h(lock_);
            for (T& item : batch) items_.push_back(std::move(item));
        }
        cv_.WakeAll();
        batch.clear();
    }

    // Returns false when the queue is drained and nothing is in flight, or
    // once Stop has been called.
    bool Pop(T& out) {
        Held h(lock_);
        for (;;) {
            // Checked before the queue: after Stop, pending items are not
            // handed out. Anything already in flight still completes.
            if (stop_) return false;

            if (!items_.empty()) {
                out = std::move(items_.back());
                items_.pop_back();
                ++busy_;
                return true;
            }
            if (busy_ == 0) return false;

            // Spurious wakeups are permitted, hence the surrounding loop.
            cv_.Wait(lock_);
        }
    }

    // Exactly one call per successful Pop.
    void Done() {
        bool wake = false;
        {
            Held h(lock_);
            if (busy_ > 0) --busy_;
            wake = (busy_ == 0 && items_.empty());
        }
        // Waking outside the lock: waiters would otherwise wake straight into
        // contention for a lock this thread still holds.
        if (wake) cv_.WakeAll();
    }

    void Stop() {
        {
            Held h(lock_);
            stop_ = true;
            items_.clear();
        }
        cv_.WakeAll();
    }

    size_t SizeForTest() {
        Held h(lock_);
        return items_.size();
    }

    int BusyForTest() {
        Held h(lock_);
        return busy_;
    }

private:
    Lock           lock_;
    CondVar        cv_;
    std::vector<T> items_;
    int            busy_ = 0;
    bool           stop_ = false;
};

}  // namespace spindle
