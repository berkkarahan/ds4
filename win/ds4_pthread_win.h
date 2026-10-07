/* ds4_pthread_win.h — minimal pthread shim for native Windows GPU builds.
 *
 * The native-Windows CPU build (MinGW-w64) gets pthreads from winpthreads, but
 * the native-Windows ROCm/HIP build compiles the C host code with clang in the
 * MSVC ABI (to match the hipcc-built ds4_cuda.o), and the MSVC toolchain has no
 * <pthread.h>. This header implements exactly the pthread subset DS4 uses on
 * top of the Win32 threading primitives:
 *
 *   pthread_t, pthread_create, pthread_join, pthread_self, pthread_equal
 *   pthread_detach (reclaimed exactly once via the lifecycle state machine)
 *   pthread_mutex_t / _init / _lock / _unlock / _destroy
 *   pthread_cond_t  / _init / _wait / _signal / _broadcast / _destroy
 *   pthread_once_t  / pthread_once / PTHREAD_ONCE_INIT
 *
 * Header-only and self-contained. The entire body is guarded by _WIN32, and it
 * is only pulled in for the Windows GPU build (not the MinGW CPU build, which
 * already has real pthreads), so POSIX builds are completely unaffected.
 *
 * Included directly by the host TUs that set DS4_WIN_PTHREAD (ds4.c,
 * ds4_rocm.cu, ds4_tp.c, ds4_distributed.c), so the MinGW CPU build keeps
 * using winpthreads.
 */
#ifndef DS4_PTHREAD_WIN_H
#define DS4_PTHREAD_WIN_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

/* ---- threads ------------------------------------------------------------- */
typedef struct {
    HANDLE        handle;
    unsigned      thread_id;
    void         *(*start)(void *);
    void         *arg;
    void         *retval;
    /* Lifecycle: 0 running+joinable, 1 running+detached,
     * 2 finished+joinable, 3 finished+detached (resources reclaimed).
     * The trampoline and pthread_detach each CAS this into the terminal
     * finished+detached state; whoever performs that last transition owns the
     * CloseHandle/free, so a detached thread is reclaimed exactly once. */
    volatile LONG state;
} ds4_pthread_state;
typedef ds4_pthread_state *pthread_t;

static unsigned __stdcall ds4_pthread_trampoline(void *p)
{
    ds4_pthread_state *st = (ds4_pthread_state *)p;
    st->retval = st->start(st->arg);
    for (;;) {
        const LONG cur = InterlockedCompareExchange(&st->state, 0, 0);
        LONG next = -1;
        if (cur == 0) next = 2;      /* finished while joinable: joiner reclaims */
        else if (cur == 1) next = 3; /* finished after detach: reclaim here */
        else break;
        if (InterlockedCompareExchange(&st->state, next, cur) == cur) {
            if (next == 3) { CloseHandle(st->handle); free(st); }
            break;
        }
    }
    return 0;
}

static inline int pthread_create(pthread_t *thread, const void *attr,
                                 void *(*start)(void *), void *arg)
{
    (void)attr;
    ds4_pthread_state *st = (ds4_pthread_state *)calloc(1, sizeof(*st));
    if (!st) return EAGAIN;
    st->start = start;
    st->arg   = arg;
    uintptr_t h = _beginthreadex(NULL, 0, ds4_pthread_trampoline, st, 0,
                                 &st->thread_id);
    if (h == 0) { free(st); return EAGAIN; }
    st->handle = (HANDLE)h;
    *thread = st;
    return 0;
}

static inline int pthread_join(pthread_t thread, void **retval)
{
    if (!thread || ((uintptr_t)thread & 1u)) return EINVAL;
    if (InterlockedCompareExchange(&thread->state, 0, 0) == 1) {
        return EINVAL; /* joining a detached thread is undefined per POSIX */
    }
    if (thread->thread_id == GetCurrentThreadId()) return EDEADLK;
    if (WaitForSingleObject(thread->handle, INFINITE) != WAIT_OBJECT_0) return EINVAL;
    if (retval) *retval = thread->retval;
    CloseHandle(thread->handle);
    free(thread);
    return 0;
}

/* pthread_detach: mark the thread detached. If it already finished, reclaim
 * its handle/state here; otherwise the trampoline reclaims on exit. */
static inline int pthread_detach(pthread_t thread)
{
    if (!thread || ((uintptr_t)thread & 1u)) return EINVAL;
    for (;;) {
        const LONG cur = InterlockedCompareExchange(&thread->state, 0, 0);
        LONG next = -1;
        if (cur == 0) next = 1;      /* running: trampoline reclaims at exit */
        else if (cur == 2) next = 3; /* finished: reclaim now */
        else return 0;               /* already detached or reclaimed */
        if (InterlockedCompareExchange(&thread->state, next, cur) == cur) {
            if (next == 3) { CloseHandle(thread->handle); free(thread); }
            return 0;
        }
    }
}

/* pthread_self / pthread_equal: a pthread created by this shim is represented
 * by its state pointer; pthread_self returns a low-bit-tagged Win32 thread ID.
 * pthread_equal normalizes either representation, so caller threads that were
 * not created through this shim still have distinct, comparable identities. */
static inline pthread_t pthread_self(void)
{
    return (pthread_t)((((uintptr_t)GetCurrentThreadId()) << 1u) | 1u);
}

static inline int pthread_equal(pthread_t a, pthread_t b)
{
    uintptr_t av = (uintptr_t)a;
    uintptr_t bv = (uintptr_t)b;
    uintptr_t aid = (av & 1u) ? av >> 1u :
                    a ? (uintptr_t)((ds4_pthread_state *)a)->thread_id : 0;
    uintptr_t bid = (bv & 1u) ? bv >> 1u :
                    b ? (uintptr_t)((ds4_pthread_state *)b)->thread_id : 0;
    return aid == bid;
}

/* ---- mutex ---------------------------------------------------------------
 * Default mutexes are non-recursive SRW locks, including the static
 * initializer used by the engine. A mutex created with
 * PTHREAD_MUTEX_RECURSIVE tracks the owning thread so the server can re-enter
 * its inference lock. Condition waits always sleep on the SRW itself, so a
 * recursive mutex must be held exactly once across pthread_cond_wait. */
typedef struct ds4_pthread_mutex {
    SRWLOCK lock;
    DWORD owner;
    LONG depth;
    int recursive;
} pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER {SRWLOCK_INIT, 0, 0, 0}
#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1

typedef struct ds4_pthread_mutexattr {
    int type;
} pthread_mutexattr_t;

static inline int pthread_mutexattr_init(pthread_mutexattr_t *a)
{
    if (!a) return EINVAL;
    a->type = PTHREAD_MUTEX_NORMAL;
    return 0;
}
static inline int pthread_mutexattr_destroy(pthread_mutexattr_t *a)
{
    (void)a;
    return 0;
}
static inline int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type)
{
    if (!a) return EINVAL;
    a->type = type;
    return 0;
}

static inline int pthread_mutex_init(pthread_mutex_t *m, const void *attr)
{
    const pthread_mutexattr_t *a = (const pthread_mutexattr_t *)attr;
    InitializeSRWLock(&m->lock);
    m->owner = 0;
    m->depth = 0;
    m->recursive = a && a->type == PTHREAD_MUTEX_RECURSIVE;
    return 0;
}
static inline int pthread_mutex_lock(pthread_mutex_t *m)
{
    if (m->recursive) {
        DWORD tid = GetCurrentThreadId();
        if (m->owner == tid) {
            m->depth++;
            return 0;
        }
        AcquireSRWLockExclusive(&m->lock);
        m->owner = tid;
        m->depth = 1;
        return 0;
    }
    AcquireSRWLockExclusive(&m->lock);
    return 0;
}
static inline int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if (m->recursive) {
        if (m->owner != GetCurrentThreadId() || m->depth <= 0) return EPERM;
        if (--m->depth > 0) return 0;
        m->owner = 0;
    }
    ReleaseSRWLockExclusive(&m->lock);
    return 0;
}
static inline int pthread_mutex_destroy(pthread_mutex_t *m)
{
    (void)m; /* SRWLOCK needs no teardown */
    return 0;
}

/* ---- condition variable -------------------------------------------------- */
typedef CONDITION_VARIABLE pthread_cond_t;
#define PTHREAD_COND_INITIALIZER CONDITION_VARIABLE_INIT

static inline int pthread_cond_init(pthread_cond_t *c, const void *attr)
{
    (void)attr;
    InitializeConditionVariable(c);
    return 0;
}
static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    /* SRWLOCK held exclusively → CONDITION_VARIABLE_LOCKMODE default (0). */
    return SleepConditionVariableSRW(c, &m->lock, INFINITE, 0) ? 0 : EINVAL;
}
/* Absolute CLOCK_REALTIME deadline, matching ds4_win.h clock_gettime. */
#ifndef ETIMEDOUT
#define ETIMEDOUT 138
#endif
static inline int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                                         const struct timespec *abs)
{
    if (!abs) return EINVAL;
    FILETIME ft;
    ULARGE_INTEGER ticks;
    GetSystemTimeAsFileTime(&ft);
    ticks.LowPart = ft.dwLowDateTime;
    ticks.HighPart = ft.dwHighDateTime;
    const uint64_t unix_ticks = ticks.QuadPart - 116444736000000000ULL;
    const int64_t now_ms = (int64_t)(unix_ticks / 10000ULL);
    const int64_t abs_ms = (int64_t)abs->tv_sec * 1000 + abs->tv_nsec / 1000000L;
    int64_t wait_ms = abs_ms - now_ms;
    if (wait_ms <= 0) return ETIMEDOUT;
    if (wait_ms > 0x7fffffff) wait_ms = 0x7fffffff;
    if (SleepConditionVariableSRW(c, &m->lock, (DWORD)wait_ms, 0)) return 0;
    return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
}
static inline int pthread_cond_signal(pthread_cond_t *c)
{
    WakeConditionVariable(c);
    return 0;
}
static inline int pthread_cond_broadcast(pthread_cond_t *c)
{
    WakeAllConditionVariable(c);
    return 0;
}
static inline int pthread_cond_destroy(pthread_cond_t *c)
{
    (void)c; /* CONDITION_VARIABLE needs no teardown */
    return 0;
}

/* ---- one-time init ------------------------------------------------------- */
typedef INIT_ONCE pthread_once_t;
#define PTHREAD_ONCE_INIT INIT_ONCE_STATIC_INIT

static BOOL CALLBACK ds4_once_trampoline(PINIT_ONCE io, PVOID param, PVOID *ctx)
{
    (void)io; (void)ctx;
    ((void (*)(void))param)();
    return TRUE;
}
static inline int pthread_once(pthread_once_t *once, void (*init)(void))
{
    InitOnceExecuteOnce(once, ds4_once_trampoline, (PVOID)init, NULL);
    return 0;
}

#endif /* _WIN32 */
#endif /* DS4_PTHREAD_WIN_H */
