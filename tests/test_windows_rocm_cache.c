/* Reinstalling hot model spans near the cache cap must not reread the GGUF. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ds4_win.h"
#include <winioctl.h>
#include "ds4_gpu.h"

int main(void) {
    enum { INPUTS = 64, OUTPUTS = 32 };
    const uint64_t bytes = 768ull << 20;
    const uint64_t hot_bytes = 640ull << 20;
    assert(_putenv_s("DS4_ROCM_STREAM_MODEL_CACHE_GB", "1") == 0);
    char path[PATH_MAX];
    assert(ds4_win_temp_path(path, sizeof(path), "ds4-hot-cache-XXXXXX") == 0);
    const int fd = mkstemp(path);
    assert(fd >= 0);
    DWORD ignored;
    assert(DeviceIoControl((HANDLE)_get_osfhandle(fd), FSCTL_SET_SPARSE,
                           NULL, 0, NULL, 0, &ignored, NULL));
    assert(_chsize_s(fd, bytes) == 0);
    unsigned char weights[OUTPUTS * (INPUTS / 32) * 34];
    for (int row = 0; row < OUTPUTS; row++) {
        for (int b = 0; b < INPUTS / 32; b++) {
            unsigned char *block = weights + (row * (INPUTS / 32) + b) * 34;
            const uint16_t scale = 0x3400; /* 0.25 */
            memcpy(block, &scale, sizeof(scale));
            memset(block + 2, (unsigned char)(int8_t)(row - 16), 32);
        }
    }
    assert(_lseeki64(fd, 0, SEEK_SET) == 0);
    assert(_write(fd, weights, sizeof(weights)) == sizeof(weights));
    void *model = mmap(NULL, (size_t)bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    assert(model != MAP_FAILED);
    assert(ds4_gpu_init());
    ds4_gpu_set_quality(true);
    ds4_gpu_set_ssd_streaming(true);
    assert(ds4_gpu_set_model_fd_for_map(fd, model));
    assert(ds4_gpu_set_model_map_range(model, bytes, 0, hot_bytes, hot_bytes));

    IO_COUNTERS before, after;
    assert(GetProcessIoCounters(GetCurrentProcess(), &before));
    assert(ds4_gpu_set_model_map_range(model, bytes, 0, hot_bytes, hot_bytes));
    const uint64_t offsets[] = {0, hot_bytes / 2};
    const uint64_t sizes[] = {hot_bytes / 2, hot_bytes / 2};
    assert(ds4_gpu_set_model_map_spans(model, bytes, offsets, sizes, 2, hot_bytes / 2));
    assert(GetProcessIoCounters(GetCurrentProcess(), &after));
    /* Leave room for unrelated runtime bookkeeping, but not a 640 MiB reload. */
    const uint64_t reread = after.ReadTransferCount - before.ReadTransferCount;
    assert(reread < (1ull << 20));

    /* A mixed request must fetch only the new span, not evict its cached part. */
    const uint64_t new_bytes = 64ull << 20;
    const uint64_t mixed_offsets[] = {0, hot_bytes};
    const uint64_t mixed_sizes[] = {hot_bytes, new_bytes};
    assert(GetProcessIoCounters(GetCurrentProcess(), &before));
    assert(ds4_gpu_set_model_map_spans(model, bytes, mixed_offsets, mixed_sizes, 2, hot_bytes));
    assert(GetProcessIoCounters(GetCurrentProcess(), &after));
    const uint64_t mixed_read = after.ReadTransferCount - before.ReadTransferCount;
    assert(mixed_read >= new_bytes && mixed_read < new_bytes + (1ull << 20));

    float input[INPUTS], output[OUTPUTS];
    for (int i = 0; i < INPUTS; i++) input[i] = 1.0f;
    ds4_gpu_tensor *x = ds4_gpu_tensor_alloc(sizeof(input));
    ds4_gpu_tensor *y = ds4_gpu_tensor_alloc(sizeof(output));
    assert(x && y && ds4_gpu_tensor_write(x, 0, input, sizeof(input)));
    assert(ds4_gpu_matmul_q8_0_tensor(y, model, bytes, 0, INPUTS, OUTPUTS, x, 1));
    assert(ds4_gpu_tensor_read(y, 0, output, sizeof(output)));
    for (int i = 0; i < OUTPUTS; i++) assert(output[i] == 16.0f * (i - 16));
    ds4_gpu_tensor_free(x);
    ds4_gpu_tensor_free(y);
    ds4_gpu_cleanup();
    assert(munmap(model, (size_t)bytes) == 0);
    assert(_close(fd) == 0 && _unlink(path) == 0);
    printf("ROCm hot range/span cache: PASS (reread=%llu, mixed_read=%llu bytes, weights intact)\n",
           (unsigned long long)reread, (unsigned long long)mixed_read);
    return 0;
}
