/* Single-token streaming selected loads must stay byte-identical to resident
 * execution while the host RAM replay cache serves hits and evicts. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ds4_win.h"
#include "ds4_gpu.h"

enum {
    DIM = 512,
    EXPERTS = 32,
    SELECTED = 6,
    LAYERS = 2,
    GUARD = 16,
    CASE_COUNT = 10,
};
static uint32_t rng = 23;
static unsigned char random_byte(void) {
    rng = rng * 1664525u + 1013904223u;
    return (unsigned char)(rng >> 24);
}
static ds4_gpu_tensor *tensor(size_t bytes, const void *data) {
    ds4_gpu_tensor *t = ds4_gpu_tensor_alloc(bytes);
    assert(t && (!data || ds4_gpu_tensor_write(t, 0, data, bytes)));
    return t;
}

typedef struct {
    uint32_t layer;
    int32_t ids[SELECTED];
} ram_case;

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
    assert(ds4_win_temp_path(path, sizeof(path), "ds4-ramcache-XXXXXX") == 0);
    int fd = mkstemp(path);
    assert(fd >= 0 && _write(fd, storage, (unsigned)bytes) == bytes);
    free(storage);
    void *model = mmap(NULL, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    assert(model != MAP_FAILED && ds4_gpu_init());
    assert(ds4_gpu_set_model_fd_for_map(fd, model));
    assert(ds4_gpu_set_model_map(model, bytes));

    /*
     * Case order: warm sets repeat immediately across both layers, then a
     * rotating tail forces eviction and re-request.  The VRAM budget is one
     * layer's selected set, so every repeat must come from the host-RAM tier
     * (or from disk when it is disabled); byte equality is what matters.
     */
    ram_case cases[CASE_COUNT];
    unsigned n = 0;
    const int32_t warm_ids[SELECTED] = {0, 3, 6, 9, 12, 15};
    for (unsigned rep = 0; rep < 2; rep++) {
        for (uint32_t l = 0; l < LAYERS; l++) {
            cases[n].layer = l;
            memcpy(cases[n].ids, warm_ids, sizeof(warm_ids));
            n++;
        }
    }
    for (unsigned rot = 0; rot < 6; rot++) {
        cases[n].layer = rot & 1u;
        for (unsigned s = 0; s < SELECTED; s++) {
            cases[n].ids[s] = (int32_t)((rot * 5u + s * 3u + 1u) % EXPERTS);
        }
        n++;
    }
    assert(n == CASE_COUNT);

    float *input = malloc(DIM * sizeof(float));
    float *weights = malloc(SELECTED * sizeof(float));
    float *ref = malloc((size_t)CASE_COUNT * DIM * sizeof(float));
    float *actual = malloc((DIM + GUARD) * sizeof(float));
    assert(input && weights && ref && actual);
    for (unsigned i = 0; i < DIM; i++) input[i] = ((int)random_byte() - 128) / 512.0f;
    for (unsigned i = 0; i < SELECTED; i++) weights[i] = 0.05f + random_byte() / 1024.0f;
    ds4_gpu_tensor *x = tensor(DIM * 4, input), *sw = tensor(SELECTED * 4, weights);
    ds4_gpu_tensor *si = tensor(SELECTED * 4, NULL);
    ds4_gpu_tensor *out = tensor((DIM + GUARD) * 4, NULL);
    ds4_gpu_tensor *gate = tensor(SELECTED * DIM * 4, NULL);
    ds4_gpu_tensor *up = tensor(SELECTED * DIM * 4, NULL);
    ds4_gpu_tensor *mid = tensor(SELECTED * DIM * 4, NULL);
    ds4_gpu_tensor *down = tensor(SELECTED * DIM * 4, NULL);

    ds4_gpu_set_streaming_expert_cache_budget(SELECTED);
    ds4_gpu_set_ssd_streaming(false);

    for (unsigned c = 0; c < CASE_COUNT; c++) {
        const uint32_t l = cases[c].layer;
        const uint64_t g = l * layer_bytes, u = g + EXPERTS * gate_bytes;
        const uint64_t d = u + EXPERTS * gate_bytes;
        assert(ds4_gpu_tensor_write(si, 0, cases[c].ids, SELECTED * 4));
        assert(ds4_gpu_tensor_fill_f32(out, NAN, DIM + GUARD));
        assert(ds4_gpu_routed_moe_one_tensor(out, gate, up, mid, down,
            model, bytes, g, u, d, 16, 10, gate_bytes, gate_row, down_bytes, down_row,
            DIM, DIM, DIM, si, sw, EXPERTS, SELECTED, 10.0f, x, NULL, l, true));
        assert(ds4_gpu_tensor_read(out, 0, actual, (DIM + GUARD) * 4));
        for (unsigned i = 0; i < DIM; i++) {
            assert(isfinite(actual[i]));
            ref[(size_t)c * DIM + i] = actual[i];
        }
        for (unsigned i = DIM; i < DIM + GUARD; i++) assert(isnan(actual[i]));
    }

    ds4_gpu_set_ssd_streaming(true);
    for (unsigned c = 0; c < CASE_COUNT; c++) {
        const uint32_t l = cases[c].layer;
        const uint64_t g = l * layer_bytes, u = g + EXPERTS * gate_bytes;
        const uint64_t d = u + EXPERTS * gate_bytes;
        const ds4_gpu_stream_expert_table table = {
            model, bytes, l, EXPERTS, g, u, d, gate_bytes, down_bytes};
        assert(ds4_gpu_tensor_write(si, 0, cases[c].ids, SELECTED * 4));
        assert(ds4_gpu_routed_moe_set_selected_override(cases[c].ids, SELECTED));
        assert(ds4_gpu_stream_expert_cache_begin_selected_load(&table, cases[c].ids, SELECTED));
        assert(ds4_gpu_tensor_fill_f32(out, NAN, DIM + GUARD));
        const int ok = ds4_gpu_routed_moe_one_tensor(out, gate, up, mid, down,
            model, bytes, g, u, d, 16, 10, gate_bytes, gate_row, down_bytes, down_row,
            DIM, DIM, DIM, si, sw, EXPERTS, SELECTED, 10.0f, x, NULL, l, false);
        if (!ok) {
            fprintf(stderr, "RAM cache decode failed case=%u layer=%u\n", c, l);
            return 1;
        }
        assert(ds4_gpu_tensor_read(out, 0, actual, (DIM + GUARD) * 4));
        for (unsigned i = 0; i < DIM; i++) {
            assert(isfinite(actual[i]));
            if (actual[i] != ref[(size_t)c * DIM + i]) {
                fprintf(stderr,
                        "RAM cache mismatch case=%u layer=%u i=%u got=%.9g ref=%.9g\n",
                        c, l, i, actual[i], ref[(size_t)c * DIM + i]);
                return 1;
            }
        }
        for (unsigned i = DIM; i < DIM + GUARD; i++) assert(isnan(actual[i]));
    }

    printf("ROCm host RAM cache: PASS cases=%u (exact resident parity, eviction, re-request)\n",
           CASE_COUNT);
    ds4_gpu_tensor_free(x); ds4_gpu_tensor_free(sw); ds4_gpu_tensor_free(si);
    ds4_gpu_tensor_free(out); ds4_gpu_tensor_free(gate); ds4_gpu_tensor_free(up);
    ds4_gpu_tensor_free(mid); ds4_gpu_tensor_free(down);
    ds4_gpu_cleanup();
    free(input); free(weights); free(ref); free(actual);
    assert(munmap(model, bytes) == 0 && _close(fd) == 0 && _unlink(path) == 0);
    return 0;
}
