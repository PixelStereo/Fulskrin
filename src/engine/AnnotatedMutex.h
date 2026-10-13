#pragma once
// Qt's mutexes are invisible to ThreadSanitizer: Qt is not built with it, and its mutexes wait on futexes, not on
// pthread ones. Every access they order then looks like a race. Wrapped in Annotated, a mutex tells ThreadSanitizer
// what it orders; outside a ThreadSanitizer build the annotations are not compiled (no cost).

#if defined(__SANITIZE_THREAD__)
#define PINOL_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define PINOL_TSAN 1
#endif
#endif

#ifdef PINOL_TSAN
extern "C" void __tsan_acquire(void *addr);
extern "C" void __tsan_release(void *addr);
#endif

// An ordering ThreadSanitizer cannot see by itself (a QThread joined: QThread::wait waits on a futex): what one thread
// did before tsanRelease(x) happens before what another does after tsanAcquire(x)
inline void tsanRelease([[maybe_unused]] void *x)
{
#ifdef PINOL_TSAN
    __tsan_release(x);
#endif
}
inline void tsanAcquire([[maybe_unused]] void *x)
{
#ifdef PINOL_TSAN
    __tsan_acquire(x);
#endif
}

template <typename Mutex>
class Annotated
{
public:
    void lock()
    {
        m_mutex.lock();
        acquired();
    }
    bool tryLock(int timeoutMs = 0)
    {
        if (!m_mutex.tryLock(timeoutMs)) return false;
        acquired();
        return true;
    }
    bool try_lock() { return tryLock(0); }
    void unlock()
    {
        released();
        m_mutex.unlock();
    }

private:
    void acquired()
    {
#ifdef PINOL_TSAN
        __tsan_acquire(this);
#endif
    }
    void released()
    {
#ifdef PINOL_TSAN
        __tsan_release(this);
#endif
    }
    Mutex m_mutex;
};
