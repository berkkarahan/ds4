# Windows ROCm validation — 2026-10-07

This is the first correctness-validation record. Follow-up cache, GPU usage,
and batched-prefill results are in [STREAMING_PERFORMANCE.md](STREAMING_PERFORMANCE.md).

## Result

**PASS for native Windows single-GPU DeepSeek V4 Flash Q2 inference with SSD
streaming.** Validation includes model integrity, actual prefill and generation,
snapshot restoration, Q8 numerical checks, both compiler ABIs, and large-file
I/O. It is not a validation of distributed serving or every supported model.

## Environment and model

- Windows 11 Home; 63.06 GiB visible RAM, approximately 40 GiB available.
- AMD Radeon RX 9070 XT, `gfx1201`, 15.92 GiB VRAM, HIP device 0.
- Radeon driver `32.0.31041.1004`; ROCm/HIP SDK 7.2.
- `HIP_VISIBLE_DEVICES=0`; other model/cache environment overrides unset for
  the final chat/snapshot test.
- AMD clang `21.0.0git`, MSVC target; MinGW GCC `16.2.0`.
- Repository branch: `bka/windows-rocm`, Windows work based on `ba29d00`
  plus the reviewed working-tree changes.
- Hub repository: `antirez/deepseek-v4-gguf`.
- Revision: `f71f23d552d664e523b422157b2befbf74040380`.
- File: `DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-0731.gguf`.
- Size: **86,720,111,488 bytes** (80.76 GiB).
- SHA-256: `ca22ae2f838e14077c22bc1c1417b71b45b5e5a3687bd96c2ac6e17fdb6261c0`.

`hf download ... --local-dir gguf` confirmed the existing official download.
`hf cache verify ... --local-dir gguf` succeeded and checked the one local model
file. Its missing-file notices refer to the other quantizations in the remote
repository; those are not needed for this test.

## Findings fixed in the final review

1. **Discrete GPU streaming was impossible with the UMA reserve.** The original
   16 GiB free-device reserve exceeded this card's entire VRAM, causing
   `streaming expert cache cannot reserve a 6.75 MiB slot` on the first layer.
   Discrete GPUs now default to a 2 GiB reserve. The integrated/UMA default
   remains 16 GiB; the existing diagnostic environment override is retained.
2. **Positional reads were not concurrency-safe.** Saving and restoring a shared
   synchronous handle's cursor races across readers. Reads now reopen an
   independent handle and preserve the Win32 read error before cleanup.
3. **Snapshot copy-back could ignore failures or truncate after a seek.** It now
   checks stream/flush/seek errors, measures the file's full length, reports
   overflow, and copies the complete payload. Registry exhaustion fails cleanly.
4. **Temporary-directory publication raced.** One-time initialization now
   publishes the complete path, and path formatting rejects truncation.
5. **Other ABI edge cases:** staged payload lengths remain 64-bit; `realpath`
   rejects nonexistent paths; close-on-exec clears handle inheritance; the clock
   shim implements real wall-clock time; thread joins check their wait result.
6. **Validation documentation overstated support.** Removed the incorrect BIOS
   UMA requirement for the RX 9070 XT and distinguished linked socket shims from
   validated distributed inference.

## Checks performed

| Check | Result |
| --- | --- |
| `make windows-rocm` | PASS: fresh compilation/link for gfx1201 |
| `make windows-cpu WIN_CPU_OUTPUT=win/build/cpu/ds4-bench.exe` | PASS; `--help` exits 0 |
| `make test-windows` | PASS under MinGW and MSVC-ABI clang |
| Concurrent snapshot initialization/copy-back, exhaustion and overrun | PASS |
| Sparse-file reads beyond 4 GiB, parallel cursor preservation, EOF | PASS |
| Native temp paths, file identity spelling, no-inherit, thread join/detach | PASS |
| Existing Linux memory parser test compiled and run on Windows | PASS |
| ROCm small pinned, managed and device allocations; oversized host refusal | PASS on actual GPU |
| Q8 CPU-reference oracle for 1/5/255/256/257 tokens, output guards | PASS |
| Full Q2 chat prompt, finite logits, generation, snapshot replay | PASS on final build, default reserve |
| `make -n`, shell syntax checks, `git diff --check` | PASS |

The Q8 oracle uses exactly representable activation scales so it checks both
the prequantized decode path and F32 batch path against the same CPU result.
It does not assert that arbitrary prequantized activations are bit-identical
to unquantized floating-point inference.

### Final-build chat and snapshot test

From Git Bash, with the SDK DLL directory on `PATH`:

```sh
export HIP_VISIBLE_DEVICES=0
unset DS4_ROCM_STREAM_FREE_RESERVE_GB
make windows-rocm
bash win/test-rocm.sh \
  gguf/DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-0731.gguf
```

The live test runs an 18-token complete chat prompt with thinking disabled:

> What is the capital of France? Reply with only the city name.

Observed output:

```text
Generated 1 tokens:  Paris
Snapshot replay: PASS (14523860 bytes, identical logits and generated tokens)
DeepSeek Flash Q2 Windows ROCm inference: PASS
```

The test checks all vocabulary logits for finiteness, saves the pre-generation
snapshot, generates through the stop token, restores the snapshot, compares
the complete logits byte-for-byte, and verifies the replayed token sequence.
Context allocation is 256; prefill chunk is 32; SSD expert preloading is off.

### Two-frontier benchmark

The diagnostic benchmark that established the reserve fix used
`DS4_ROCM_STREAM_FREE_RESERVE_GB=2`, equivalent to the final discrete-GPU default.
It prefills 16 tokens, generates 16, restores its snapshot, extends to 32 prompt
tokens, and generates another 16. Context allocation was 128, prefill chunk 32,
with `--ssd-streaming --ssd-streaming-cold --show-output`.

| Prompt frontier | Newly prefilled tokens | Prefill tokens/s | Generated tokens | Generation tokens/s |
| ---: | ---: | ---: | ---: | ---: |
| 16 | 16 | 0.38 | 16 | 0.47 |
| 32 | 16 | 0.41 | 16 | 0.44 |

Generated continuations included:

```text
 of Germany is Berlin. Berlin is a city in Europe. The capital of Italy
 is two. Two plus two is four. The capital of Spain is Madrid.
```

The input was a raw passage beginning "The capital of France is Paris. Paris is
a city in Europe. The capital of Japan is Tokyo...", sliced at each frontier.
These are short cold-streaming measurements, not peak throughput claims. The
automatic cache held 176 experts (1.16 GiB dynamic), below the per-token routed
working set, so repeated SSD reads dominated performance.

## Reproduction artifacts and limits

Local logs are under the ignored `win/build/validation/` directory:

- `build-rocm-final.log`, `build-cpu-final.log`
- `test-rocm-final.log` (includes the full chat/snapshot test)
- `test-rocm-launcher-single-gpu.log` (SDK DLL path resolved by the launcher)
- `test-rocm-launcher-default.log`, `q8-all-visible.log` (final isolated fixture)
- `q2-reserve.csv`, `q2-reserve.stderr.log`, `prompt.txt`
- `default-make.txt`

The regression sources and `win/test-rocm.sh` are checked-in source candidates;
the logs and downloaded weights are not source artifacts.

Not exercised: long-context soak tests, full-model CPU/GPU logits parity,
gfx1151 hardware, Linux ROCm, CUDA, Metal, distributed/TP serving, the Windows
CLI/server, or the machine's secondary gfx1036 GPU. Socket-shim gaps remain
documented in `win/README.md`. No BIOS settings, driver installation, or
system-wide environment settings were changed.

A follow-up synthetic test exposed a fixture issue: heap-backed model weights
could share a page with the activation buffer. Model registration rounds to
pages, partially registering that buffer and causing HIP tensor-copy errors.
The fixture now isolates its model on dedicated aligned pages, as a real GGUF
mapping does. It passes both with the launcher's default single-device mask and
with both adapters visible. The earlier suspicion of a visibility-dependent
runtime failure was not borne out. Full-model inference remains validated only
on device 0.
