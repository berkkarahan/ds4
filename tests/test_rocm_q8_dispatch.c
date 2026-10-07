/* Exercise both sides of the gfx1151-only WMMA dispatch boundary. */
#include "ds4_gpu.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "ds4_win.h"
#endif

enum { IN_DIM = 64, OUT_DIM = 1024, MAX_TOKENS = 257, GUARD = 16 };

static void test_streaming_shared_projection(void) {
    enum { INPUTS = 2048, OUTPUTS = 4096, TOKENS = 8 };
    const size_t bytes = OUTPUTS * (INPUTS / 32) * 34, alignment = 65536;
    unsigned char *storage = malloc(bytes + 2 * alignment);
    assert(storage);
    unsigned char *weights = (unsigned char *)
        (((uintptr_t)storage + alignment - 1) & ~(uintptr_t)(alignment - 1));
    float *input = malloc(TOKENS * INPUTS * sizeof(float));
    float *actual = malloc(TOKENS * OUTPUTS * sizeof(float));
    assert(input && actual);
    /* Non-power-of-two scales/activations expose the rounding introduced by
     * optional FP16 expansion of this shared-expert projection. */
    const uint16_t scale_h = 0x2555;
    const float scale = 1365.0f / 65536.0f;
    for (size_t b = 0; b < bytes / 34; b++) {
        memcpy(weights + b * 34, &scale_h, 2);
        for (int k = 0; k < 32; k++)
            weights[b * 34 + 2 + k] = (unsigned char)(int8_t)((b * 7 + k * 11) % 127 - 63);
    }
    for (int i = 0; i < TOKENS * INPUTS; i++) input[i] = ((i * 13 % 61) - 30) * 0.0013f;
#ifdef _WIN32
    FILE *file = ds4_tmpfile();
#else
    FILE *file = tmpfile();
#endif
    assert(file && fwrite(weights, 1, bytes, file) == bytes && fflush(file) == 0);
    assert(ds4_gpu_init());
    ds4_gpu_set_ssd_streaming(true);
#ifdef _WIN32
    assert(ds4_gpu_set_model_fd_for_map(_fileno(file), weights));
#else
    assert(ds4_gpu_set_model_fd_for_map(fileno(file), weights));
#endif
    assert(ds4_gpu_set_model_map(weights, bytes));
    assert(ds4_gpu_set_model_map_range(weights, bytes, 0, bytes, bytes));
    ds4_gpu_tensor *x = ds4_gpu_tensor_alloc(TOKENS * INPUTS * sizeof(float));
    ds4_gpu_tensor *y = ds4_gpu_tensor_alloc(TOKENS * OUTPUTS * sizeof(float));
    assert(x && y && ds4_gpu_tensor_write(x, 0, input, TOKENS * INPUTS * sizeof(float)));
    assert(ds4_gpu_matmul_q8_0_tensor(y, weights, bytes, 0, INPUTS, OUTPUTS, x, TOKENS));
    assert(ds4_gpu_tensor_read(y, 0, actual, TOKENS * OUTPUTS * sizeof(float)));
    float max_error = 0;
    for (int t = 0; t < TOKENS; t++) for (int r = 0; r < OUTPUTS; r++) {
        float ref = 0;
        for (int k = 0; k < INPUTS; k++) {
            const size_t b = (size_t)r * (INPUTS / 32) + k / 32;
            ref += scale * (int8_t)weights[b * 34 + 2 + k % 32] * input[t * INPUTS + k];
        }
        assert(isfinite(actual[t * OUTPUTS + r]));
        float error = fabsf(actual[t * OUTPUTS + r] - ref);
        if (error > max_error) max_error = error;
        assert(error < 0.00002f);
    }
    printf("Q8 streaming shared projection: PASS (CPU reference max_abs=%.9g)\n", max_error);
    ds4_gpu_tensor_free(x); ds4_gpu_tensor_free(y);
    ds4_gpu_cleanup();
    assert(fclose(file) == 0);
    free(storage); free(input); free(actual);
}

int main(void) {
    const uint64_t weight_bytes = OUT_DIM * (IN_DIM / 32) * 34;
    /* Model ranges may be host-registered after rounding to whole pages.
     * Isolate the synthetic model from other heap allocations, just like a
     * GGUF mmap. A shared last page can partially register the input buffer
     * and make HIP reject its subsequent host-to-device copy. */
    const size_t alignment = 65536;
    unsigned char *storage = malloc((size_t)weight_bytes + 2 * alignment);
    assert(storage);
    unsigned char *weights = (unsigned char *)
        (((uintptr_t)storage + alignment - 1) & ~(uintptr_t)(alignment - 1));
    float *input = malloc(MAX_TOKENS * IN_DIM * sizeof(float));
    float *output = malloc((MAX_TOKENS * OUT_DIM + GUARD) * sizeof(float));
    assert(weights && input && output);
    for (uint64_t b = 0; b < weight_bytes / 34; b++) {
        const uint16_t scale = 0x3800; /* exactly 0.5 in fp16 */
        memcpy(weights + b * 34, &scale, 2);
        for (int k = 0; k < 32; k++)
            weights[b * 34 + 2 + k] = (unsigned char)(int8_t)((b * 7 + k * 11) % 15 - 7);
    }
    for (int i = 0; i < MAX_TOKENS * IN_DIM; i++) input[i] = ((i * 13 % 61) - 30) * 0.125f;
    /* Decode prequantizes each activation block. Choose an exact power-of-two
     * scale so this CPU reference covers both prequantized and F32 paths. */
    for (int i = 0; i < MAX_TOKENS * IN_DIM; i += 32) input[i] = 127 * 0.125f;
    assert(ds4_gpu_init());
    assert(ds4_gpu_set_model_map(weights, weight_bytes));
    assert(ds4_gpu_cache_model_range(weights, weight_bytes, 0, weight_bytes, "q8 test"));
    ds4_gpu_tensor *x = ds4_gpu_tensor_alloc(MAX_TOKENS * IN_DIM * sizeof(float));
    ds4_gpu_tensor *y = ds4_gpu_tensor_alloc((MAX_TOKENS * OUT_DIM + GUARD) * sizeof(float));
    assert(x && y && ds4_gpu_tensor_write(x, 0, input, MAX_TOKENS * IN_DIM * sizeof(float)));
    const uint32_t cases[] = {1, 5, 255, 256, 257};
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        uint32_t tokens = cases[c];
        const uint64_t n = (uint64_t)tokens * OUT_DIM;
        assert(ds4_gpu_tensor_fill_f32(y, NAN, n + GUARD));
        assert(ds4_gpu_matmul_q8_0_tensor(y, weights, weight_bytes, 0,
                                         IN_DIM, OUT_DIM, x, tokens));
        assert(ds4_gpu_synchronize());
        assert(ds4_gpu_tensor_read(y, 0, output, (n + GUARD) * sizeof(float)));
        for (uint32_t t = 0; t < tokens; t++) {
            for (uint32_t r = 0; r < OUT_DIM; r++) {
                float ref = 0;
                for (uint32_t k = 0; k < IN_DIM; k++) {
                    uint64_t b = (uint64_t)r * (IN_DIM / 32) + k / 32;
                    ref += 0.5f * (int8_t)weights[b * 34 + 2 + k % 32] * input[t * IN_DIM + k];
                }
                assert(isfinite(output[(uint64_t)t * OUT_DIM + r]));
                if (fabsf(output[(uint64_t)t * OUT_DIM + r] - ref) > 0.0002f) {
                    fprintf(stderr, "Q8 mismatch tokens=%u token=%u row=%u got=%.9g want=%.9g\n",
                            tokens, t, r, output[(uint64_t)t * OUT_DIM + r], ref);
                    return 1;
                }
            }
        }
        for (uint64_t i = n; i < n + GUARD; i++) assert(isnan(output[i]));
        printf("Q8 dispatch: PASS tokens=%u (CPU reference and output guards)\n", tokens);
    }
    ds4_gpu_tensor_free(x);
    ds4_gpu_tensor_free(y);
    ds4_gpu_cleanup();
    free(storage); free(input); free(output);
    test_streaming_shared_projection();
    return 0;
}
