/* Windows surface for ds4, ds4-agent, and ds4-server.
 *
 * The bench keeps using ds4_win.h alone. Frontends also need a console, a
 * poll that works on pipes, a process tree for tool shells, and the POSIX
 * calls those three programs already make. Include this before ds4.h so
 * Winsock is initialized before any later windows.h.
 */
#ifndef DS4_FRONTEND_WIN_H
#define DS4_FRONTEND_WIN_H

#ifdef _WIN32

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _CRT_RAND_S
#define _CRT_RAND_S
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#include "ds4_sockets_win.h"
#undef poll

#define DS4_WIN_NO_FCNTL
#include "../ds4_win.h"

#if defined(DS4_WIN_PTHREAD)
#include "ds4_pthread_win.h"
#else
#include <pthread.h>
#endif

#include "ds4_regex_win.h"
typedef unsigned long nfds_t;

#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__MINGW32__)
#include <dirent.h>
#include <sys/time.h>
#include <unistd.h>
#else
static inline int gettimeofday(struct timeval *tv, void *tz)
{
    FILETIME ft;
    ULARGE_INTEGER ticks;
    (void)tz;
    if (!tv) {
        errno = EINVAL;
        return -1;
    }
    GetSystemTimeAsFileTime(&ft);
    ticks.LowPart = ft.dwLowDateTime;
    ticks.HighPart = ft.dwHighDateTime;
    const uint64_t unix_ticks = ticks.QuadPart - 116444736000000000ULL;
    tv->tv_sec = (long)(unix_ticks / 10000000ULL);
    tv->tv_usec = (long)((unix_ticks % 10000000ULL) / 10ULL);
    return 0;
}
#endif

#ifndef EAGAIN
#ifdef EWOULDBLOCK
#define EAGAIN EWOULDBLOCK
#else
#define EAGAIN 11
#endif
#endif
#ifndef ENOTTY
#define ENOTTY 25
#endif
#ifndef ECHILD
#define ECHILD 10
#endif
#ifndef ESRCH
#define ESRCH 3
#endif
#ifndef ENOTSUP
#define ENOTSUP 129
#endif

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x40000
#endif
#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
#ifndef F_DUPFD
#define F_DUPFD 0
#endif
#ifndef F_DUPFD_CLOEXEC
#define F_DUPFD_CLOEXEC 1030
#endif

#ifndef F_OK
#define F_OK 0
#endif
#ifndef X_OK
#define X_OK 1
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef R_OK
#define R_OK 4
#endif

#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef SIGTERM
#define SIGTERM 15
#endif

#ifndef WNOHANG
#define WNOHANG 1
#endif
#ifndef WIFEXITED
#define WIFEXITED(s) (((s) & 0x7f) == 0)
#endif
#ifndef WEXITSTATUS
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#endif
#ifndef WIFSIGNALED
#define WIFSIGNALED(s) (((s) & 0x7f) > 0 && ((s) & 0x7f) != 0x7f)
#endif
#ifndef WTERMSIG
#define WTERMSIG(s) ((s) & 0x7f)
#endif

#if !defined(__MINGW32__) && !defined(_PID_T_DEFINED)
typedef int pid_t;
#define _PID_T_DEFINED
#endif

#if !defined(__MINGW32__) && !defined(_USECONDS_T_DEFINED)
typedef unsigned int useconds_t;
#define _USECONDS_T_DEFINED
#endif

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) (0)
#endif
#ifndef S_IRUSR
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRWXO 0007
#endif
#if !defined(__MINGW32__) && !defined(_MODE_T_)
typedef unsigned short mode_t;
#endif

/* ---- console (linenoise) ------------------------------------------------ */
#define BRKINT  0000001
#define ICRNL   0000002
#define INPCK   0000004
#define ISTRIP  0000010
#define IXON    0000020
#define OPOST   0000001
#define CS8     0000001
#define ECHO    0000010
#define ICANON  0000002
#define IEXTEN  0000004
#define ISIG    0000040
#define VMIN    6
#define VTIME   5
#define NCCS    32
#define TCSAFLUSH 2
#define TCSANOW   0
#define TIOCGWINSZ 0x5413

struct termios {
    unsigned c_iflag;
    unsigned c_oflag;
    unsigned c_cflag;
    unsigned c_lflag;
    unsigned char c_cc[NCCS];
};

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

static struct termios ds4_fe_term;
static int ds4_fe_term_ready;
static DWORD ds4_fe_orig_in, ds4_fe_orig_out;
static int ds4_fe_orig_ready;

static inline void ds4_fe_enable_vt(void)
{
    DWORD mode = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode))
        SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE && GetConsoleMode(err, &mode))
        SetConsoleMode(err, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor))
#endif
static void ds4_fe_console_ctor(void)
{
    ds4_fe_enable_vt();
}

static inline int tcgetattr(int fd, struct termios *t)
{
    (void)fd;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(in, &mode)) {
        errno = ENOTTY;
        return -1;
    }
    if (!ds4_fe_term_ready) {
        memset(&ds4_fe_term, 0, sizeof(ds4_fe_term));
        ds4_fe_term.c_iflag = ICRNL;
        ds4_fe_term.c_oflag = OPOST;
        ds4_fe_term.c_cflag = CS8;
        ds4_fe_term.c_lflag = ECHO | ICANON | ISIG | IEXTEN;
        ds4_fe_term.c_cc[VMIN] = 1;
        ds4_fe_term.c_cc[VTIME] = 0;
        ds4_fe_term_ready = 1;
    }
    *t = ds4_fe_term;
    return 0;
}

static inline int tcsetattr(int fd, int opt, const struct termios *t)
{
    (void)fd;
    (void)opt;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD in_mode = 0, out_mode = 0;
    if (!GetConsoleMode(in, &in_mode)) {
        errno = ENOTTY;
        return -1;
    }
    GetConsoleMode(out, &out_mode);
    int raw = (t->c_lflag & (ECHO | ICANON)) == 0;
    if (raw) {
        if (!ds4_fe_orig_ready) {
            ds4_fe_orig_in = in_mode;
            ds4_fe_orig_out = out_mode;
            ds4_fe_orig_ready = 1;
        }
        SetConsoleMode(in, ENABLE_VIRTUAL_TERMINAL_INPUT);
        SetConsoleMode(out, ds4_fe_orig_out | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                ENABLE_PROCESSED_OUTPUT);
    } else if (ds4_fe_orig_ready) {
        SetConsoleMode(in, ds4_fe_orig_in);
        SetConsoleMode(out, ds4_fe_orig_out | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                ENABLE_PROCESSED_OUTPUT);
    }
    ds4_fe_term = *t;
    ds4_fe_term_ready = 1;
    return 0;
}

static inline int ds4_fe_ioctl(int fd, unsigned long req, ...)
{
    (void)fd;
    if (req != TIOCGWINSZ) {
        errno = EINVAL;
        return -1;
    }
    va_list ap;
    va_start(ap, req);
    struct winsize *ws = va_arg(ap, struct winsize *);
    va_end(ap);
    if (!ws) {
        errno = EINVAL;
        return -1;
    }
    CONSOLE_SCREEN_BUFFER_INFO info;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleScreenBufferInfo(out, &info)) {
        errno = ENOTTY;
        return -1;
    }
    ws->ws_col = (unsigned short)(info.srWindow.Right - info.srWindow.Left + 1);
    ws->ws_row = (unsigned short)(info.srWindow.Bottom - info.srWindow.Top + 1);
    ws->ws_xpixel = 0;
    ws->ws_ypixel = 0;
    return 0;
}

/* ---- signals ------------------------------------------------------------- */
typedef struct {
    int dummy;
} ds4_fe_sigset;
#define sigset_t ds4_fe_sigset
#define sigemptyset(set) (((set)->dummy = 0), 0)

struct sigaction {
    void (*sa_handler)(int);
    ds4_fe_sigset sa_mask;
    int sa_flags;
};
#ifndef SA_RESTART
#define SA_RESTART 1
#endif

static inline int ds4_fe_sigaction(int sig, const struct sigaction *act,
                                   struct sigaction *old)
{
    if (sig == SIGPIPE) {
        if (old) old->sa_handler = SIG_IGN;
        return 0;
    }
    void (*handler)(int) = (act && act->sa_handler) ? act->sa_handler : SIG_DFL;
    void (*prev)(int) = signal(sig, handler);
    if (prev == SIG_ERR) return -1;
    if (old) old->sa_handler = prev;
    return 0;
}

/* ---- directory walk (MSVC; MinGW has dirent) ----------------------------- */
#if !defined(__MINGW32__)
struct dirent {
    char d_name[260];
};
typedef struct ds4_dir {
    HANDLE h;
    WIN32_FIND_DATAA data;
    int first;
    struct dirent ent;
} DIR;

static inline DIR *opendir(const char *path)
{
    if (!path || !path[0]) {
        errno = ENOENT;
        return NULL;
    }
    char pattern[PATH_MAX];
    size_t n = strlen(path);
    int slash = n && (path[n - 1] == '/' || path[n - 1] == '\\');
    snprintf(pattern, sizeof(pattern), "%s%s*", path, slash ? "" : "\\");
    DIR *d = (DIR *)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->h = FindFirstFileA(pattern, &d->data);
    if (d->h == INVALID_HANDLE_VALUE) {
        free(d);
        errno = ENOENT;
        return NULL;
    }
    d->first = 1;
    return d;
}

static inline struct dirent *readdir(DIR *d)
{
    if (!d) return NULL;
    if (!d->first && !FindNextFileA(d->h, &d->data)) return NULL;
    d->first = 0;
    snprintf(d->ent.d_name, sizeof(d->ent.d_name), "%s", d->data.cFileName);
    return &d->ent;
}

static inline int closedir(DIR *d)
{
    if (!d) return -1;
    FindClose(d->h);
    free(d);
    return 0;
}
#endif

/* ---- glob ---------------------------------------------------------------- */
#ifndef FNM_NOMATCH
#define FNM_NOMATCH 1
#endif

static int ds4_fe_fnmatch_r(const char *pat, const char *str)
{
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return 0;
            for (; *str; str++) {
                if (ds4_fe_fnmatch_r(pat, str) == 0) return 0;
            }
            return ds4_fe_fnmatch_r(pat, str);
        }
        if (*pat == '?') {
            if (!*str) return FNM_NOMATCH;
            pat++;
            str++;
            continue;
        }
        if (*pat == '[') {
            int neg = 0, ok = 0;
            pat++;
            if (*pat == '!' || *pat == '^') {
                neg = 1;
                pat++;
            }
            if (*pat == ']') {
                if (*str == ']') ok = 1;
                pat++;
            }
            while (*pat && *pat != ']') {
                if (pat[1] == '-' && pat[2] && pat[2] != ']') {
                    if (*str >= pat[0] && *str <= pat[2]) ok = 1;
                    pat += 3;
                } else {
                    if (*pat == *str) ok = 1;
                    pat++;
                }
            }
            if (*pat == ']') pat++;
            if (neg) ok = !ok;
            if (!ok || !*str) return FNM_NOMATCH;
            str++;
            continue;
        }
        if (*pat != *str) return FNM_NOMATCH;
        pat++;
        str++;
    }
    return *str ? FNM_NOMATCH : 0;
}

static inline int ds4_fe_fnmatch(const char *pat, const char *str, int flags)
{
    (void)flags;
    if (!pat || !str) return FNM_NOMATCH;
    return ds4_fe_fnmatch_r(pat, str);
}

/* ---- nonblocking reads, /dev/urandom, fcntl ------------------------------ */
static int ds4_fe_nb_ids[128];
static int ds4_fe_nb_ready;
static unsigned char ds4_fe_urand[512];

static void ds4_fe_nb_init(void)
{
    if (ds4_fe_nb_ready) return;
    for (int i = 0; i < 128; i++) ds4_fe_nb_ids[i] = -1;
    ds4_fe_nb_ready = 1;
}

static void ds4_fe_nb_set(int fd, int on)
{
    ds4_fe_nb_init();
    int slot = -1;
    for (int i = 0; i < 128; i++) {
        if (ds4_fe_nb_ids[i] == fd) {
            if (!on) ds4_fe_nb_ids[i] = -1;
            return;
        }
        if (slot < 0 && ds4_fe_nb_ids[i] < 0) slot = i;
    }
    if (on && slot >= 0) ds4_fe_nb_ids[slot] = fd;
}

static int ds4_fe_nb_get(int fd)
{
    ds4_fe_nb_init();
    for (int i = 0; i < 128; i++) {
        if (ds4_fe_nb_ids[i] == fd) return 1;
    }
    return 0;
}

static HANDLE ds4_fe_oshandle(int fd)
{
    intptr_t h = _get_osfhandle(fd);
    if (h == -1 || h == (intptr_t)INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    return (HANDLE)h;
}

static int ds4_fe_is_socket(int fd)
{
    ds4_win_wsa_startup();
    int typ = 0;
    int len = (int)sizeof(typ);
    return getsockopt((SOCKET)fd, SOL_SOCKET, SO_TYPE, (char *)&typ, &len) == 0;
}

static inline int ds4_fe_open(const char *path, int flags, ...)
{
    if (path && strcmp(path, "/dev/urandom") == 0) {
        int fd = _open("NUL", _O_RDONLY);
        if (fd >= 0 && fd < (int)sizeof(ds4_fe_urand)) ds4_fe_urand[fd] = 1;
        return fd;
    }
    if (path && strcmp(path, "/dev/null") == 0) path = "NUL";
    int mode = 0;
    if (flags & _O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    int crt = flags;
    crt &= ~O_NONBLOCK;
    crt &= ~O_NOFOLLOW;
    crt |= _O_BINARY;
    if (flags & _O_CREAT) return _open(path, crt, mode);
    return _open(path, crt);
}

static inline ssize_t ds4_fe_read(int fd, void *buf, size_t n)
{
    if (fd >= 0 && fd < (int)sizeof(ds4_fe_urand) && ds4_fe_urand[fd]) {
        unsigned char *p = (unsigned char *)buf;
        size_t left = n;
        while (left) {
            unsigned int v = 0;
            if (rand_s(&v) != 0) {
                errno = EIO;
                return -1;
            }
            size_t c = left < sizeof(v) ? left : sizeof(v);
            memcpy(p, &v, c);
            p += c;
            left -= c;
        }
        return (ssize_t)n;
    }
    if (ds4_fe_nb_get(fd)) {
        HANDLE h = ds4_fe_oshandle(fd);
        DWORD kind = h == INVALID_HANDLE_VALUE ? FILE_TYPE_UNKNOWN : GetFileType(h);
        if (kind == FILE_TYPE_PIPE) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) return 0;
                errno = EIO;
                return -1;
            }
            if (avail == 0) {
                errno = EAGAIN;
                return -1;
            }
            if (n > avail) n = avail;
        } else if (kind == FILE_TYPE_CHAR) {
            DWORD events = 0;
            if (!GetNumberOfConsoleInputEvents(h, &events) || events == 0) {
                errno = EAGAIN;
                return -1;
            }
        }
    }
    if (n > 0x7fffffffu) n = 0x7fffffffu;
    return (ssize_t)_read(fd, buf, (unsigned)n);
}

static inline ssize_t ds4_fe_write(int fd, const void *buf, size_t n)
{
    if (n > 0x7fffffffu) n = 0x7fffffffu;
    return (ssize_t)_write(fd, buf, (unsigned)n);
}

static inline int ds4_fe_close(int fd)
{
    if (fd >= 0 && fd < (int)sizeof(ds4_fe_urand)) ds4_fe_urand[fd] = 0;
    ds4_fe_nb_set(fd, 0);
    return ds4_win_close(fd);
}

static inline int ds4_fe_fcntl(int fd, int cmd, ...)
{
    if (cmd == F_GETFL) return ds4_fe_nb_get(fd) ? O_NONBLOCK : 0;
    if (cmd == F_SETFD) {
        va_list ap;
        va_start(ap, cmd);
        int flags = va_arg(ap, int);
        va_end(ap);
        HANDLE h = ds4_fe_oshandle(fd);
        if (h == INVALID_HANDLE_VALUE) {
            errno = EBADF;
            return -1;
        }
        if (!SetHandleInformation(h, HANDLE_FLAG_INHERIT,
                                  (flags & FD_CLOEXEC) ? 0 : HANDLE_FLAG_INHERIT)) {
            errno = EIO;
            return -1;
        }
        return 0;
    }
    if (cmd == F_SETFL) {
        va_list ap;
        va_start(ap, cmd);
        int flags = va_arg(ap, int);
        va_end(ap);
        int on = (flags & O_NONBLOCK) != 0;
        if (ds4_fe_is_socket(fd)) {
            u_long nb = on ? 1 : 0;
            if (ioctlsocket((SOCKET)fd, FIONBIO, &nb) != 0) {
                errno = WSAGetLastError();
                return -1;
            }
        }
        ds4_fe_nb_set(fd, on);
        return 0;
    }
    if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
        va_list ap;
        va_start(ap, cmd);
        int minfd = va_arg(ap, int);
        va_end(ap);
        int held[64];
        int nheld = 0;
        int result = -1;
        while (nheld < 64) {
            int d = _dup(fd);
            if (d < 0) break;
            if (d >= minfd) {
                result = d;
                break;
            }
            held[nheld++] = d;
        }
        for (int i = 0; i < nheld; i++) _close(held[i]);
        if (result < 0) return -1;
        if (cmd == F_DUPFD_CLOEXEC) {
            HANDLE h = ds4_fe_oshandle(result);
            if (h != INVALID_HANDLE_VALUE)
                SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);
        }
        return result;
    }
    errno = EINVAL;
    return -1;
}

static inline int ds4_fe_pipe(int fds[2])
{
    return _pipe(fds, 64 * 1024, _O_BINARY);
}

/* ---- poll for sockets, pipes, and the console ---------------------------- */
static int ds4_fe_poll_one(struct pollfd *p)
{
    p->revents = 0;
    if ((intptr_t)p->fd < 0) return 0;
    if (ds4_fe_is_socket(p->fd)) {
        struct pollfd one = *p;
        int r = WSAPoll(&one, 1, 0);
        if (r < 0) return -1;
        p->revents = one.revents;
        return p->revents ? 1 : 0;
    }
    HANDLE h = ds4_fe_oshandle(p->fd);
    if (h == INVALID_HANDLE_VALUE) {
        p->revents = POLLNVAL;
        return 1;
    }
    DWORD kind = GetFileType(h);
    if (kind == FILE_TYPE_PIPE) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE) {
                p->revents = POLLHUP;
                return 1;
            }
            p->revents = POLLERR;
            return 1;
        }
        if (avail > 0 && (p->events & POLLIN)) {
            p->revents = POLLIN;
            return 1;
        }
        return 0;
    }
    if (kind == FILE_TYPE_CHAR) {
        DWORD n = 0;
        if (GetNumberOfConsoleInputEvents(h, &n) && n > 0 && (p->events & POLLIN)) {
            p->revents = POLLIN;
            return 1;
        }
        return 0;
    }
    if (kind == FILE_TYPE_DISK && (p->events & POLLIN)) {
        p->revents = POLLIN;
        return 1;
    }
    return 0;
}

static inline int ds4_fe_poll(struct pollfd *fds, unsigned long nfds, int timeout)
{
    int sockets = 1;
    for (unsigned long i = 0; i < nfds; i++) {
        if ((intptr_t)fds[i].fd >= 0 && !ds4_fe_is_socket(fds[i].fd)) sockets = 0;
    }
    if (sockets) {
        int r = WSAPoll(fds, (ULONG)nfds, timeout);
        if (r < 0) errno = WSAGetLastError();
        return r;
    }
    ULONGLONG start = GetTickCount64();
    for (;;) {
        int ready = 0;
        for (unsigned long i = 0; i < nfds; i++) {
            int rc = ds4_fe_poll_one(&fds[i]);
            if (rc < 0) {
                errno = EIO;
                return -1;
            }
            ready += rc;
        }
        if (ready || timeout == 0) return ready;
        ULONGLONG elapsed = GetTickCount64() - start;
        if (timeout > 0 && elapsed >= (ULONGLONG)timeout) return 0;
        DWORD slice = 10;
        if (timeout > 0) {
            DWORD left = (DWORD)((ULONGLONG)timeout - elapsed);
            if (left < slice) slice = left ? left : 1;
        }
        Sleep(slice);
    }
}

/* ---- files --------------------------------------------------------------- */
static inline int ds4_fe_mkdir(const char *path, int mode)
{
    (void)mode;
    if (!path || !path[0]) {
        errno = EINVAL;
        return -1;
    }
    char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", path);
    size_t n = strlen(buf);
    while (n > 1 && (buf[n - 1] == '/' || buf[n - 1] == '\\')) buf[--n] = '\0';
    if (n == 2 && buf[1] == ':') return 0;
    return _mkdir(buf);
}

static inline int ds4_fe_rename(const char *from, const char *to)
{
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING)) return 0;
    errno = EACCES;
    return -1;
}

static inline int ds4_fe_link(const char *from, const char *to)
{
    if (CreateHardLinkA(to, from, NULL)) return 0;
    errno = EEXIST;
    return -1;
}

static inline int ds4_fe_fsync(int fd)
{
    HANDLE h = ds4_fe_oshandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    if (!FlushFileBuffers(h)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static inline int ds4_fe_fchmod(int fd, int mode)
{
    (void)fd;
    (void)mode;
    return 0;
}

static inline int ds4_fe_fchown(int fd, int uid, int gid)
{
    (void)fd;
    (void)uid;
    (void)gid;
    return 0;
}

static inline int ds4_fe_access(const char *path, int mode)
{
    int m = 0;
    if (mode & R_OK) m |= 4;
    if (mode & W_OK) m |= 2;
    return _access(path, m);
}

static inline mode_t ds4_fe_umask(mode_t mode)
{
    (void)mode;
    return 0;
}

/* ---- child processes ----------------------------------------------------- */
typedef struct {
    DWORD pid;
    HANDLE process;
    HANDLE job;
    int used;
    int killed;
} ds4_fe_proc;

static ds4_fe_proc ds4_fe_procs[64];

static inline ds4_fe_proc *ds4_fe_track_process(DWORD pid, HANDLE process, HANDLE job)
{
    for (int i = 0; i < 64; i++) {
        if (!ds4_fe_procs[i].used) {
            ds4_fe_procs[i].pid = pid;
            ds4_fe_procs[i].process = process;
            ds4_fe_procs[i].job = job;
            ds4_fe_procs[i].used = 1;
            ds4_fe_procs[i].killed = 0;
            return &ds4_fe_procs[i];
        }
    }
    return NULL;
}

static ds4_fe_proc *ds4_fe_find_proc(DWORD pid)
{
    for (int i = 0; i < 64; i++) {
        if (ds4_fe_procs[i].used && ds4_fe_procs[i].pid == pid) return &ds4_fe_procs[i];
    }
    return NULL;
}

static inline int ds4_fe_kill(pid_t pid, int sig)
{
    int group = 0;
    if (pid < 0) {
        group = 1;
        pid = -pid;
    }
    if (pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    ds4_fe_proc *proc = ds4_fe_find_proc((DWORD)pid);
    if (sig == 0) {
        if (proc) return 0;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
        if (!h) {
            errno = ESRCH;
            return -1;
        }
        CloseHandle(h);
        return 0;
    }
    if (proc) proc->killed = 1;
    if (group && proc && proc->job) {
        if (!TerminateJobObject(proc->job, 1)) {
            errno = EPERM;
            return -1;
        }
        return 0;
    }
    HANDLE h = proc ? proc->process : OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (!h) {
        errno = ESRCH;
        return -1;
    }
    BOOL ok = TerminateProcess(h, 1);
    if (!proc) CloseHandle(h);
    if (!ok) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

static inline pid_t ds4_fe_waitpid(pid_t pid, int *status, int options)
{
    if (pid <= 0) {
        errno = ECHILD;
        return -1;
    }
    ds4_fe_proc *proc = ds4_fe_find_proc((DWORD)pid);
    HANDLE h = proc ? proc->process
                    : OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                  FALSE, (DWORD)pid);
    if (!h) {
        errno = ECHILD;
        return -1;
    }
    DWORD w = WaitForSingleObject(h, (options & WNOHANG) ? 0 : INFINITE);
    if (w == WAIT_TIMEOUT) {
        if (!proc) CloseHandle(h);
        return 0;
    }
    if (w != WAIT_OBJECT_0) {
        if (!proc) CloseHandle(h);
        errno = EINTR;
        return -1;
    }
    DWORD code = 1;
    GetExitCodeProcess(h, &code);
    if (status) {
        if (proc && proc->killed) *status = SIGKILL;
        else *status = (int)((code & 0xff) << 8);
    }
    if (proc) {
        CloseHandle(proc->process);
        if (proc->job) CloseHandle(proc->job);
        proc->process = NULL;
        proc->job = NULL;
        proc->used = 0;
    } else {
        CloseHandle(h);
    }
    return pid;
}

static inline int ds4_fe_is_bash_name(const char *path)
{
    const char *base = path ? path : "";
    for (const char *p = base; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return strncmp(base, "bash", 4) == 0 || strncmp(base, "sh.exe", 6) == 0 ||
           strcmp(base, "sh") == 0;
}

static inline int ds4_fe_find_shell(char *out, size_t cap, int *bash)
{
    const char *env = getenv("DS4_SHELL");
    if (env && env[0] && _access(env, 0) == 0) {
        snprintf(out, cap, "%s", env);
        *bash = ds4_fe_is_bash_name(env);
        return 0;
    }
    if (SearchPathA(NULL, "bash.exe", NULL, (DWORD)cap, out, NULL)) {
        *bash = 1;
        return 0;
    }
    const char *cands[] = {
        "C:\\Program Files\\Git\\bin\\bash.exe",
        "C:\\Program Files\\Git\\usr\\bin\\bash.exe",
        NULL
    };
    for (int i = 0; cands[i]; i++) {
        if (_access(cands[i], 0) == 0) {
            snprintf(out, cap, "%s", cands[i]);
            *bash = 1;
            return 0;
        }
    }
    if (!SearchPathA(NULL, "cmd.exe", NULL, (DWORD)cap, out, NULL))
        snprintf(out, cap, "%s", "C:\\Windows\\System32\\cmd.exe");
    *bash = 0;
    return 0;
}

static inline const char *ds4_fe_home(void)
{
    const char *h = getenv("HOME");
    if (h && h[0]) return h;
    h = getenv("USERPROFILE");
    if (h && h[0]) return h;
    return ".";
}

static inline struct tm *localtime_r(const time_t *t, struct tm *out)
{
    return localtime_s(out, t) == 0 ? out : NULL;
}
#if !defined(__MINGW32__)
#define strtok_r(s, delim, save) strtok_s((s), (delim), (save))
#define getpid _getpid
#define isatty _isatty
#define fileno _fileno
#endif

#ifdef lstat
#undef lstat
#endif

#undef close
#undef read
#undef write
#undef open
#undef poll

#define read(fd, buf, n) ds4_fe_read((fd), (buf), (n))
#define write(fd, buf, n) ds4_fe_write((fd), (buf), (size_t)(n))
#define open(...) ds4_fe_open(__VA_ARGS__)
#define close(fd) ds4_fe_close(fd)
#define fcntl(...) ds4_fe_fcntl(__VA_ARGS__)
#define pipe(fds) ds4_fe_pipe(fds)
#define poll(fds, n, timeout) ds4_fe_poll((fds), (unsigned long)(n), (int)(timeout))
#define ioctl(...) ds4_fe_ioctl(__VA_ARGS__)
#define sigaction(sig, act, old) ds4_fe_sigaction((sig), (act), (old))
#define mkdir(path, mode) ds4_fe_mkdir((path), (mode))
#define rename(from, to) ds4_fe_rename((from), (to))
#define link(from, to) ds4_fe_link((from), (to))
#define fsync(fd) ds4_fe_fsync(fd)
#define fchmod(fd, mode) ds4_fe_fchmod((fd), (mode))
#define fchown(fd, uid, gid) ds4_fe_fchown((fd), (uid), (gid))
#define access(path, mode) ds4_fe_access((path), (mode))
#define umask(mode) ds4_fe_umask(mode)
#define fnmatch(pat, str, flags) ds4_fe_fnmatch((pat), (str), (flags))
#define kill(pid, sig) ds4_fe_kill((pid), (sig))
#define waitpid(pid, status, options) ds4_fe_waitpid((pid), (status), (options))
#define lstat stat
#define chdir(path) _chdir(path)
#define unlink(path) _unlink(path)
#define dup2(a, b) _dup2((a), (b))

#endif /* _WIN32 */
#endif /* DS4_FRONTEND_WIN_H */
