# DeepSeek V4 Flash on ds4 (DwarfStar) — Q2 setup for a 64 GB RAM / 16 GB VRAM machine

> **Created:** 2026-10-06
> **Status:** Realized (download + staged files done on this Windows host; binary build must happen on a Metal/CUDA/ROCm host — see “Open questions”)
> **Repo / scope:** antirez/ds4 (DwarfStar) — model choice, GGUF download, and run configuration for one 64 GB RAM + 16 GB VRAM machine

## Conventions captured at creation

World state as observed on 2026-10-06 at commit-level `main` (shallow clone of `antirez/ds4`):

- ds4 is **not a general GGUF runner** (README, `docs/MODELS.md`): only the GGUFs produced by the project itself, fetched via `download_model.sh` targets, are supported. Other quant mixes from the community may have unsupported tensor layouts.
- DeepSeek V4 Flash targets in `download_model.sh`, repo `antirez/deepseek-v4-gguf` (`REPO=` in `download_model.sh:10`):
  - `ds4f-q2` → `DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-0731.gguf`, about **81 GiB on disk** (HF reports 86,720,111,488 bytes), "Recommended model for 96 and 128 GB RAM machines". Routed experts IQ2_XXS gate/up + Q2_K down, imatrix-guided; Q8 projections/shared/output.
  - `ds4f-q2-q4` ≈ 98 GB; `ds4f-q4` ≈ 153 GB ("256 GB RAM or more"); `ds4f-mxfp4` ≈ 156 GB. All are larger than the 64 GB budget and are documented for larger systems.
- Platform detection in `Makefile:2` branches on `uname -s` (Darwin vs not); Linux targets are `make cuda-generic`, `make cuda CUDA_ARCH=sm_XX` (`docs/CUDA_MULTI_GPU.md`, Ada sm_89 explicitly supported), `make strix-halo` (ROCm), plus a `make cpu` CPU-only build. Metal (`make`) is macOS-only. There is no Windows build path anywhere in the repo (no MSVC/MinGW references; code uses Linux/GNU C APIs, e.g. `-D_GNU_SOURCE` at `Makefile:32`).
- Layer placement without TP (`docs/CUDA_MULTI_GPU.md`): whole layers go on the selected GPUs; `--gpu-vram auto` reserves free VRAM for graph+context; explicit budgets are comma-separated GiB per device ("Startup refuses a layout that would require unsupported CPU execution; reduce context or choose a smaller model"). Tested Flash-Q2 configs are four 48 GB cards — a single 16 GB card is **not** a tested configuration.
- Models larger than RAM run via SSD streaming (`docs/SSD_STREAMING.md`): `--ssd-streaming` with automatic expert-cache budget; optional `--ssd-streaming-cache-experts <bytes|slots>`. CUDA has streaming paths ("CUDA has streaming paths too"); example given is "Flash Q2 on a smaller Mac: `./ds4 --ssd-streaming --ctx 32768 --nothink`". Streaming trades speed for capacity and does not remove memory for non-routed weights, activations, scratch, and context.
- After a main-model download the script normally links `./ds4flash.gguf` to the GGUF; commands work from the repo root, elsewhere use `--chdir`. On Windows NTFS I created an **NTFS hardlink** `ds4flash.gguf` instead of a symlink — same effect for the loader; alternative is explicit `-m FILE`.
- Thinking defaults on; `--nothink` opts out. `--power N` trades throughput for lower GPU load (default 100).

## Sizing decision

Budget: 64 GB system RAM + 16 GB VRAM (80 GB combined, minus OS/KV/runtime headroom).

| target | file size | verdict for this budget |
| --- | --- | --- |
| `ds4f-q2` | ~81 GiB | **chosen** — smallest supported target; will not fit fully resident on 64 GB (docs' starting point is 96–128 GB), so run with SSD streaming; the 16 GB GPU holds some layers resident via `--gpu-vram` |
| `ds4f-q2-q4` | ~98 GB | no — needs 128 GB-class hosts |
| `ds4f-q4` | ~153 GB | no — documented for 256 GB+ or multi-GPU/TP |
| `ds4f-mxfp4` | ~156 GB | no — same class |

There is no smaller official DeepSeek V4 Flash quant in the project; anything below `ds4f-q2` requires untested community GGUFs, which ds4 explicitly does not support.

## What was realized on 2026-10-06

1. `git clone --depth 1 https://github.com/antirez/ds4.git` → `D:\ds4`.
2. Downloaded the `ds4f-q2` GGUF from `antirez/deepseek-v4-gguf` with the HF CLI (v1.28, unauthenticated) into `D:\ds4\gguf\`, matching what `download_model.sh ds4f-q2` would fetch; verified on-disk size equals the HF-reported 86,720,111,488 bytes (HF LFS sha256 `ca22ae2f…b6261c0`).
3. Created NTFS hardlink `D:\ds4\ds4flash.gguf` → the Q2 GGUF (stands in for the script's `ln -sfn`).

## Run configuration for the 64 GB + 16 GB machine (to apply on a Linux/CUDA host)

Build (e.g. RTX-class 16 GB card, Ada or newer — pick the real arch):

```sh
make cuda-generic        # or: make cuda CUDA_ARCH=sm_89 for Ada/L40S-class
```

Run Q2 with SSD streaming, auto expert-cache budget, GPU layers resident on the 16 GB card:

```sh
./ds4 --cuda --gpu-devices 0 --gpu-vram auto --ssd-streaming --ctx 32768
```

Guidance from the docs: keep the GGUF on a fast local SSD; start context modest (32K as in the docs example; the model card's native context is 1M but KV state eats RAM); prefer the automatic cache budget; if startup refuses a layout, reduce `--ctx` (choose a smaller model is not available — Q2 is already the smallest). Generation may be cache-miss sensitive; test a short generation before long tasks.

## Open questions / caveats as of creation

- This Windows host has no C toolchain, no CUDA (no NVIDIA driver installed), and no WSL distribution, so the binary could **not** be built here; ds4 has no Windows port. The plan stops at "code + model staged on disk + configuration documented".
- A single 16 GB GPU + 64 GB RAM running V4 Flash Q2 (streaming) is not among the tested configurations in the docs; expect either successful bounded placement or a clean startup refusal, per `docs/CUDA_MULTI_GPU.md`.
- If the 16 GB GPU is not NVIDIA, ds4's CUDA path won't apply; ROCm support is documented for Strix Halo hosts only (`make strix-halo`, gfx1151) — a desktop AMD GPU is untested territory.
