/* ds4_win.h — minimal POSIX compatibility layer for native Windows builds.
 *
 * Provides just the POSIX surface ds4.c relies on that MinGW/UCRT lacks:
 *   - mmap / munmap / madvise (read-only file mappings)
 *   - sysconf(_SC_NPROCESSORS_ONLN / _SC_PAGESIZE)
 *   - flock / fcntl(F_SETFD,FD_CLOEXEC) / pread / ftruncate / dprintf  (instance lock)
 *   - realpath / mkstemp / ftello / fseeko / str[n]casecmp (MSVC ABI side)
 *   - fmemopen (fixed-buffer "wb"/"rb", temp-file backed with copy-back on close)
 *   - ds4_win_temp_dir (per-user %TEMP% replacement for hardcoded /tmp paths)
 *
 * Header-only, self-contained, no third-party deps. The whole body is guarded by
 * _WIN32, so this header is inert on POSIX platforms. ds4.c includes it in place
 * of <sys/mman.h> (and the other POSIX-only surface) behind #ifdef _WIN32, so the
 * native MinGW-w64 CPU build needs no extra include/search-path flags. MinGW
 * already provides pthread, clock_gettime and ftruncate.
 */
#ifndef DS4_WIN_H
#define DS4_WIN_H

#ifdef _WIN32

/* WIN32_LEAN_AND_MEAN keeps <windows.h> from pulling in the legacy <winsock.h>
 * (v1), which clashes with <winsock2.h> used by win/ds4_sockets_win.h. Define
 * it before the first <windows.h> so the order is irrelevant across TUs. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/types.h>
#if !defined(__MINGW32__)
#include <share.h>           /* _SH_DENYNO for the mkstemp shim (MSVC ABI) */
#endif

/* ---- mmap ---------------------------------------------------------------- */
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FAILED  ((void *)-1)
#define POSIX_MADV_NORMAL     0
#define POSIX_MADV_RANDOM     1
#define POSIX_MADV_SEQUENTIAL 2
#define POSIX_MADV_WILLNEED   3
#define POSIX_MADV_DONTNEED   4
#define MADV_WILLNEED POSIX_MADV_WILLNEED

#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE         0x1
#endif
#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 0x2
#endif

/* ---- misc POSIX surface used by the GPU (HIP) host code ------------------ */
/* MinGW/UCRT supplies these; the clang-MSVC HIP toolchain (ds4_cuda.cu build)
 * does not. Guard each so the MinGW CPU build is unaffected. */
#ifndef STDIN_FILENO
#define STDIN_FILENO  0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif

#ifndef SSIZE_MAX
#define SSIZE_MAX ((ssize_t)(((size_t)-1) >> 1))
#endif

/* ssize_t / off_t: MinGW defines these via <sys/types.h>; MSVC does not.
 * MSVC exposes _SSIZE_T_DEFINED once <BaseTsd.h> (pulled in by windows.h) and
 * the CRT have declared SSIZE_T; provide ssize_t/off_t only when absent. */
#if !defined(_SSIZE_T_DEFINED) && !defined(__MINGW32__) && !defined(_SSIZE_T_)
typedef SSIZE_T ssize_t;
#define _SSIZE_T_DEFINED
#endif
#if !defined(_OFF_T_DEFINED) && !defined(__MINGW32__)
/* MSVC <sys/types.h> already typedefs off_t to long; only define if missing. */
#ifndef _OFF_T_
typedef long long off_t;
#endif
#endif

/* ---- 64-bit file stat --------------------------------------------------- */
/* The Windows CRT's default `struct stat` / stat() / fstat() carry a 32-bit
 * st_size, so stat'ing a file larger than 2 GB fails with EOVERFLOW
 * ("value too large") — fatal for the ~80 GB DeepSeek V4 GGUF. Remap the bare
 * names to the 64-bit `_stat64` family (which names both the struct and the
 * functions, with an __int64 st_size).
 *
 * <sys/stat.h> is pulled in here first so its own real declarations are parsed
 * before the macros exist; thanks to its include guard, any later
 * `#include <sys/stat.h>` in a translation unit (e.g. ds4.c includes it after
 * this header; ds4_cuda.cu includes it before) is a no-op but still sees the
 * remap. #undef first in case the CRT already exposes stat/fstat as macros. */
#include <sys/stat.h>
#undef stat
#undef fstat
#define stat  _stat64
#define fstat _fstat64

/* clock_gettime / CLOCK_MONOTONIC: present in MinGW, absent in clang-MSVC.
 * MSVC's <time.h> already declares struct timespec, so we only supply the
 * clock id macros and the function. */
#if !defined(CLOCK_MONOTONIC) && !defined(__MINGW32__)
#include <time.h>
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
static inline int clock_gettime(int clk, struct timespec *ts)
{
    if (!ts || (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC)) {
        errno = EINVAL;
        return -1;
    }
    if (clk == CLOCK_REALTIME) {
        FILETIME ft;
        ULARGE_INTEGER ticks;
        GetSystemTimeAsFileTime(&ft);
        ticks.LowPart = ft.dwLowDateTime;
        ticks.HighPart = ft.dwHighDateTime;
        const uint64_t unix_ticks = ticks.QuadPart - 116444736000000000ULL;
        ts->tv_sec = (time_t)(unix_ticks / 10000000ULL);
        ts->tv_nsec = (long)((unix_ticks % 10000000ULL) * 100ULL);
        return 0;
    }
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    ts->tv_sec  = (long long)(cnt.QuadPart / freq.QuadPart);
    long long rem = cnt.QuadPart % freq.QuadPart;
    ts->tv_nsec = (long)((rem * 1000000000LL) / freq.QuadPart);
    return 0;
}
#endif

/* nanosleep: MinGW supplies it; the clang-MSVC build does not. Sleep at ms
 * granularity (Windows' coarsest portable sleep). Used for the bench pacing
 * delay and ds4.c backoff loops, where exact sub-ms timing is not required. */
#if !defined(__MINGW32__)
static inline int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    if (!req) { errno = EINVAL; return -1; }
    DWORD ms = (DWORD)(req->tv_sec * 1000LL + req->tv_nsec / 1000000LL);
    Sleep(ms);
    return 0;
}
#endif

/* sleep: POSIX whole-seconds sleep used by ds4_distributed.c retry loops.
 * MinGW supplies it via <unistd.h>; the MSVC ABI build needs a shim. Returns 0
 * (no early wake on Windows). */
#if !defined(__MINGW32__)
static inline unsigned sleep(unsigned seconds)
{
    Sleep((DWORD)seconds * 1000u);
    return 0;
}
#endif

/* usleep: POSIX microsecond sleep used by ds4_tp.c's connect retry loop.
 * MinGW supplies it; the MSVC ABI build does not. Sleep at ms granularity. */
#if !defined(__MINGW32__)
static inline int usleep(unsigned usec)
{
    Sleep((DWORD)(usec / 1000u));
    return 0;
}
#endif

/* suseconds_t: POSIX type for struct timeval.tv_usec. The MSVC CRT does not
 * provide it, and MinGW-w64 leaves it undeclared under -std=c99 (its
 * <sys/time.h> only defines struct timeval). Used by ds4_tp.c gate timeouts. */
#if !defined(_SUSECONDS_T_DEFINED)
typedef long suseconds_t;
#define _SUSECONDS_T_DEFINED
#endif

/* getpagesize: removed from POSIX-2008 but still used by ds4.c. MSVC has no
 * declaration; MinGW does. Mirror sysconf(_SC_PAGESIZE). */
#if !defined(__MINGW32__)
static inline int getpagesize(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwPageSize;
}
#endif

/* PATH_MAX: <limits.h> on Windows lacks it. Use MAX_PATH (260). Buffers sized
 * with this are only used for short temp/CSV paths in the bench and loader. */
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif

static INIT_ONCE ds4_temp_once = INIT_ONCE_STATIC_INIT;
static char ds4_temp_dir[MAX_PATH + 1];
static BOOL CALLBACK ds4_temp_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    const DWORD n = GetTempPathA((DWORD)sizeof(ds4_temp_dir), ds4_temp_dir);
    if (n == 0 || n >= (DWORD)sizeof(ds4_temp_dir)) {
        ds4_temp_dir[0] = '\0';
        return FALSE;
    }
    return TRUE;
}

/* Publish the complete path once; a partially initialized directory can send
 * competing callers to different lock files. */
static inline const char *ds4_win_temp_dir(void)
{
    if (!InitOnceExecuteOnce(&ds4_temp_once, ds4_temp_init, NULL, NULL)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    return ds4_temp_dir;
}

static inline int ds4_win_temp_path(char *path, size_t cap, const char *name)
{
    const char *dir = ds4_win_temp_dir();
    if (!dir) return -1;
    const int n = snprintf(path, cap, "%s%s", dir, name);
    if (n < 0 || (size_t)n >= cap) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

/* mkstemp / ftello / fseeko: the MSVC CRT exposes _mktemp_s, _ftelli64 and
 * _fseeki64; MinGW supplies the POSIX names directly. Shim only for MSVC. */
#if !defined(__MINGW32__)
static inline int mkstemp(char *tmpl)
{
    /* tmpl ends in "XXXXXX"; _mktemp_s rewrites those in place, then open
     * O_CREAT|O_EXCL. Matches mkstemp(3) semantics closely enough for the
     * loader's scratch-file use. */
    size_t len = strlen(tmpl);
    if (_mktemp_s(tmpl, len + 1) != 0) { errno = EINVAL; return -1; }
    int fd = -1;
    if (_sopen_s(&fd, tmpl, _O_RDWR | _O_CREAT | _O_EXCL | _O_BINARY,
                 _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0) {
        return -1;
    }
    return fd;
}
#define ftello(fp)        _ftelli64(fp)
#define fseeko(fp, o, w)  _fseeki64((fp), (o), (w))

/* strcasecmp / strncasecmp: POSIX names declared in <strings.h>, absent from
 * MSVC; map to the CRT's _stricmp / _strnicmp. MinGW supplies the POSIX names. */
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#endif /* !__MINGW32__ */

/* realpath: POSIX path canonicalizer. MSVC does not declare it, and MinGW-w64
 * exposes only _fullpath (no realpath at least under -std=c99), so both
 * toolchains get the same shim. When `resolved` is NULL, allocate the result
 * (realpath(3) semantics) so callers can free() it. */
static inline char *ds4_win_realpath(const char *path, char *resolved)
{
    if (!path) { errno = EINVAL; return NULL; }
    if (_access(path, 0) != 0) return NULL;
    char *buf = _fullpath(NULL, path, 0);
    if (!buf) return NULL;
    if (resolved) {
        if (strlen(buf) >= PATH_MAX) { free(buf); errno = ENAMETOOLONG; return NULL; }
        strcpy(resolved, buf); free(buf); return resolved;
    }
    return buf;
}
#define realpath(p, r) ds4_win_realpath((p), (r))

/* ---- file locking / fd flags -------------------------------------------- */
#ifndef F_SETFD
#define F_SETFD    2
#endif
#ifndef FD_CLOEXEC
#define FD_CLOEXEC 1
#endif
/* The CRT's no-inherit flag is the Windows equivalent of O_CLOEXEC. */
#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

static inline void *mmap(void *addr, size_t length, int prot, int flags,
                         int fd, long long offset)
{
    (void)addr;
    /* Anonymous mapping (fd == -1 and/or MAP_ANONYMOUS): used for the
     * --simulate-used-memory scratch allocation in ds4_ssd.c. Back it with
     * committed private VM via VirtualAlloc so mlock()/munlock() and writes
     * behave like an anonymous POSIX mapping. munmap() distinguishes these
     * from file views by trying UnmapViewOfFile first, then VirtualFree. */
    if ((flags & MAP_ANONYMOUS) || fd < 0) {
        DWORD protect = (prot & PROT_WRITE) ? PAGE_READWRITE
                      : (prot == PROT_NONE) ? PAGE_NOACCESS : PAGE_READONLY;
        void *p = VirtualAlloc(NULL, length, MEM_COMMIT | MEM_RESERVE, protect);
        if (p == NULL) { errno = ENOMEM; return MAP_FAILED; }
        return p;
    }
    HANDLE fh = (HANDLE)_get_osfhandle(fd);
    if (fh == INVALID_HANDLE_VALUE) { errno = EBADF; return MAP_FAILED; }
    HANDLE mh = CreateFileMappingA(fh, NULL, PAGE_READONLY, 0, 0, NULL);
    if (mh == NULL) { errno = ENOMEM; return MAP_FAILED; }
    DWORD off_hi = (DWORD)((uint64_t)offset >> 32);
    DWORD off_lo = (DWORD)((uint64_t)offset & 0xFFFFFFFFu);
    void *p = MapViewOfFile(mh, FILE_MAP_READ, off_hi, off_lo, length);
    CloseHandle(mh); /* view keeps the section alive */
    if (p == NULL) { errno = ENOMEM; return MAP_FAILED; }
    return p;
}

static inline int munmap(void *addr, size_t length)
{
    (void)length;
    /* File views come from MapViewOfFile; anonymous mappings from VirtualAlloc.
     * Try the file-view unmap first; if the address is not a mapped view, fall
     * back to releasing the VirtualAlloc reservation. */
    if (UnmapViewOfFile(addr)) return 0;
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}

/* mlock/munlock: VirtualLock/VirtualUnlock pin pages in the working set. Used
 * by ds4_ssd.c's --simulate-used-memory path. VirtualLock has a per-process
 * working-set-size limit; treat best-effort failure as success would diverge
 * from POSIX, so report it via errno like mlock(2). */
static inline int mlock(const void *addr, size_t len)
{
    if (VirtualLock((void *)addr, len)) return 0;
    errno = (GetLastError() == ERROR_WORKING_SET_QUOTA) ? EAGAIN : ENOMEM;
    return -1;
}
static inline int munlock(const void *addr, size_t len)
{
    return VirtualUnlock((void *)addr, len) ? 0 : -1;
}

static inline int posix_madvise(void *addr, size_t length, int advice)
{
    (void)addr; (void)length; (void)advice;
    return 0; /* advisory only */
}
static inline int madvise(void *addr, size_t length, int advice)
{
    return posix_madvise(addr, length, advice);
}

static inline long sysconf(int name)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    if (name == _SC_NPROCESSORS_ONLN) return (long)si.dwNumberOfProcessors;
    if (name == _SC_PAGESIZE)         return (long)si.dwPageSize;
    errno = EINVAL;
    return -1;
}

static inline int flock(int fd, int op)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    OVERLAPPED ov; memset(&ov, 0, sizeof(ov));
    if (op & LOCK_UN) {
        return UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov) ? 0 : -1;
    }
    DWORD f = 0;
    if (op & LOCK_EX) f |= LOCKFILE_EXCLUSIVE_LOCK;
    if (op & LOCK_NB) f |= LOCKFILE_FAIL_IMMEDIATELY;
    if (!LockFileEx(h, f, 0, MAXDWORD, MAXDWORD, &ov)) {
        errno = (GetLastError() == ERROR_LOCK_VIOLATION) ? EWOULDBLOCK : EACCES;
        return -1;
    }
    return 0;
}

/* Frontends supply a richer fcntl (nonblocking, F_DUPFD). The bench keeps this
 * one: it only clears the inherit bit for the instance lock. */
#ifndef DS4_WIN_NO_FCNTL
static inline int fcntl(int fd, int cmd, ...)
{
    if (cmd != F_SETFD) { errno = ENOTSUP; return -1; }
    va_list ap;
    va_start(ap, cmd);
    const int flags = va_arg(ap, int);
    va_end(ap);
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (!SetHandleInformation(h, HANDLE_FLAG_INHERIT,
                              (flags & FD_CLOEXEC) ? 0 : HANDLE_FLAG_INHERIT)) {
        errno = EIO; return -1;
    }
    return 0;
}
#endif /* !DS4_WIN_NO_FCNTL */

static inline long long ds4_pread(int fd, void *buf, size_t count, long long offset)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (offset < 0) { errno = EINVAL; return -1; }
    if (count == 0) return 0;
    /* ReadFile takes a DWORD length; a size_t count that is an exact multiple
     * of 2^32 would truncate to 0. Return a short read instead (legal pread
     * behavior; callers loop). */
    if (count > 0xFFFFFFFFu) count = 0xFFFFFFFFu;
    /* An explicit offset still advances synchronous handles. ReOpenFile gives
     * this read an independent file position (DuplicateHandle does not), so
     * parallel SSD readers cannot race over a save/restore of the CRT cursor. */
    HANDLE reader = ReOpenFile(h, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
    if (reader == INVALID_HANDLE_VALUE) { errno = EIO; return -1; }
    OVERLAPPED ov; memset(&ov, 0, sizeof(ov));
    ov.Offset     = (DWORD)((uint64_t)offset & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
    DWORD got = 0;
    const int ok = ReadFile(reader, buf, (DWORD)count, &got, &ov) ? 1 : 0;
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(reader);
    if (!ok) {
        if (error == ERROR_HANDLE_EOF) return 0;
        errno = EIO; return -1;
    }
    return (long long)got;
}
#define pread(fd, buf, count, offset) ds4_pread((fd), (buf), (size_t)(count), (long long)(offset))

/* ftruncate: provided by MinGW <unistd.h>; absent in the MSVC ABI build. */
#if !defined(__MINGW32__)
static inline int ftruncate(int fd, long long length)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    LARGE_INTEGER li; li.QuadPart = length;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) { errno = EINVAL; return -1; }
    if (!SetEndOfFile(h)) { errno = EIO; return -1; }
    return 0;
}
#endif

static inline int dprintf(int fd, const char *fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
    return _write(fd, buf, (unsigned)n);
}

/* ---- fmemopen (temp-file backed, fixed buffer) --------------------------- */
typedef struct { FILE *fp; void *buf; size_t cap; int writeback; } ds4_memstream;
#define DS4_MEMSTREAM_MAX 16
static ds4_memstream ds4_ms_tab[DS4_MEMSTREAM_MAX];
static CRITICAL_SECTION ds4_ms_cs;
static INIT_ONCE ds4_ms_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK ds4_ms_init_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&ds4_ms_cs);
    return TRUE;
}

static inline void ds4_ms_ensure(void)
{
    (void)InitOnceExecuteOnce(&ds4_ms_once, ds4_ms_init_once, NULL, NULL);
}

static inline FILE *ds4_tmpfile(void)
{
    char path[MAX_PATH];
    const char *dir = ds4_win_temp_dir();
    if (!dir) return NULL;
    if (!GetTempFileNameA(dir, "ds4", 0, path)) return NULL;
    /* open read/write, delete on close */
    FILE *fp = fopen(path, "wb+TD"); /* T=temporary, D=delete-on-close */
    if (!fp) DeleteFileA(path);
    return fp;
}

static inline FILE *fmemopen(void *buf, size_t size, const char *mode)
{
    /* DS4 uses only fixed binary snapshot readers and writers. Reject modes
     * whose append/update semantics this shim does not implement. */
    if (!buf || !size || !mode || (strcmp(mode, "rb") && strcmp(mode, "wb"))) {
        errno = EINVAL; return NULL;
    }
    ds4_ms_ensure();
    int writing = (mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+')));
    FILE *fp = ds4_tmpfile();
    if (!fp) return NULL;
    if (!writing && buf && size) {
        if (fwrite(buf, 1, size, fp) != size) { fclose(fp); return NULL; }
        rewind(fp);
    }
    int slot = -1;
    EnterCriticalSection(&ds4_ms_cs);
    for (int i = 0; i < DS4_MEMSTREAM_MAX; i++) {
        if (ds4_ms_tab[i].fp == NULL) { slot = i; break; }
    }
    if (slot >= 0) {
        ds4_ms_tab[slot].fp = fp; ds4_ms_tab[slot].buf = buf;
        ds4_ms_tab[slot].cap = size; ds4_ms_tab[slot].writeback = writing ? 1 : 0;
    }
    LeaveCriticalSection(&ds4_ms_cs);
    if (slot < 0) {
        /* Table full: a stream that can never copy its buffer back on close
         * would silently drop the caller's writes, so refuse to open it. */
        fclose(fp);
        errno = EMFILE;
        return NULL;
    }
    return fp;
}

static inline int ds4_win_fclose(FILE *fp)
{
    int saved_error = 0;
    if (!fp) { errno = EINVAL; return EOF; }
    ds4_ms_ensure();
    EnterCriticalSection(&ds4_ms_cs);
    for (int i = 0; i < DS4_MEMSTREAM_MAX; i++) {
        if (ds4_ms_tab[i].fp == fp) {
            if (ds4_ms_tab[i].writeback && ds4_ms_tab[i].buf && ds4_ms_tab[i].cap) {
                void *dst = ds4_ms_tab[i].buf;
                const size_t cap = ds4_ms_tab[i].cap;
                if (ferror(fp) || fflush(fp) != 0 || _fseeki64(fp, 0, SEEK_END) != 0) {
                    saved_error = EIO;
                } else {
                    const long long end = _ftelli64(fp);
                    if (end < 0) saved_error = EIO;
                    else if ((uint64_t)end > (uint64_t)cap) saved_error = ENOSPC;
                    else if (_fseeki64(fp, 0, SEEK_SET) != 0 ||
                             fread(dst, 1, (size_t)end, fp) != (size_t)end) {
                        saved_error = EIO;
                    } else if ((size_t)end < cap) {
                        ((char *)dst)[end] = '\0';
                    }
                }
            }
            ds4_ms_tab[i].fp = NULL; ds4_ms_tab[i].buf = NULL;
            ds4_ms_tab[i].cap = 0;   ds4_ms_tab[i].writeback = 0;
            break;
        }
    }
    LeaveCriticalSection(&ds4_ms_cs);
    const int rc = fclose(fp); /* real fclose — macro defined only after this header */
    if (saved_error) { errno = saved_error; return EOF; }
    return rc;
}

#endif /* _WIN32 */

/* Redirect fclose AFTER all helpers above so ds4_win_fclose's own call hits the
 * real fclose. Source files including this header get the memory-stream-aware one. */
#ifdef _WIN32
#define fclose(fp) ds4_win_fclose(fp)
#endif

#endif /* DS4_WIN_H */
