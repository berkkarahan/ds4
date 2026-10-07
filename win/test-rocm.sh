#!/usr/bin/env bash
# Run the Q8 CPU-reference oracle using an existing windows-rocm build.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
ROCM_PATH="${ROCM_PATH:-C:/Program Files/AMD/ROCm/7.2}"
ROCM_ARCH="${ROCM_ARCH:-gfx1201}"
# Validate one device, matching the single-GPU backend.
export HIP_VISIBLE_DEVICES="${HIP_VISIBLE_DEVICES:-0}"
SDK_BIN="$(cygpath -u "$ROCM_PATH/bin")"
OBJ=win/build/rocm
OUT=win/build/validation
mkdir -p "$OUT"
if [ ! -f "$OBJ/ds4_rocm.o" ]; then
    echo "Build with make windows-rocm first." >&2
    exit 2
fi
"$ROCM_PATH/bin/clang.exe" --target=x86_64-pc-windows-msvc -std=c11 \
    -DDS4_ROCM_BUILD -D_CRT_SECURE_NO_WARNINGS -I. -c tests/test_rocm_q8_dispatch.c -o "$OUT/test-rocm-q8.o"
"$ROCM_PATH/bin/hipcc.exe" --offload-arch="$ROCM_ARCH" \
    "$OUT/test-rocm-q8.o" "$OBJ/ds4_rocm.o" "$OBJ/ds4_image.o" "$OBJ/"*.rocm.o \
    -Lwin/third_party -lhipblas -lhipblaslt -lrocblas -o "$OUT/test-rocm-q8.exe"
PATH="$SDK_BIN:$PATH" "$OUT/test-rocm-q8.exe"

"$ROCM_PATH/bin/clang.exe" --target=x86_64-pc-windows-msvc -std=c11 \
    -DDS4_ROCM_BUILD -D_CRT_SECURE_NO_WARNINGS -I. \
    -c tests/test_windows_rocm_cache.c -o "$OUT/test-rocm-cache.o"
"$ROCM_PATH/bin/hipcc.exe" --offload-arch="$ROCM_ARCH" \
    "$OUT/test-rocm-cache.o" "$OBJ/ds4_rocm.o" "$OBJ/ds4_image.o" "$OBJ/"*.rocm.o \
    -Lwin/third_party -lhipblas -lhipblaslt -lrocblas -o "$OUT/test-rocm-cache.exe"
PATH="$SDK_BIN:$PATH" "$OUT/test-rocm-cache.exe"

"$ROCM_PATH/bin/clang.exe" --target=x86_64-pc-windows-msvc -std=c11 \
    -DDS4_ROCM_BUILD -D_CRT_SECURE_NO_WARNINGS -I. \
    -c tests/test_windows_rocm_prefill.c -o "$OUT/test-rocm-prefill.o"
"$ROCM_PATH/bin/hipcc.exe" --offload-arch="$ROCM_ARCH" \
    "$OUT/test-rocm-prefill.o" "$OBJ/ds4_rocm.o" "$OBJ/ds4_image.o" "$OBJ/"*.rocm.o \
    -Lwin/third_party -lhipblas -lhipblaslt -lrocblas -o "$OUT/test-rocm-prefill.exe"
PATH="$SDK_BIN:$PATH" "$OUT/test-rocm-prefill.exe"

# Host RAM replay cache: disabled, hit path, then forced eviction. The hit run
# requires at least one replay from process memory.
"$ROCM_PATH/bin/clang.exe" --target=x86_64-pc-windows-msvc -std=c11 \
    -DDS4_ROCM_BUILD -D_CRT_SECURE_NO_WARNINGS -I. \
    -c tests/test_windows_rocm_ram_cache.c -o "$OUT/test-rocm-ram.o"
"$ROCM_PATH/bin/hipcc.exe" --offload-arch="$ROCM_ARCH" \
    "$OUT/test-rocm-ram.o" "$OBJ/ds4_rocm.o" "$OBJ/ds4_image.o" "$OBJ/"*.rocm.o \
    -Lwin/third_party -lhipblas -lhipblaslt -lrocblas -o "$OUT/test-rocm-ram.exe"
DS4_ROCM_STREAM_RAM_CACHE_BYTES=0 \
    PATH="$SDK_BIN:$PATH" "$OUT/test-rocm-ram.exe"
DS4_ROCM_STREAM_RAM_CACHE_BYTES=8388608 DS4_ROCM_STREAM_CACHE_STATS=1 \
    PATH="$SDK_BIN:$PATH" "$OUT/test-rocm-ram.exe" > "$OUT/test-rocm-ram-hits.log" 2>&1
cat "$OUT/test-rocm-ram-hits.log"
grep -Eq 'host RAM cache: hits=[1-9]' "$OUT/test-rocm-ram-hits.log"
DS4_ROCM_STREAM_RAM_CACHE_BYTES=262144 \
    PATH="$SDK_BIN:$PATH" "$OUT/test-rocm-ram.exe"

"$ROCM_PATH/bin/hipcc.exe" --offload-arch="$ROCM_ARCH" -std=c++17 \
    -D_CRT_SECURE_NO_WARNINGS -I. tests/test_rocm_memory.cu -o "$OUT/test-rocm-memory.exe"
PATH="$SDK_BIN:$PATH" "$OUT/test-rocm-memory.exe"

# An explicit model argument opts into the full-model test (one process).
if [ "$#" -gt 0 ]; then
    "$ROCM_PATH/bin/clang.exe" --target=x86_64-pc-windows-msvc -std=c11 \
        -DDS4_ROCM_BUILD -I. -c tests/test_windows_inference.c -o "$OUT/test-inference.o"
    objects=()
    for obj in "$OBJ/"*.o; do
        [ "$(basename "$obj")" = ds4_bench.o ] || objects+=("$obj")
    done
    "$ROCM_PATH/bin/hipcc.exe" --offload-arch="$ROCM_ARCH" \
        "$OUT/test-inference.o" "${objects[@]}" -Lwin/third_party \
        -lhipblas -lhipblaslt -lrocblas -lws2_32 -o "$OUT/test-inference.exe"
    PATH="$SDK_BIN:$PATH" "$OUT/test-inference.exe" "$1"
fi
