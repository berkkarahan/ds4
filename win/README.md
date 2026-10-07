# Native Windows builds

The native Windows port builds `ds4.exe`, `ds4-server.exe`, `ds4-agent.exe`,
and `ds4-bench.exe`. MinGW produces the CPU binaries. AMD HIP/ROCm produces
the GPU binaries. Both targets write those four executables; the last build
selects the backend of the copies at the repository root.

On Windows 11 with ROCm 7.2, the RX 9070 XT (`gfx1201`, 15.92 GiB VRAM)
successfully runs the official DeepSeek V4 Flash Q2 GGUF with SSD streaming.
The model is 80.76 GiB; it does not need to fit entirely in RAM or VRAM.
See [validation results](VALIDATION.md) for the exact hardware, model hash,
commands, results, and coverage limits.
For GPU-utilization measurements, cache settings, the memory hierarchy
(VRAM → host RAM replay cache → SSD), and long-prompt prefill work,
see [RX 9070 XT streaming performance](STREAMING_PERFORMANCE.md).

## Prerequisites

- Git Bash/MSYS and GNU make. Use Git Bash rather than Windows' WSL `bash.exe`.
- MinGW-w64 GCC for `windows-cpu` and `test-windows`.
- AMD HIP SDK, default `C:/Program Files/AMD/ROCm/7.2`, for `windows-rocm`.
- Visual Studio C++ Build Tools and Windows SDK headers/libraries, which the
  HIP compiler discovers automatically. A full Visual Studio IDE is unnecessary.
- `llvm-dlltool.exe` on `PATH`, or the Scoop LLVM installation, to generate
  MSVC import libraries for hipBLAS and hipBLASLt.

## Build

From Git Bash in the repository root:

```sh
make windows-rocm
make windows-cpu
```

Both commands build `ds4.exe`, `ds4-server.exe`, `ds4-agent.exe`, and
`ds4-bench.exe`. Objects are separate under `win/build/rocm` and
`win/build/cpu`. The default `WIN_CPU_OUTPUT` is `ds4-bench.exe` at the
repository root, and the other three CPU binaries are written beside it, so
a later `windows-rocm` build replaces all four. To keep the CPU binaries
alongside the GPU binaries:

```sh
make windows-cpu WIN_CPU_OUTPUT=win/build/cpu/ds4-bench.exe
```

The default GPU target is `gfx1201`. Override the SDK and architecture with:

```sh
make windows-rocm ROCM_PATH="C:/Program Files/AMD/ROCm/7.2" ROCM_ARCH=gfx1151
```

`gfx1151` is the existing Strix Halo target; inference validation here used
`gfx1201`. Architecture-specific WMMA kernels retain a runtime dispatch guard;
unsupported gfx12 entry into the gfx11-only rowtile kernel traps instead of
silently returning zero outputs.

Plain `make` retains the upstream host's `all` target. `make clean` removes
Windows objects as well as the existing build outputs.

## Download and run DeepSeek Flash Q2

Use the project's supported quantization from `download_model.sh`:

```sh
MODEL=DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-0731.gguf
hf download antirez/deepseek-v4-gguf "$MODEL" --local-dir gguf
hf cache verify antirez/deepseek-v4-gguf --local-dir gguf

export PATH="/c/Program Files/AMD/ROCm/7.2/bin:$PATH"
export HIP_VISIBLE_DEVICES=0   # verify this is the intended GPU with hipInfo.exe
./ds4-bench.exe --rocm -m "gguf/$MODEL" --prompt-file prompt.txt \
  --ssd-streaming --ssd-streaming-cold --ssd-streaming-cache-experts 512 \
  --ctx-start 16 --ctx-max 32 --step-incr 16 --ctx-alloc 128 \
  --prefill-chunk 32 --gen-tokens 16 --show-output --csv result.csv
```

From `cmd.exe`, `win\start-ds4.cmd` and `win\start-ds4-agent.cmd` prepend
`%ROCM_PATH%\bin` (default `C:\Program Files\AMD\ROCm\7.2\bin`) and start the
compiled binaries from the repository root. Extra arguments are forwarded.
`HIP_VISIBLE_DEVICES` is set to `0` when it is unset.

```bat
win\start-ds4.cmd --model "gguf\DeepSeek V4 Flash.gguf" -n 8 --nothink -p "Say hi"
win\start-ds4-agent.cmd --model "gguf\DeepSeek V4 Flash.gguf" -n 16 --non-interactive --ssd-streaming
```

`prompt.txt` must contain at least 32 tokens for this sweep. Generation is a
continuation of each selected prefix. Omit `--ssd-streaming-cold` for normal
expert-cache preloading. The validation report includes a full chat-prompt test
as well as this benchmark.

The same ROCm runtime serves one-shot CLI prompts and the HTTP API. Keep the
context small for a smoke test; the model still streams from SSD:

```sh
./ds4.exe --rocm -m "gguf/$MODEL" --nothink -n 8 -c 2048 -p "Say hi" \
  --ssd-streaming --ssd-streaming-cache-experts 512

./ds4-server.exe --rocm -m "gguf/$MODEL" --host 127.0.0.1 --port 8000 \
  -n 8 -c 2048 --ssd-streaming --ssd-streaming-cache-experts 512
```

A chat completion with `"model": "deepseek-chat"` disables thinking. The agent
runs one turn without the TUI:

```sh
./ds4-agent.exe --rocm -m "gguf/$MODEL" --non-interactive --nothink \
  -n 16 -c 8192 -p "Say hi" --ssd-streaming --ssd-streaming-cache-experts 512
```

Tool shells prefer Git Bash. Set `DS4_SHELL` to an explicit `bash.exe` when it
is not on `PATH`. The agent writes the command to a temporary script and does
not duplicate the model mapping into that process.

Select the intended GPU with `HIP_VISIBLE_DEVICES` on mixed-GPU machines.
The test launcher defaults to device 0 and respects an explicit override.

The instance lock and staged session files use Windows' temporary directory.
`DS4_LOCK_FILE` can still override the lock path. ROCm DLLs must be on `PATH`;
the CPU binary needs MinGW's `libwinpthread-1.dll` on `PATH`.

Discrete GPUs reserve **2 GiB of device headroom** for expert-cache growth.
The existing integrated/UMA default remains 16 GiB. The diagnostic override
`DS4_ROCM_STREAM_FREE_RESERVE_GB` remains available. Host allocations separately
respect available physical memory and commit headroom via `GlobalMemoryStatusEx`.
An RX 9070 XT is a discrete GPU: changing a BIOS UMA reservation does not expand
its VRAM, and `DS4_CUDA_MANAGED=1` is not required by the validated configuration.
The fixed-weight cache cap defaults to two-thirds of discrete VRAM (about
10.61 GiB here), avoiding repeated reloads of Flash Q2's 8.20 GiB dense weights.
The 512-expert setting is the measured starting point for this 16 GiB card;
the automatic expert budget remains available by omitting the count.
DeepSeek IQ2/Q2 prefill now packs selected experts for the existing resident
compute kernels. Discrete SSD streaming uses native Q8 projections consistently,
so optional FP16 cache availability no longer changes the default arithmetic.

A managed **host-RAM expert replay cache** sits between the VRAM expert cache
and the SSD: single-token loads record their file spans in process memory and
later misses replay from RAM (byte-exact, LRU-bounded). The default capacity is
half of usable host memory clamped to 2–32 GiB; set
`DS4_ROCM_STREAM_RAM_CACHE_GB` (0 disables) or
`DS4_ROCM_STREAM_RAM_CACHE_BYTES` to override it. Unified-memory devices
disable it because their VRAM expert cache already lives in system RAM.

For longer prompts, the profiling launcher accepts a larger prefill chunk and
allocates context space from the requested prompt/generation lengths:

```powershell
.\win\profile-streaming.ps1 -Label long-prefill -Experts 512 `
  -PromptTokens 1024 -GenerateTokens 8 -PrefillChunk 256 `
  -PromptFile win/README.md -DumpLogits
```

With a direct `ds4-bench.exe` command, set `--prefill-chunk 256` and allocate
enough context for the prompt plus generated tokens. Measurement details and
the same-precision comparison are in [STREAMING_PERFORMANCE.md](STREAMING_PERFORMANCE.md).

## Regression checks

```sh
make test-windows
make windows-rocm
bash win/test-rocm.sh
# Optional full-model validation (prefill, generation, snapshot replay):
bash win/test-rocm.sh "gguf/$MODEL"
```

`test-windows` exercises both MSVC-ABI and MinGW builds: concurrent snapshots,
registry exhaustion, buffer overruns, reads beyond 4 GiB, cursor preservation,
temporary paths, and the pthread shim. `win/test-rocm.sh` compares Q8 decode
and batch sizes 255/256/257 to a CPU oracle, verifies hot range/span retention,
compares selected-expert prefill exactly with resident execution across batch
sizes and precision modes, verifies the host-RAM replay cache byte-for-byte
with hits and forced eviction, and checks allocation admission on the actual
GPU. Its optional model argument runs one model process at a time, including
batched prefill and snapshot replay.

## Implementation notes

- The GPU executable uses the MSVC ABI throughout its host and HIP translation
  units; it does not mix MinGW and MSVC heaps or `FILE *` objects.
- `ds4_win.h` supplies the required POSIX subset. Positional reads reopen an
  independent handle so parallel readers do not alter the original file
  position. Snapshot streams support `rb` and `wb`; exhaustion, flush failures,
  and copy-back overruns return errors.
- Generated import libraries and their SDK path/size/mtime stamps live under
  `win/third_party`, are ignored by git, and are refreshed when the SDK changes.
  `DS4_REGEN_IMPORT_LIBS=1` forces hipBLAS/hipBLASLt regeneration.
- Vendored rocWMMA headers are from `rocm-7.1.0`; the generated version header
  reads 2.2.1. This mixed provenance is retained from the original port and must
  be reconciled when updating the vendor snapshot. Compilation and numerical
  checks here use ROCm 7.2 on gfx1201.

## Scope and remaining work

`ds4-server.exe` serves the local HTTP API on one GPU. Distributed/TP serving
on Windows remains unvalidated. The socket shim has known gaps, including
POSIX error translation, per-call nonblocking semantics, socket-handle width,
and Winsock initialization lifetime. Linux ROCm, CUDA, and Metal runtime tests
require their respective hosts.
