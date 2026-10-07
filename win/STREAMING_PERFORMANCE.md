# RX 9070 XT + SSD streaming

Measured 2026-10-07 on the Windows 11 / ROCm 7.2 setup in
[VALIDATION.md](VALIDATION.md), using the same official 80.76 GiB Flash Q2 GGUF.
The model is on a Kingston NV3 NVMe SSD (D:), with 64 GB system RAM and a
15.92 GiB RX 9070 XT. GPU inference runs on HIP device 0.

## Second iteration: cache reuse

### What was limiting GPU use

The GPU was already doing the computation. The main problem was repeatedly
discarding and transferring weights:

- The default fixed-weight cache was **8 GiB**, below the model's **8.20 GiB**
  non-routed working set. Cycling through layers repeatedly flushed the cache.
- The automatic expert budget held **176 experts**, while one token selects
  **258 experts across 43 layers**. The measured expert-cache hit rate was 0%.
- Batched prefill reused a GLM-only capability predicate. DeepSeek therefore
  mapped complete expert tables in addition to preparing selected experts.
- The shared Windows prefill preparation path still cast file positions through
  MSVC's 32-bit `off_t`. Longer prompts failed after reaching later layers.

## Changes

1. Discrete GPUs now allow two-thirds of VRAM for the fixed-weight cache
   (**about 10.61 GiB** here), keeping the existing min/max clamps. This is a cap,
   not an eager allocation; the existing integrated/UMA split is retained.
2. Cache admission charges only missing spans. Re-requesting cached ranges
   near the cap no longer evicts and rereads those same ranges.
3. Prefill preparation and n-gram reads preserve 64-bit offsets before entering
   the Windows read shim.
4. `win/profile-streaming.ps1` records benchmark output, expert/read statistics,
   process-specific WDDM GPU activity and memory, process reads, and physical
   disk reads. Environment changes are process-local and restored afterward.

## Measurements

Same raw prompt, 16 prompt tokens, 16 generated tokens, context allocation 256,
prefill chunk 32, SSD streaming, expert preloading off:

| Configuration | Prefill tokens/s | Generation tokens/s | Sampled compute-engine mean | Expert hits |
| --- | ---: | ---: | ---: | ---: |
| Previous defaults, matched reference | 0.50 | 0.50 | 4.5% | 0% |
| 10 GiB fixed cache, automatic experts | 2.73 | 3.62 | 15.7% | 0% |
| New fixed-cache default, 512 experts | 3.04 | **4.71** | 27.2% | **47.8%** |
| 10 GiB fixed cache, 768 experts | 2.70 | 4.62 | 23.6% | 55.3% |

The matched short run improved generation by **9.4x**. The 512-expert run
transferred 28.40 GiB of routed weights over the whole run, versus 54.42 GiB
with automatic experts. Generated text matched, and the complete frontier
logits JSON was byte-identical:

`547dcca147028049768ae3a130e483e45829684350b7fa953b6467e79d2f8133`

Increasing to 768 experts used more memory without a meaningful speed gain in
this sample, so 512 is the recommended starting point for this configuration.

### Batched-prefill experiment: reverted

Enabling selected-expert-only mapping for DeepSeek batched prefill completed
64 prompt + 64 generated tokens at 3.37 tokens/s prefill and 3.54 tokens/s
generation. However, comparison with the original batched path found a maximum
absolute logit difference of **1.634**, with mean absolute difference **0.234**.
The argmax matched, but that is insufficient for numerical parity.

The initial arithmetic diagnosis was incomplete: the resident IQ2/Q2 path uses
hot-expert WMMA kernels and FP16 intermediates/output, whereas the selected
pointer-table path uses different scalar kernels. Optional Q8-to-FP16 expansion
also makes shared-expert arithmetic depend on cache pressure. Mapping policy
therefore changed arithmetic too. That experiment was **reverted**. The retained cache improvements
pass the exact short-run parity check above. Long-prompt batched prefill remains
a performance bottleneck requiring separate numerical validation.

### Final longer-prompt run

The final, conservative build completed **64 prompt tokens + 64 generated
tokens**: 0.30 tokens/s prefill, **1.98 tokens/s generation overall**, and
**2.65 tokens/s steady generation** after the first token (8.52 seconds).
Total wall time was 245 seconds, dominated by batched prefill. Its full frontier
logits matched the original batched arithmetic byte-for-byte:

`a53279f78570dba7b4f666d4687bdde797fa8347548ff8d74115917756825152`

The whole-run compute-engine mean was 3.5%, with a 42% peak; the low mean includes
the lengthy weight-loading prefill. Thus the 9.4x short-run improvement is not a
promise of 4.7 tokens/s or high GPU occupancy for every prompt. The retained
changes improve cache reuse without replacing the existing numerical path.

These are short measurements, not medians or cold-storage guarantees. Windows'
file cache was warm for much of the short comparison. Process read rates
include file-cache hits, so they are not SSD bandwidth. WDDM compute percentages
are sampled engine activity, not shader occupancy; shared GPU memory includes
host/runtime buffers and does not by itself prove VRAM spilling.

## Recommended use

Rebuild once, then start with 512 expert slots:

```sh
# Git Bash
make windows-rocm
export PATH="/c/Program Files/AMD/ROCm/7.2/bin:$PATH"
export HIP_VISIBLE_DEVICES=0
./ds4-bench.exe --rocm -m ds4flash.gguf --prompt-file prompt.txt \
  --ssd-streaming --ssd-streaming-cache-experts 512 \
  --ctx-start 64 --ctx-max 64 --ctx-alloc 256 --prefill-chunk 32 \
  --gen-tokens 64 --show-output
```

`prompt.txt` must have at least 64 tokens. The fixed-weight cache adjustment is
automatic; no `DS4_ROCM_STREAM_MODEL_CACHE_GB` override is needed with this build.
Use the expert **count** `512`, not `512GB`. Start with modest contexts and leave
the 2 GiB discrete-device reserve enabled; larger contexts consume more memory.
The host-RAM replay cache is also automatic (see the fourth iteration below);
disable it with `DS4_ROCM_STREAM_RAM_CACHE_GB=0` or `-RamCacheBytes 0` in the
profiler for A/B comparisons.

For the reproducible built-in prompt and GPU telemetry, from PowerShell:

```powershell
.\win\profile-streaming.ps1 -Label tuned -Experts 512 `
  -PromptTokens 64 -GenerateTokens 64
```

It writes `bench.csv`, `telemetry.csv`, `summary.json`, and logs under
`win/build/streaming/tuned/`. In Task Manager, inspect **Compute** and **Copy**
engines: the 3D engine stayed at 0% throughout these HIP runs.

## Regression coverage

The ROCm suite includes a real GPU cache test: a 640 MiB hot range under a
1 GiB cap must survive repeated range/span requests without rereading the file;
a mixed request reads only its missing 64 MiB. A Q8 operation then verifies
that the cached weights remain intact. The live chat/snapshot test now uses a
prompt longer than 32 tokens to exercise batched preparation and large offsets.
On the final build, the 36-token chat prompt answered **Paris** and restored a
16,377,884-byte snapshot with identical logits and generated tokens. Q8 checks,
cache retention tests, both Windows compatibility builds, and the CPU build passed.

The final code retains the original batched-prefill arithmetic. Profiles named
`tuned-64-fixed` and `test-rocm-prefill.log` were collected during the reverted
experiment; they are not evidence for the final batched path.

Logs for this iteration are under `win/build/streaming/`: `before-parity` and
`after-parity` hold the short comparison, `full-prefill-reference` and `final-64`
hold the original/final batched comparison, and `test-rocm-final.log` records
the final regression suite. The older validation
report is retained as the initial correctness baseline. CUDA, Metal, other
models and GPUs were not benchmarked in this iteration.

## Third iteration: long-prompt prefill

**PASS: 1,024-token prefill improved from 2.23 to 7.67 tokens/s, with exact
full-logit parity against the same-precision reference.**

The new prefill path loads selected experts, packs their **unchanged quantized
weight bytes on the GPU**, and uses the resident path's existing tiled/WMMA
kernels. Compact expert IDs preserve routing and token order. Packing scratch
is bounded by one layer's selected experts and released at prefill-chunk
boundaries, so it does not consume the subsequent decode cache budget. The SSD
reader still overlaps selected-expert loading with shared-expert computation.

### Precision must not depend on free VRAM

Packing alone passed exact MoE tests but left a full-model logit difference
(max absolute 0.538 on the 64-token case). Layer dumps localized the first
difference to **layer 8's shared-expert projection**; attention and routed MoE
outputs still matched there. The existing optional Q8-to-FP16 expansion cache
was admitting different layers under the two memory layouts, selecting FP16
BLAS or native Q8 arithmetic accordingly.

Discrete-GPU SSD streaming now defaults to native Q8 arithmetic consistently
(zero optional FP16-expansion cache). The explicit
`DS4_ROCM_STREAM_Q8_F16_CACHE_GB` diagnostic override and integrated-GPU budget
remain available. For parity comparisons, the original executable must use
`DS4_ROCM_STREAM_Q8_F16_CACHE_GB=0` too. **This is not a claim of identical
logits to the old, memory-dependent mixed-precision default.**

### Full-model comparison

Same official Q2 model and RX 9070 XT, 1,024 prompt tokens, 8 generated tokens,
prefill chunk 256, context allocation 1,064, and 512 expert slots:

| Measurement | Original prefill, native Q8 | New prefill, defaults |
| --- | ---: | ---: |
| Prefill tokens/s | 2.23 | **7.67** |
| Approximate prefill time | 459 s | **134 s** |
| Total process wall time | 470.4 s | **137.6 s** |
| Generation tokens/s (8 tokens) | 0.78 | **2.56** |
| Sampled compute-engine mean | 2.34% | **11.55%** |
| Sampled compute-engine peak | 28% | **44%** |

The measured prefill gain is **3.44x**. These are single-run measurements with
uncontrolled Windows file-cache warmth; a rebuild overlapped part of the
reference run. They establish the observed improvement, not a controlled
statistical estimate. No second model process ran concurrently.

The prompt was a captured version of `win/README.md`, rather than a repeated
synthetic passage. Its SHA-256 is
`27c558b2ca60a091ffcb316ecc405b5ebf55e948576199fb8c356cba334b4dbb`.
All **129,280 logits were finite** and the complete logits JSON files were
byte-identical, SHA-256:

`b2b9702ce815fdca6d786f8578c323b620beff035b794dca757fa742dbf52b6c`

The generated text also matched. Both runs read 180.77 GiB through the
selected-expert reader; the gain comes from eliminating the **additional
whole-table mapping/transfers and dense-cache churn**, not changing which
experts the model executes. SSD transfers remain a major limit on utilization.

Reproduce the final run with the captured prompt on this checkout:

```powershell
.\win\profile-streaming.ps1 -Label verify-prefill -Experts 512 `
  -PromptTokens 1024 -GenerateTokens 8 -PrefillChunk 256 `
  -PromptFile win/build/streaming/prefill-reference-1024/prompt.txt -DumpLogits
```

For new prompts, use your own `-PromptFile`; for direct benchmark commands,
use `--ssd-streaming --ssd-streaming-cache-experts 512 --prefill-chunk 256`
and allocate sufficient context. The FP16-expansion override can stay unset.

Evidence under `win/build/streaming/`:

- `prefill-reference-1024/` and `prefill-final-1024/`: commands, configuration,
  executable/prompt hashes, logs, CSV, telemetry, and logits.
- `prefill-parity.json`: identical prompt/logits checks, finite-logit check,
  and confirmation that the tested binary is the current `ds4-bench.exe`.
- `prefill-dumps/comparison.csv`: first divergence in the intermediate
  mixed-precision experiment, before native-Q8 stabilization.
- `build-prefill-final.log`, `test-prefill-final-build.log`, and
  `test-prefill-cpu.log`: final build/regression evidence.

### Regression evidence

- `tests/test_windows_rocm_prefill.c` compares SSD-selected and resident IQ2/Q2
  outputs exactly: 2, 7, 8, 32, 128, 257, then 32 rows, in normal and quality
  modes. It covers hot/cold experts, compact remapping, eviction, pending reads,
  buffer growth/reuse, and output bounds.
- `tests/test_rocm_q8_dispatch.c` also checks the actual 2048→4096 shared-Q8
  projection shape against a CPU oracle using non-power-of-two scales and
  activations. Maximum absolute error: **9.39e-7**.
- The live 36-token chat test answered **Paris**, with exact logits and token
  replay from its 16,377,884-byte snapshot.
- CPU build and both Windows compatibility toolchains passed.

The profiler now accepts `-PrefillChunk`, `-ContextTokens`, `-PromptFile`, and
`-TimeoutSeconds`; context allocation grows with prompt/generation length. It
also records prompt hashes and repeats the built-in passage for longer runs.
This iteration validates the 1,024-token workload on the stated Windows GPU;
multi-thousand-token soak tests and other backend/hardware runs are not included.

## Fourth iteration: managed host-RAM expert tier

**The VRAM → RAM → SSD order is now explicit.** A process-managed LRU replay
cache holds streamed expert spans in host memory, so a VRAM miss is served
from RAM when possible and the SSD is the last resort. It is byte-exact:
every parity check below produced identical logits.

### What the hierarchy looked like before this change

Device-wide Windows counters during a 64+8 run on the RX 9070 XT:

- **VRAM peaked at 8.92 GiB of 15.92 GiB (~56%)**: the fixed-weight cap
  (10.61 GiB) was not reached (~6.8 GiB of dense tensors were fetched) and the
  expert cache was capped at the recommended 512 slots (3.38 GiB) with a 2 GiB
  growth reserve. The unused VRAM is deliberate headroom, not spill.
- **System RAM was only an incidental OS file cache.** The standby list held
  ~35 GiB of previously read model pages during runs and served most warm-run
  reads, but nothing in the process managed that tier, and a 1,024-token
  prefill (180.77 GiB of expert traffic) churns it.
- **Every expert-cache miss was an SSD read.**

### The replay cache

`rocm/ds4_rocm_runtime.cuh` adds an LRU cache of raw file spans between the
VRAM expert cache and the SSD:

- Single-token (selected) loads record their spans after a disk read; a later
  request for the same `(offset, bytes)` is copied from process memory into the
  pinned read stage and skips the read.
- Prefill batch loads only look up: one prefill pass cycles ~74 GiB of routed
  experts, which no bounded cache can retain, so recording it would only add
  copies.
- Payloads are byte-exact copies of file bytes; only the I/O source changes.
- Capacity defaults to **half of usable host memory, clamped to 2–32 GiB**,
  and is disabled on unified-memory devices (their VRAM expert cache is
  already host RAM). `DS4_ROCM_STREAM_RAM_CACHE_GB` sizes it or disables it
  (`=0`); `DS4_ROCM_STREAM_RAM_CACHE_BYTES` is an exact diagnostic override.
  The profiler exposes `-RamCacheBytes` for A/B runs.

### Validation: byte-exact logits

- 64 prompt + 64 generated, context 256, cache **off**, **auto (~20 GiB)**, and
  **forced eviction (256 MiB)**: identical complete logits,
  `be8d9fd319b1062d307b68fdb7da048c04b6b36a43eae68119d4dfd4a7a9efeb`.
  The eviction run recorded 28,870 evictions and zero hits, so correctness is
  covered at full-model scale while the cache is constantly discarding and
  replacing entries.
- 64 + 256 generated, context 512, cache off vs auto in both run orders:
  identical logits (`806744376b4712…`) and identical generated text.
- The 1,024-token reference prompt reproduces the third iteration's stored
  logits hash
  `b2b9702ce815fdca6d786f8578c323b620beff035b794dca757fa742dbf52b6c`
  with the cache enabled.

The measurement runs used the build beginning `30fbcc264cf6`. A later rebuild
(the CPU target had replaced the root artifact) produced identical behavior:
the rebuilt binary (beginning `abfd1d39`) reproduces both hashes above.

### Measurements

64 prompt tokens + 256 generated, context 512, 512 expert slots; the cache
(auto ~20 GiB) recorded **110–112k replay hits out of ~145k span lookups
(~76%)** and worker-side read time fell about 20% (540.7 s → 433.3 s in the
first pair). Steady generation:

| Run order | Cache off | Cache auto |
| --- | ---: | ---: |
| First pair (off, then on) | 3.02 t/s | 3.21 t/s |
| Second pair (on, then off) | 3.08 t/s | 3.22 t/s |

The steady decode gain is about **+5%** and the first generated token was
~160 ms slower with the cache enabled (insert copies of freshly allocated
pages). Short 64+64 warm runs were unchanged (2.92 vs 2.84 t/s steady): the
Windows file cache already served most of those reads. A tiny cache
(256 MiB) is pure overhead for long outputs because no span survives long
enough to be replayed.

These are interleaved single runs on an already-warm file cache, so they
characterize this workload rather than a controlled median. The tier's value
is the explicit lookup order and managed retention — the cache keeps decode
history in committed process memory even when Windows trims its standby list —
not a fixed speedup on any machine. Cold-boot behavior and multi-thousand-token
decodes after a long prefill were not measured; the latter is where the largest
difference is expected.

### Regression evidence

- `tests/test_windows_rocm_ram_cache.c` compares single-token selected loads
  byte-for-byte against resident execution in three process configurations:
  cache disabled, 8 MiB (66 hits), and 256 KiB (forced eviction). It is wired
  into `win/test-rocm.sh`, where the hit configuration must record at least
  one replay hit or the suite fails.
- The full suite (Q8 dispatch, hot range/span cache, prefill parity, RAM cache,
  allocation admission) and the live chat/snapshot test pass; the chat prompt
  answered **Paris** and the 16,377,884-byte snapshot replayed identically.
