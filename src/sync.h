// Spindle - synchronisation primitives.
//
// Win32 SRWLOCK and CONDITION_VARIABLE on Windows, pthreads everywhere else.
// This exists because of a crash, not for portability's own sake: MinGW's
// win32 threading model backs std::mutex and std::condition_variable with an
// implementation that fell over under a sixteen-thread scanner sustained for
// seconds. SRWLOCK and CONDITION_VARIABLE are what the OS itself is built on,
// and using them directly also makes the binary identical under both MinGW
// threading models. The pthreads side is what lets the identical work queue
// run under ThreadSanitizer on the host.
//
// Nothing here allocates and nothing here can fail on the wait path, which is
// exactly the property the standard types could not offer.

#ifndef SPINDLE_SYNC_H_
#define SPINDLE_SYNC_H_

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace spindle {

class Mutex {
public:
    Mutex() = default;
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void Lock() { AcquireSRWLockExclusive(&lock_); }
    void Unlock() { ReleaseSRWLockExclusive(&lock_); }

    SRWLOCK* Native() { return &lock_; }

private:
    SRWLOCK lock_ = SRWLOCK_INIT;
};

class CondVar {
public:
    CondVar() = default;
    CondVar(const CondVar&) = delete;
    CondVar& operator=(const CondVar&) = delete;

    // Atomically releases the mutex and sleeps; the mutex is held again on
    // return. Spurious wakes are possible, so every caller waits in a loop
    // over its own predicate.
    void Wait(Mutex& m) {
        SleepConditionVariableSRW(&cv_, m.Native(), INFINITE, 0);
    }

    void NotifyOne() { WakeConditionVariable(&cv_); }
    void NotifyAll() { WakeAllConditionVariable(&cv_); }

private:
    CONDITION_VARIABLE cv_ = CONDITION_VARIABLE_INIT;
};

}  // namespace spindle

#else  // POSIX host, for the tests and the stress walker

#include <pthread.h>

namespace spindle {

class Mutex {
public:
    Mutex() = default;
    ~Mutex() { pthread_mutex_destroy(&lock_); }
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void Lock() { pthread_mutex_lock(&lock_); }
    void Unlock() { pthread_mutex_unlock(&lock_); }

    pthread_mutex_t* Native() { return &lock_; }

private:
    pthread_mutex_t lock_ = PTHREAD_MUTEX_INITIALIZER;
};

class CondVar {
public:
    CondVar() = default;
    ~CondVar() { pthread_cond_destroy(&cv_); }
    CondVar(const CondVar&) = delete;
    CondVar& operator=(const CondVar&) = delete;

    void Wait(Mutex& m) { pthread_cond_wait(&cv_, m.Native()); }

    void NotifyOne() { pthread_cond_signal(&cv_); }
    void NotifyAll() { pthread_cond_broadcast(&cv_); }

private:
    pthread_cond_t cv_ = PTHREAD_COND_INITIALIZER;
};

}  // namespace spindle

#endif  // _WIN32

namespace spindle {

class LockGuard {
public:
    explicit LockGuard(Mutex& m) : m_(m) { m_.Lock(); }
    ~LockGuard() { m_.Unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Mutex& m_;
};

}  // namespace spindle

#endif  // SPINDLE_SYNC_H_
