/* SSD-selected IQ2/Q2 prefill must preserve resident hot/cold-expert arithmetic. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ds4_win.h"
#include "ds4_gpu.h"

enum { DIM = 512, EXPERTS = 32, SELECTED = 6, LAYERS = 2, ROWS = 257, GUARD = 16 };
static uint32_t rng = 17;
static unsigned char random_byte(void) {
    rng = rng * 1664525u + 1013904223u;
    return (unsigned char)(rng >> 24);
}
static ds4_gpu_tensor *tensor(size_t bytes, const void *data) {
    ds4_gpu_tensor *t = ds4_gpu_tensor_alloc(bytes);
    assert(t && (!data || ds4_gpu_tensor_write(t, 0, data, bytes)));
    return t;
}

int main(void) {
    const uint64_t gate_row = (DIM / 256) * 66, down_row = (DIM / 256) * 84;
    const uint64_t gate_bytes = DIM * gate_row, down_bytes = DIM * down_row;
    const uint64_t layer_bytes = EXPERTS * (2 * gate_bytes + down_bytes);
    const size_t bytes = (size_t)(LAYERS * layer_bytes);
    unsigned char *storage = malloc(bytes);
    assert(storage);
    for (unsigned l = 0; l < LAYERS; l++) for (unsigned part = 0; part < 3; part++) {
        const size_t unit = part == 2 ? 84 : 66;
        const size_t size = EXPERTS * (part == 2 ? down_bytes : gate_bytes);
        unsigned char *p = storage + l * layer_bytes + part * EXPERTS * gate_bytes;
        for (size_t i = 0; i < size; i++) p[i] = random_byte();
        for (size_t i = 0; i < size; i += unit) {
            const size_t d = i + (part == 2 ? 80 : 0);
            p[d] = 0; p[d + 1] = 0x14;
            if (part == 2) { p[d + 2] = 0; p[d + 3] = 0x10; }
        }
    }
    char path[PATH_MAX];
    assert(ds4_win_temp_path(path, sizeof(path), "ds4-prefill-XXXXXX") == 0);
    int fd = mkstemp(path);
    assert(fd >= 0 && _write(fd, storage, (unsigned)bytes) == bytes);
    free(storage);
    void *model = mmap(NULL, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    assert(model != MAP_FAILED && ds4_gpu_init());
    assert(ds4_gpu_set_model_fd_for_map(fd, model));
    assert(ds4_gpu_set_model_map(model, bytes));
    float *input = malloc(ROWS * DIM * sizeof(float));
    float *weights = malloc(ROWS * SELECTED * sizeof(float));
    int32_t *ids = malloc(ROWS * SELECTED * sizeof(int32_t));
    float *ref = malloc(LAYERS * ROWS * DIM * sizeof(float));
    float *actual = malloc((ROWS * DIM + GUARD) * sizeof(float));
    assert(input && weights && ids && ref && actual);
    for (unsigned i = 0; i < ROWS * DIM; i++) input[i] = ((int)random_byte() - 128) / 512.0f;
    for (unsigned i = 0; i < ROWS * SELECTED; i++) weights[i] = 0.05f + random_byte() / 1024.0f;
    ds4_gpu_tensor *x = tensor(ROWS * DIM * 4, input), *sw = tensor(ROWS * SELECTED * 4, weights);
    ds4_gpu_tensor *si = tensor(ROWS * SELECTED * 4, NULL);
    ds4_gpu_tensor *out = tensor((ROWS * DIM + GUARD) * 4, NULL);
    ds4_gpu_tensor *gate = tensor(ROWS * SELECTED * DIM * 4, NULL);
    ds4_gpu_tensor *up = tensor(ROWS * SELECTED * DIM * 4, NULL);
    ds4_gpu_tensor *mid = tensor(ROWS * SELECTED * DIM * 4, NULL);
    ds4_gpu_tensor *down = tensor(ROWS * SELECTED * DIM * 4, NULL);
    const unsigned counts[] = {2, 7, 8, 32, 128, 257, 32};
    ds4_gpu_set_streaming_expert_cache_budget(24);
    for (unsigned quality = 0; quality <= 1; quality++) {
        ds4_gpu_set_quality(quality != 0);
        for (unsigned c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
            const unsigned rows = counts[c];
            /* Four frequent experts and two rotating slots cross the WMMA
             * hot threshold. Change IDs each chunk to catch stale remaps. */
            for (unsigned r = 0; r < rows; r++) for (unsigned s = 0; s < SELECTED; s++) {
                ids[r * SELECTED + s] = s < 4 ? (s * 7 + c * 3) % 16 :
                    16 + (r * 2 + s + c * 5) % 16;
            }
            assert(ds4_gpu_tensor_write(si, 0, ids, rows * SELECTED * 4));
            for (unsigned pass = 0; pass < 3; pass++) {
                if (pass < 2) ds4_gpu_set_ssd_streaming(pass != 0);
                for (unsigned k = 0; k < LAYERS; k++) {
                    const unsigned l = pass == 2 ? LAYERS - 1 - k : k;
                    const uint64_t g = l * layer_bytes, u = g + EXPERTS * gate_bytes;
                    const uint64_t d = u + EXPERTS * gate_bytes;
                    const ds4_gpu_stream_expert_table table = {
                        model, bytes, l, EXPERTS, g, u, d, gate_bytes, down_bytes};
                    /* Exercise pending uploads and partially resident batches. */
                    if (pass == 2) assert(ds4_gpu_stream_expert_cache_prepare_selected_batch(&table, ids, rows, SELECTED));
                    assert(ds4_gpu_tensor_fill_f32(out, NAN, rows * DIM + GUARD));
                    bool half = false;
                    const int ok = ds4_gpu_routed_moe_batch_tensor(out, gate, up, mid, down,
                        model, bytes, g, u, d, 16, 10, gate_bytes, gate_row, down_bytes, down_row,
                        DIM, DIM, DIM, si, sw, EXPERTS, SELECTED, 10, x, l, rows, &half, pass == 0);
                    if (!ok) {
                        fprintf(stderr, "prefill failed quality=%u rows=%u pass=%u layer=%u\n", quality, rows, pass, l);
                        return 1;
                    }
                    assert(!half && ds4_gpu_tensor_read(out, 0, actual, (rows * DIM + GUARD) * 4));
                    for (unsigned i = 0; i < rows * DIM; i++) {
                        assert(isfinite(actual[i]));
                        if (pass == 0) ref[l * ROWS * DIM + i] = actual[i];
                        else if (actual[i] != ref[l * ROWS * DIM + i]) {
                            fprintf(stderr, "prefill mismatch quality=%u rows=%u pass=%u layer=%u i=%u got=%.9g ref=%.9g\n",
                                quality, rows, pass, l, i, actual[i], ref[l * ROWS * DIM + i]);
                            return 1;
                        }
                    }
                    for (unsigned i = rows * DIM; i < rows * DIM + GUARD; i++) assert(isnan(actual[i]));
                }
            }
            printf("ROCm SSD prefill: PASS quality=%u rows=%u (exact resident parity, remap, reuse, output guards)\n", quality, rows);
        }
    }
    ds4_gpu_tensor_free(x); ds4_gpu_tensor_free(sw); ds4_gpu_tensor_free(si);
    ds4_gpu_tensor_free(out); ds4_gpu_tensor_free(gate); ds4_gpu_tensor_free(up);
    ds4_gpu_tensor_free(mid); ds4_gpu_tensor_free(down);
    ds4_gpu_cleanup();
    free(input); free(weights); free(ids); free(ref); free(actual);
    assert(munmap(model, bytes) == 0 && _close(fd) == 0 && _unlink(path) == 0);
    return 0;
}
