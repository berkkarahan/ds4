/* ds4_sockets_win.h — minimal Berkeley-sockets-over-Winsock shim.
 *
 * main's refactor moved the distributed runtime (ds4_distributed.c) into
 * CORE_OBJS, so it now links into *every* binary — including ds4-bench. That
 * file is a full TCP coordinator/worker transport written against the POSIX
 * sockets API (<arpa/inet.h>, <netdb.h>, <sys/socket.h>, <poll.h>, …). The
 * Windows HIP/MSVC-ABI build has no such headers; this shim supplies just the
 * surface ds4_distributed.c uses, mapped onto Winsock2 / ws2tcpip.
 *
 * Scope: enough for ds4_distributed.c to *compile and link* on Windows so that
 * ds4-bench.exe builds, plus faithful dup() (WSADuplicateSocket) and
 * SO_RCVTIMEO/SO_SNDTIMEO handling for serving-mode traffic. The bench never
 * enters distributed serving, so the remaining runtime-fidelity gaps
 * (MSG_DONTWAIT emulation, WSACleanup lifetime) are not exercised by the
 * bench and are called out in win/README.md as follow-ups.
 *
 * Header-only, self-contained. Whole body guarded by _WIN32 (and not pulled in
 * by the MinGW CPU build, which is GPU-less and does not link the distributed
 * runtime into the bench), so POSIX builds are byte-for-byte unchanged.
 */
#ifndef DS4_SOCKETS_WIN_H
#define DS4_SOCKETS_WIN_H

#ifdef _WIN32

/* winsock2.h must precede windows.h; WIN32_LEAN_AND_MEAN stops a later
 * <windows.h> from dragging in the legacy <winsock.h>. Include the Winsock
 * headers first and let the include guards settle the order across TUs. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>      /* if_nametoindex */
#include <io.h>            /* _close fallback for non-socket fds */
#include <signal.h>        /* signal/SIG_IGN (MSVC CRT; SIGPIPE absent) */
#include <errno.h>
#include <stdio.h>

#ifdef _MSC_VER
/* The MSVC-ABI link pulls these in automatically; gcc needs them on the link
 * line instead (the Makefile windows-cpu target passes -lws2_32 -liphlpapi). */
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif

/* ---- POSIX errno aliases for the Winsock failure codes ds4_distributed.c
 * inspects. recv/send/poll set errno via the wrappers below. -------------- */
#ifndef EINTR
#define EINTR        WSAEINTR
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK  WSAEWOULDBLOCK
#endif
#ifndef EINPROGRESS
#define EINPROGRESS  WSAEINPROGRESS
#endif

/* socklen_t is defined by ws2tcpip.h on recent SDKs; guard just in case. */
#ifndef _SOCKLEN_T_DEFINED
#ifndef socklen_t
typedef int socklen_t;
#endif
#endif

/* ssize_t: MSVC spells it SSIZE_T (<BaseTsd.h>, pulled in via winsock2.h). The
 * POSIX name is defined here because this shim is included before ds4_win.h. */
#if !defined(_SSIZE_T_DEFINED) && !defined(_SSIZE_T_)
typedef SSIZE_T ssize_t;
#define _SSIZE_T_DEFINED
#endif

/* poll(): provided as WSAPoll on Windows Vista+. struct pollfd / POLL* and
 * nfds_t come from winsock2.h. SHUT_RDWR maps to SD_BOTH. */
#ifndef SHUT_RD
#define SHUT_RD   SD_RECEIVE
#define SHUT_WR   SD_SEND
#define SHUT_RDWR SD_BOTH
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0    /* Windows never raises SIGPIPE on a dead socket */
#endif
/* Winsock has no MSG_DONTWAIT; non-blocking is a socket mode, not a per-call
 * flag. ds4_tp.c's gate traffic (not exercised by the bench) is the only user. */
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

/* ds4_distributed.c calls poll() directly; route it to WSAPoll. */
#define poll(fds, n, timeout) WSAPoll((fds), (ULONG)(n), (int)(timeout))

/* close() is used on socket fds (overwhelmingly) and, in one path, on the fd
 * returned by mkstemp(). Try closesocket() first; if the descriptor is not a
 * socket, fall back to the CRT _close(). This keeps both cases correct. */
static __inline int ds4_win_close(int fd)
{
    if (closesocket((SOCKET)fd) == 0) return 0;
    if (WSAGetLastError() == WSAENOTSOCK) return _close(fd);
    return -1;
}
#define close(fd) ds4_win_close(fd)

/* signal(SIGPIPE, SIG_IGN): Windows has no SIGPIPE; make it a no-op. The
 * generic signal()/SIGINT path that the MSVC CRT *does* support is unaffected
 * because ds4_distributed.c only ever ignores SIGPIPE. */
#ifndef SIGPIPE
#define SIGPIPE 13
#endif

/* WSA bootstrap: ds4_distributed.c has no WSAStartup call (it is POSIX code).
 * Run it once on first socket use via a constructor-style guard. clang-cl
 * supports __attribute__((constructor)); fall back to lazy init otherwise. */
static __inline void ds4_win_wsa_startup(void)
{
    static volatile long started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) == 0) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
}

/* Wrap the entry-point socket-creating calls so Winsock is initialized before
 * first use, and translate the last Winsock error into errno for the EINTR /
 * EWOULDBLOCK checks in ds4_distributed.c. */
static __inline SOCKET ds4_win_socket(int af, int type, int proto)
{
    ds4_win_wsa_startup();
    SOCKET s = socket(af, type, proto);
    if (s == INVALID_SOCKET) errno = WSAGetLastError();
    return s;
}
#define socket(af, type, proto) ds4_win_socket((af), (type), (proto))

static __inline int ds4_win_getaddrinfo(const char *node, const char *service,
                                        const struct addrinfo *hints,
                                        struct addrinfo **res)
{
    ds4_win_wsa_startup();
    return getaddrinfo(node, service, hints, res);
}
#define getaddrinfo(n, s, h, r) ds4_win_getaddrinfo((n), (s), (h), (r))

/* recv/send/accept/connect set errno from the Winsock error so the POSIX-style
 * `errno == EINTR` retry loops in ds4_distributed.c behave. */
static __inline int ds4_win_recv(int s, void *buf, size_t len, int flags)
{
    int r = recv((SOCKET)s, (char *)buf, (int)len, flags);
    if (r < 0) errno = WSAGetLastError();
    return r;
}
#define recv(s, b, l, f) ds4_win_recv((s), (b), (l), (f))

static __inline int ds4_win_send(int s, const void *buf, size_t len, int flags)
{
    int r = send((SOCKET)s, (const char *)buf, (int)len, flags & ~MSG_NOSIGNAL);
    if (r < 0) errno = WSAGetLastError();
    return r;
}
#define send(s, b, l, f) ds4_win_send((s), (b), (l), (f))

static __inline int ds4_win_accept(int s, struct sockaddr *addr, socklen_t *len)
{
    SOCKET a = accept((SOCKET)s, addr, len);
    if (a == INVALID_SOCKET) { errno = WSAGetLastError(); return -1; }
    return (int)a;
}
#define accept(s, a, l) ds4_win_accept((s), (a), (l))

static __inline int ds4_win_connect(int s, const struct sockaddr *addr, socklen_t len)
{
    int r = connect((SOCKET)s, addr, len);
    if (r != 0) errno = WSAGetLastError();
    return r;
}
#define connect(s, a, l) ds4_win_connect((s), (a), (l))

/* setsockopt: ds4_distributed.c passes plain pointers (int / struct timeval).
 * Winsock wants `const char *`; cast through. SO_RCVTIMEO/SO_SNDTIMEO take a
 * DWORD milliseconds value on Windows rather than a struct timeval, so convert
 * those when the caller passes the POSIX-sized value. */
static __inline int ds4_win_setsockopt(int s, int level, int opt,
                                       const void *val, socklen_t len)
{
    int r;
    if (level == SOL_SOCKET && (opt == SO_RCVTIMEO || opt == SO_SNDTIMEO) &&
        len == (socklen_t)sizeof(struct timeval)) {
        const struct timeval *tv = (const struct timeval *)val;
        const DWORD ms = (DWORD)((long long)tv->tv_sec * 1000 + tv->tv_usec / 1000);
        r = setsockopt((SOCKET)s, level, opt, (const char *)&ms, (int)sizeof(ms));
    } else {
        r = setsockopt((SOCKET)s, level, opt, (const char *)val, len);
    }
    if (r != 0) errno = WSAGetLastError();
    return r;
}
#define setsockopt(s, lvl, o, v, l) ds4_win_setsockopt((s), (lvl), (o), (v), (l))

/* dup() of a socket: the CRT's _dup cannot duplicate a Winsock socket, so use
 * WSADuplicateSocket for the in-process equivalent of dup(2). Non-socket fds
 * (mkstemp staging, pipes) fall back to _dup. */
static __inline int ds4_win_dup(int fd)
{
    WSAPROTOCOL_INFOA info;
    if (WSADuplicateSocketA((SOCKET)fd, GetCurrentProcessId(), &info) == 0) {
        SOCKET d = WSASocketA(info.iAddressFamily, info.iSocketType,
                              info.iProtocol, &info, 0, 0);
        if (d != INVALID_SOCKET) return (int)d;
    }
    if (WSAGetLastError() == WSAENOTSOCK) return _dup(fd);
    errno = WSAGetLastError();
    return -1;
}
#ifndef dup
#define dup(fd) ds4_win_dup(fd)
#endif

/* ---- O_NONBLOCK + iovec/sendmsg/recvmsg ----------------------------------
 * Added for the port onto current main: main also links ds4_tp.c (the tensor-
 * parallel transport) into every binary, and it speaks the iovec/`sendmsg`
 * /`recvmsg` gate-traffic surface that neither the Windows CRT nor Winsock
 * provides. Same scope rules as above: enough for ds4-bench.exe to compile and
 * link; the TP serving path is not exercised by the bench. struct timeval
 * itself comes from <winsock2.h> (MSVC) or MinGW's <time.h>; struct pollfd
 * and the POLL* and MSG_DONTWAIT constants come from <winsock2.h>. */
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x20    /* flag value only: ds4_win.h makes fcntl a no-op */
#endif

#ifndef _DS4_IOVEC_DEFINED
#define _DS4_IOVEC_DEFINED
struct iovec {
    void  *iov_base;
    size_t iov_len;
};
struct msghdr {
    void         *msg_name;
    socklen_t     msg_namelen;
    struct iovec *msg_iov;
    int           msg_iovlen;
    void         *msg_control;
    socklen_t     msg_controllen;
    int           msg_flags;
};
#endif

static __inline ssize_t ds4_win_sendmsg(int s, const struct msghdr *msg, int flags)
{
    size_t sent = 0;
    for (int i = 0; i < msg->msg_iovlen; i++) {
        const char *p = (const char *)msg->msg_iov[i].iov_base;
        size_t left = msg->msg_iov[i].iov_len;
        while (left) {
            int n = ds4_win_send(s, p, left, flags);
            if (n < 0) return sent ? (ssize_t)sent : (ssize_t)-1;
            if (n == 0) return sent ? (ssize_t)sent : (ssize_t)-1;
            p += n; left -= (size_t)n; sent += (size_t)n;
        }
    }
    return (ssize_t)sent;
}
#define sendmsg(s, m, f) ds4_win_sendmsg((s), (m), (f))

static __inline ssize_t ds4_win_recvmsg(int s, struct msghdr *msg, int flags)
{
    size_t got = 0;
    for (int i = 0; i < msg->msg_iovlen; i++) {
        char *p = (char *)msg->msg_iov[i].iov_base;
        size_t left = msg->msg_iov[i].iov_len;
        while (left) {
            int n = ds4_win_recv(s, p, left, flags);
            if (n < 0) return got ? (ssize_t)got : (ssize_t)-1;
            if (n == 0) return (ssize_t)got;
            p += n; left -= (size_t)n; got += (size_t)n;
        }
    }
    return (ssize_t)got;
}
#define recvmsg(s, m, f) ds4_win_recvmsg((s), (m), (f))

#endif /* _WIN32 */
#endif /* DS4_SOCKETS_WIN_H */
