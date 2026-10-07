/* Live Q2 validation: full chat prompt, finite logits, generation, snapshot replay.
 * Invoke separately from the lightweight tests; it opens the real GGUF once. */
#include "ds4.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void checked(int rc, const char *operation, const char *error) {
    if (rc != 0) {
        fprintf(stderr, "%s failed: %s\n", operation, error);
        exit(1);
    }
}

static int generate(ds4_engine *engine, ds4_session *session, int *tokens,
                    char *text, size_t capacity) {
    char error[256] = {0};
    int count = 0;
    size_t used = 0;
    text[0] = '\0';
    for (int i = 0; i < 16; i++) {
        const int token = ds4_session_argmax(session);
        assert(token >= 0);
        if (ds4_token_is_stop(engine, token)) break;
        tokens[count++] = token;
        size_t len = 0;
        char *piece = ds4_token_text(engine, token, &len);
        assert(piece && used + len < capacity);
        memcpy(text + used, piece, len);
        used += len;
        text[used] = '\0';
        free(piece);
        checked(ds4_session_eval(session, token, error, sizeof(error)), "decode", error);
    }
    return count;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s MODEL.gguf\n", argv[0]);
        return 2;
    }
    ds4_engine_options options = {
        .model_path = argv[1],
        .backend = DS4_BACKEND_CUDA, /* shared GPU API; this test links ROCm */
        .context_size = 256,
        .prefill_chunk = 32,
        .ssd_streaming = true,
        .ssd_streaming_cold = true,
        .power_percent = 100,
    };
    ds4_engine *engine = NULL;
    checked(ds4_engine_open(&engine, &options), "engine open", "see backend log");
    ds4_tokens prompt = {0};
    ds4_encode_chat_prompt(engine, NULL,
        "Please answer the following geography question using only the city name. "
        "Do not include an explanation, punctuation, or any other information. "
        "What is the capital of France?", DS4_THINK_NONE, &prompt);
    /* Cross the short decode-style prefill threshold and exercise batched
     * layer preparation, including file offsets beyond MSVC's 32-bit off_t. */
    assert(prompt.len > 32);
    printf("Chat prompt: %d tokens\n", prompt.len);
    fflush(stdout);
    ds4_session *session = NULL;
    checked(ds4_session_create(&session, engine, 256), "session create", "see backend log");
    char error[256] = {0};
    checked(ds4_session_sync(session, &prompt, error, sizeof(error)), "prefill", error);
    const int vocab = ds4_engine_vocab_size(engine);
    float *before = malloc((size_t)vocab * sizeof(float));
    float *restored = malloc((size_t)vocab * sizeof(float));
    assert(before && restored);
    assert(ds4_session_copy_logits(session, before, vocab) == vocab);
    for (int i = 0; i < vocab; i++) assert(isfinite(before[i]));
    ds4_session_snapshot snapshot = {0};
    checked(ds4_session_save_snapshot(session, &snapshot, error, sizeof(error)), "snapshot save", error);
    int first[16], replay[16];
    char text[2048], replay_text[2048];
    const int count = generate(engine, session, first, text, sizeof(text));
    printf("Generated %d tokens: %s\n", count, text);
    fflush(stdout);
    assert(count > 0 && strstr(text, "Paris"));
    checked(ds4_session_load_snapshot(session, &snapshot, error, sizeof(error)), "snapshot restore", error);
    assert(ds4_session_copy_logits(session, restored, vocab) == vocab);
    assert(memcmp(before, restored, (size_t)vocab * sizeof(float)) == 0);
    const int replay_count = generate(engine, session, replay, replay_text, sizeof(replay_text));
    assert(replay_count == count && memcmp(first, replay, (size_t)count * sizeof(int)) == 0);
    printf("Snapshot replay: PASS (%llu bytes, identical logits and generated tokens)\n",
           (unsigned long long)snapshot.len);
    ds4_session_snapshot_free(&snapshot);
    free(before); free(restored);
    ds4_session_free(session);
    ds4_tokens_free(&prompt);
    ds4_engine_close(engine);
    puts("DeepSeek Flash Q2 Windows ROCm inference: PASS");
    return 0;
}
