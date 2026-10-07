#!/usr/bin/env bash
# build-rocm.sh — native Windows ROCm/HIP build of ds4-bench.exe.
#
# Builds DS4's GPU (HIP) backend natively on Windows with the AMD HIP SDK — no
# WSL, no MSVC on PATH, no full Visual Studio install required at the command
# line (hipcc auto-discovers the VS build tools' headers). Defaults to gfx1201
# (RDNA4, e.g. Radeon RX 9070 XT); pass ROCM_ARCH=gfx1151 for Strix Halo.
# Produces ds4-bench.exe at the repo root; objects stage under win/build/rocm/.
#
# Why a script instead of pure Make: hipcc.exe's .bat wrapper splits arguments
# on spaces, so paths like "C:/Program Files/AMD/ROCm/7.2" break -I/-L flags.
# This script relies on the SDK's default include/lib search (hipcc adds the SDK
# include via -idirafter automatically) and a space-free path for the hipblas
# import lib, sidestepping the quoting problem.
#
# Usage:
#   win/build-rocm.sh                 # uses defaults below
#   ROCM_PATH="C:/Program Files/AMD/ROCm/7.2" ROCM_ARCH=gfx1151 win/build-rocm.sh
#
# Requirements:
#   - AMD HIP SDK (default C:/Program Files/AMD/ROCm/7.2) with hipcc.exe + clang.
#   - Vendored rocWMMA headers at win/third_party/rocwmma (in-tree).
#   - hipblas.lib / hipblaslt.lib (MSVC import libs). Generated on the fly from
#     the SDK DLLs when absent or stale (needs llvm-dlltool on PATH or in the
#     scoop LLVM install); set DS4_REGEN_IMPORT_LIBS=1 to force regeneration.
set -euo pipefail

ROCM_PATH="${ROCM_PATH:-C:/Program Files/AMD/ROCm/7.2}"
ROCM_ARCH="${ROCM_ARCH:-gfx1201}"

# Resolve the repo root (this script lives in win/).
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

HIPCC="$ROCM_PATH/bin/hipcc.exe"
CLANG="$ROCM_PATH/bin/clang.exe"
THIRD="win/third_party"
ROCWMMA_INC="$THIRD/rocwmma"
# Objects stage under win/build/rocm/ so they cannot clobber POSIX/windows-cpu
# outputs built in the same tree; only ds4-bench.exe lands at the repo root.
OBJ="win/build/rocm"
mkdir -p "$OBJ"

if [ ! -x "$HIPCC" ]; then
    echo "error: hipcc.exe not found at '$HIPCC' (set ROCM_PATH)" >&2
    exit 2
fi

# --- ensure MSVC-style import libs exist ------------------------------------
# The Windows HIP SDK ships only MinGW-style lib*.dll.a import libs, which the
# MSVC linker (lld-link) cannot consume. Synthesize MSVC .lib files from each
# DLL's export table and cache them in win/third_party/. main's ROCm path links
# BOTH hipBLAS and hipBLASLt (see Makefile ROCM_LDLIBS).
DLLTOOL="$(command -v llvm-dlltool.exe 2>/dev/null || true)"
if [ -z "$DLLTOOL" ] && [ -x "$HOME/scoop/apps/llvm/current/bin/llvm-dlltool.exe" ]; then
    DLLTOOL="$HOME/scoop/apps/llvm/current/bin/llvm-dlltool.exe"
fi
OBJDUMP="$ROCM_PATH/bin/llvm-objdump.exe"

# Signature of a file (path + size + mtime) used as a cache stamp, so an SDK
# upgrade at the same ROCM_PATH invalidates the cached import libs. Falls back
# to ls output when stat -c is unavailable.
file_sig() {
    stat -c '%n %s %Y' "$1" 2>/dev/null || ls -l "$1" 2>/dev/null || echo unknown
}

gen_import_lib() {
    # $1 = dll base name (e.g. libhipblas) ; produces $THIRD/<short>.lib
    local dll="$1.dll" out="$THIRD/${1#lib}.lib" stamp="$THIRD/.${1#lib}.lib.src"
    if [ ! -f "$ROCM_PATH/bin/$dll" ]; then
        echo "error: $ROCM_PATH/bin/$dll not found (HIP SDK incomplete?)" >&2
        exit 2
    fi
    local sig; sig="$(file_sig "$ROCM_PATH/bin/$dll")"
    if [ -f "$out" ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$sig" ] && \
       [ -z "${DS4_REGEN_IMPORT_LIBS:-}" ]; then
        return 0
    fi
    if [ -z "$DLLTOOL" ]; then
        echo "error: need llvm-dlltool to build $out (install LLVM or scoop llvm)" >&2
        exit 2
    fi
    echo "==> generating $out from $dll"
    local def; def="$(mktemp)"
    { echo "LIBRARY $dll"; echo "EXPORTS"; \
      "$OBJDUMP" -p "$ROCM_PATH/bin/$dll" \
        | awk '/Export Table:/{f=1} f&&/^[[:space:]]+[0-9]+[[:space:]]+0x[0-9a-f]+[[:space:]]+/{print $NF}' \
        | sort -u; } > "$def"
    "$DLLTOOL" -m i386:x86-64 -d "$def" -l "$out" -D "$dll"
    rm -f "$def"
    printf '%s' "$sig" > "$stamp"
}

gen_import_lib libhipblas
gen_import_lib libhipblaslt

# rocBLAS is also used directly by ds4_rocm.cu (the Makefile's ROCM_LDLIBS links
# -lrocblas). Unlike hipBLAS/hipBLASLt it comes with a usable MSVC import lib at
# lib/rocblas.lib, so just copy it into the space-free third_party path (kept in
# sync with the SDK copy it came from).
if [ ! -f "$ROCM_PATH/lib/rocblas.lib" ]; then
    echo "error: rocblas.lib not found in '$ROCM_PATH/lib'" >&2
    exit 2
fi
rbsig="$(file_sig "$ROCM_PATH/lib/rocblas.lib")"
if [ ! -f "$THIRD/rocblas.lib" ] || [ ! -f "$THIRD/.rocblas.lib.src" ] || \
   [ "$(cat "$THIRD/.rocblas.lib.src")" != "$rbsig" ]; then
    echo "==> copying rocblas.lib from the HIP SDK"
    cp "$ROCM_PATH/lib/rocblas.lib" "$THIRD/rocblas.lib"
    printf '%s' "$rbsig" > "$THIRD/.rocblas.lib.src"
fi

# --- common flags ----------------------------------------------------------
# Host C files are compiled in the MSVC ABI (clang --target=...-windows-msvc) so
# they link against the hipcc-produced (MSVC-ABI) ds4_rocm.o. DS4_WIN_PTHREAD
# selects the Win32 pthread shim (MSVC has no <pthread.h>). DS4_ROCM_BUILD
# matches the Makefile `strix-halo` target's CFLAGS so the host C files take the
# ROCm code paths. -Wno-microsoft-goto: the shared C code deliberately uses
# goto over initializations (legal C; clang warns only because the target is
# MSVC-compatible).
HOSTFLAGS="--target=x86_64-pc-windows-msvc -O3 -ffast-math -fno-finite-math-only \
  -DDS4_ROCM_BUILD -DDS4_WIN_PTHREAD -D_CRT_SECURE_NO_WARNINGS \
  -Wno-deprecated-declarations -Wno-unused-command-line-argument \
  -Wno-microsoft-goto"

# -std=c++17 is required by ROCm's hipcub/rocprim headers (std::visit and
# constexpr_value_variant), which main's ds4_rocm.h pulls in via <hipcub>.
# DS4_WIN_PTHREAD makes ds4_rocm.cu pull in the Win32 pthread shim instead of
# <pthread.h> (see win/ds4_pthread_win.h); the host C files already set it.
GPUFLAGS="--offload-arch=$ROCM_ARCH -std=c++17 -O3 -fno-finite-math-only \
  -D__HIP_PLATFORM_AMD__ -DDS4_WIN_PTHREAD -D_CRT_SECURE_NO_WARNINGS \
  -Wno-deprecated-declarations -Wno-unused-command-line-argument -I$ROCWMMA_INC"

# main split the GPU backend: the ROCm path compiles ds4_rocm.cu (which pulls in
# the rocm/*.cuh kernel/launch units), NOT ds4_cuda.cu (that is the CUDA/nvcc
# unit). current main's ROCm CORE_OBJS additionally carries the API-compat and
# no-GPU stub TUs, the shared ds4_image/ds4_tp/ds4_layer_pack/ds4_gpu_args host
# TUs, and the vendored mmq prefill tier ($(ROCM_MMQ_OBJS) of the Makefile
# strix-halo target), so all of those are compiled and linked here too.
echo "==> compiling ds4_rocm.cu (HIP, $ROCM_ARCH)"
"$HIPCC" $GPUFLAGS -c ds4_rocm.cu -o "$OBJ/ds4_rocm.o"

echo "==> compiling ds4_rocm_compat.cu ds4_rocm_unavailable.cu (HIP)"
for src in ds4_rocm_compat ds4_rocm_unavailable; do
    "$HIPCC" $GPUFLAGS -c "$src.cu" -o "$OBJ/$src.o"
done

MMQFLAGS="$GPUFLAGS -DGGML_USE_HIP -DDS4_HIP_MMQ_Y=64 -Icuda/mmq"
for src in cuda/mmq/ds4_ggml_stubs cuda/mmq/ds4_mmq cuda/mmq/quantize cuda/mmq/mmid cuda/mmq/mmvq cuda/mmq/test/d2r_stubs; do
    echo "==> compiling $src.cu (HIP, mmq tier)"
    "$HIPCC" $MMQFLAGS -c "$src.cu" -o "$OBJ/$(basename "$src").rocm.o"
done

# Host C translation units (MSVC ABI, to match the hipcc-built ds4_rocm.o).
for src in ds4 ds4_bench ds4_help ds4_distributed ds4_ssd ds4_image ds4_tp ds4_layer_pack ds4_gpu_args; do
    echo "==> compiling $src.c (host, MSVC ABI)"
    "$CLANG" $HOSTFLAGS -c "$src.c" -o "$OBJ/$src.o"
done

echo "==> linking ds4-bench.exe"
"$HIPCC" --offload-arch="$ROCM_ARCH" \
    "$OBJ/ds4_bench.o" "$OBJ/ds4_help.o" "$OBJ/ds4.o" "$OBJ/ds4_distributed.o" \
    "$OBJ/ds4_ssd.o" "$OBJ/ds4_image.o" "$OBJ/ds4_tp.o" "$OBJ/ds4_layer_pack.o" \
    "$OBJ/ds4_gpu_args.o" "$OBJ/ds4_rocm.o" "$OBJ/ds4_rocm_compat.o" \
    "$OBJ/ds4_rocm_unavailable.o" "$OBJ/ds4_ggml_stubs.rocm.o" "$OBJ/ds4_mmq.rocm.o" \
    "$OBJ/quantize.rocm.o" "$OBJ/mmid.rocm.o" "$OBJ/mmvq.rocm.o" "$OBJ/d2r_stubs.rocm.o" \
    -o ds4-bench.exe -L"$THIRD" -lhipblas -lhipblaslt -lrocblas -lws2_32

echo "==> done: ds4-bench.exe"
echo "    Run with the SDK bin on PATH, e.g.:"
echo "      PATH=\"$(cygpath -u "$ROCM_PATH/bin"):\$PATH\" ./ds4-bench.exe --prompt-file FILE -m MODEL.gguf"
