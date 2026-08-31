// Spindle - synchronisation primitives.
//
// Win32 SRWLOCK and CONDITION_VARIABLE on Windows, pthreads elsewhere. The
// POSIX path exists so the work queue can be compiled and run under
// ThreadSanitizer on a host machine; the queue's state machine is identical
// either way, and that state machine is the part worth testing.
//
// The Win32 primitives are used directly rather than <mutex> and
// <condition_variable> because MinGW's win32 threading model backs those types
// with a comparatively young implementation, and a sixteen-thread scanner
// making hundreds of thousands of lock acquisitions is the harshest possible
// exercise of it.

#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace spindle {

class Lock {
public:
    Lock() = default;
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

    void Acquire() { AcquireSRWLockExclusive(&srw_); }
    void Release() { ReleaseSRWLockExclusive(&srw_); }
    SRWLOCK* Raw() { return &srw_; }

private:
    SRWLOCK srw_ = SRWLOCK_INIT;
};

class CondVar {
public:
    CondVar() = default;
    CondVar(const CondVar&) = delete;
    CondVar& operator=(const CondVar&) = delete;

    // Must be called with `lock` held. May wake spuriously, so every caller
    // loops on its predicate.
    void Wait(Lock& lock) {
        SleepConditionVariableSRW(&cv_, lock.Raw(), INFINITE, 0);
    }
    void WakeOne() { WakeConditionVariable(&cv_); }
    void WakeAll() { WakeAllConditionVariable(&cv_); }

private:
    CONDITION_VARIABLE cv_ = CONDITION_VARIABLE_INIT;
};

}  // namespace spindle

#else  // ---------------------------------------------------- host test build

#include <pthread.h>

namespace spindle {

class Lock {
public:
    Lock() { pthread_mutex_init(&m_, nullptr); }
    ~Lock() { pthread_mutex_destroy(&m_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

    void Acquire() { pthread_mutex_lock(&m_); }
    void Release() { pthread_mutex_unlock(&m_); }
    pthread_mutex_t* Raw() { return &m_; }

private:
    pthread_mutex_t m_{};
};

class CondVar {
public:
    CondVar() { pthread_cond_init(&c_, nullptr); }
    ~CondVar() { pthread_cond_destroy(&c_); }
    CondVar(const CondVar&) = delete;
    CondVar& operator=(const CondVar&) = delete;

    void Wait(Lock& lock) { pthread_cond_wait(&c_, lock.Raw()); }
    void WakeOne() { pthread_cond_signal(&c_); }
    void WakeAll() { pthread_cond_broadcast(&c_); }

private:
    pthread_cond_t c_{};
};

}  // namespace spindle

#endif

namespace spindle {

// Scoped lock. Named Held rather than Guard so it reads as a state at the
// call site: `Held h(lock_);`
class Held {
public:
    explicit Held(Lock& l) : l_(l) { l_.Acquire(); }
    ~Held() { l_.Release(); }
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;

private:
    Lock& l_;
};

}  // namespace spindle
