/* Native Windows file/snapshot/thread regressions; no model or GPU needed. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "ds4_win.h"
#include <winioctl.h>
#ifdef DS4_WIN_PTHREAD
#include "win/ds4_pthread_win.h"
#endif

static DWORD WINAPI snapshots(void *unused) {
    (void)unused;
    for (int i = 0; i < 32; i++) {
        char path[PATH_MAX], data[16] = {0};
        assert(ds4_win_temp_path(path, sizeof(path), "ds4-test") == 0);
        assert(strstr(path, "ds4-test"));
        FILE *fp = fmemopen(data, sizeof(data), "wb");
        assert(fp && fwrite("snapshot", 1, 8, fp) == 8);
        /* Rewinding before close must not discard the existing high watermark. */
        assert(_fseeki64(fp, 0, SEEK_SET) == 0);
        assert(fwrite("S", 1, 1, fp) == 1);
        assert(fclose(fp) == 0 && !strcmp(data, "Snapshot"));
        fp = fmemopen(data, 8, "rb");
        char copy[8];
        assert(fp && fread(copy, 1, 8, fp) == 8);
        assert(!memcmp(data, copy, 8) && fclose(fp) == 0);
    }
    return 0;
}

typedef struct { int fd; int64_t offset; } read_job;
static DWORD WINAPI positional_reader(void *arg) {
    const read_job *job = arg;
    for (int i = 0; i < 32; i++) {
        char buf[16] = {0};
        assert(pread(job->fd, buf, 6, job->offset) == 6);
        assert(!memcmp(buf, "abcdef", 6));
    }
    return 0;
}

static void join_handles(HANDLE *threads, DWORD n) {
    assert(WaitForMultipleObjects(n, threads, TRUE, 30000) == WAIT_OBJECT_0);
    for (DWORD i = 0; i < n; i++) assert(CloseHandle(threads[i]));
}

static void test_streams(void) {
    HANDLE threads[8];
    for (int i = 0; i < 8; i++) {
        threads[i] = CreateThread(NULL, 0, snapshots, NULL, 0, NULL);
        assert(threads[i]);
    }
    join_handles(threads, 8);
    char data[17][8] = {{0}};
    FILE *streams[16];
    for (int i = 0; i < 16; i++) {
        streams[i] = fmemopen(data[i], 8, "wb");
        assert(streams[i]);
    }
    assert(fmemopen(data[16], 8, "wb") == NULL && errno == EMFILE);
    for (int i = 0; i < 16; i++) assert(fclose(streams[i]) == 0);
    FILE *fp = fmemopen(data[0], 2, "wb");
    assert(fp && fwrite("toolong", 1, 7, fp) == 7);
    assert(fclose(fp) == EOF && errno == ENOSPC);
}

static void test_files(void) {
    char path[PATH_MAX];
    assert(ds4_win_temp_path(path, sizeof(path), "ds4-compat-XXXXXX") == 0);
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(_setmode(fd, _O_BINARY) != -1);
    DWORD got;
    assert(DeviceIoControl((HANDLE)_get_osfhandle(fd), FSCTL_SET_SPARSE,
                           NULL, 0, NULL, 0, &got, NULL));
    const int64_t large = (INT64_C(1) << 32) + 123;
    assert(_write(fd, "abcdef", 6) == 6);
    assert(_lseeki64(fd, large, SEEK_SET) == large);
    assert(_write(fd, "abcdef", 6) == 6);
    assert(_lseeki64(fd, 2, SEEK_SET) == 2);
    HANDLE threads[8];
    read_job jobs[8];
    for (int i = 0; i < 8; i++) {
        jobs[i].fd = fd;
        jobs[i].offset = i % 2 ? large : 0;
        threads[i] = CreateThread(NULL, 0, positional_reader, &jobs[i], 0, NULL);
        assert(threads[i]);
    }
    join_handles(threads, 8);
    assert(_telli64(fd) == 2);
    char buf[16];
    assert(pread(fd, buf, sizeof(buf), large) == 6);
    assert(pread(fd, buf, sizeof(buf), large + 6) == 0);
    assert(pread(fd, buf, 1, -1) == -1 && errno == EINVAL);
    struct stat st;
    assert(fstat(fd, &st) == 0 && st.st_size == large + 6);
    assert(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
    DWORD flags = 0;
    assert(GetHandleInformation((HANDLE)_get_osfhandle(fd), &flags));
    assert(!(flags & HANDLE_FLAG_INHERIT));
    char *full = realpath(path, NULL);
    assert(full && strlen(full) > 3 && full[1] == ':');
    free(full);
    assert(_close(fd) == 0 && _unlink(path) == 0);
    assert(realpath(path, NULL) == NULL);
}

#ifdef DS4_WIN_PTHREAD
static void *thread_result(void *p) {
    assert(pthread_equal(pthread_self(), pthread_self()));
    return p;
}
static void test_pthreads(void) {
    for (int i = 0; i < 128; i++) {
        pthread_t t;
        void *result = NULL;
        assert(pthread_create(&t, NULL, thread_result, (void *)(uintptr_t)17) == 0);
        assert(!pthread_equal(t, pthread_self()));
        assert(pthread_join(t, &result) == 0 && (uintptr_t)result == 17);
        assert(pthread_create(&t, NULL, thread_result, NULL) == 0);
        assert(pthread_detach(t) == 0);
    }
    struct timespec ts;
    assert(clock_gettime(CLOCK_REALTIME, &ts) == 0);
    assert(llabs((long long)ts.tv_sec - (long long)time(NULL)) <= 2);
}
#endif

int main(void) {
    test_streams();
    test_files();
#ifdef DS4_WIN_PTHREAD
    test_pthreads();
#endif
    puts("Windows compatibility: PASS (concurrency, >4 GiB offsets, snapshots, paths)");
    return 0;
}
