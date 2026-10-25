#pragma once
#include <mutex>

#if !defined(_WIN32)
#include <pthread.h>
#endif

namespace simpleplc {

/**
 * A mutex with priority inheritance where the platform has it (POSIX
 * PTHREAD_PRIO_INHERIT): a normal-priority thread holding it is raised to the
 * priority of a real-time task thread waiting for it, so the wait is bounded by
 * the holder's critical section. Elsewhere a plain mutex. Meets Lockable.
 */
class PiMutex {
public:
    PiMutex() {
#if !defined(_WIN32)
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
#if defined(_POSIX_THREAD_PRIO_INHERIT) && _POSIX_THREAD_PRIO_INHERIT > 0
        pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
#endif
        pthread_mutex_init(&m_, &attr);
        pthread_mutexattr_destroy(&attr);
#endif
    }
    ~PiMutex() {
#if !defined(_WIN32)
        pthread_mutex_destroy(&m_);
#endif
    }
    PiMutex(const PiMutex&) = delete;
    PiMutex& operator=(const PiMutex&) = delete;

#if !defined(_WIN32)
    void lock() { pthread_mutex_lock(&m_); }
    void unlock() { pthread_mutex_unlock(&m_); }
    bool try_lock() { return pthread_mutex_trylock(&m_) == 0; }

private:
    pthread_mutex_t m_;
#else
    void lock() { m_.lock(); }
    void unlock() { m_.unlock(); }
    bool try_lock() { return m_.try_lock(); }

private:
    std::mutex m_;
#endif
};

}  // namespace simpleplc
