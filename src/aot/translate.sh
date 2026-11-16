#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"

DEFAULT_SO="$REPO_ROOT/ddk/libGLES_mali.so"
if [ ! -f "$DEFAULT_SO" ] && [ -f "$ROOT_DIR/ddk/libGLES_mali.so" ]; then
    DEFAULT_SO="$ROOT_DIR/ddk/libGLES_mali.so"
elif [ ! -f "$DEFAULT_SO" ] && [ -f "$ROOT_DIR/driver/r54p3-00eac0-6eda07/lib64/egl/libGLES_mali.so" ]; then
    DEFAULT_SO="$ROOT_DIR/driver/r54p3-00eac0-6eda07/lib64/egl/libGLES_mali.so"
fi

SO_ARG="${1:-$DEFAULT_SO}"
SO_PATH=""
if [ -f "$SO_ARG" ]; then
    SO_PATH="$(realpath "$SO_ARG")"
elif [ -f "$REPO_ROOT/$SO_ARG" ]; then
    SO_PATH="$(realpath "$REPO_ROOT/$SO_ARG")"
fi

# Pinned static LLVM-21 headers & libraries in third_party/
LLVM21_INC_DIR="$REPO_ROOT/third_party/prebuilts/llvm-21/includes"
LLVM21_LIB_DIR="$REPO_ROOT/third_party/prebuilts/llvm-21/lib"

ELF_TAG="r54p3-00eac0-6eda07"
if [ -n "$SO_PATH" ] && [ -f "$SO_PATH" ]; then
    ELF_TAG="$(python3 -c '
import struct, re, sys
with open(sys.argv[1], "rb") as f:
    raw = f.read()
if raw[:4] != b"\x7fELF" or raw[4] != 2 or struct.unpack_from("<H", raw, 18)[0] != 183:
    print("INVALID")
    sys.exit(0)
m = re.search(rb"r\d+p\d+-\d+[a-z0-9]+", raw)
ver = m.group(0).decode("ascii") if m else "arm64_generic"
e_shoff = struct.unpack_from("<Q", raw, 40)[0]
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", raw, 58)
shstr_off = struct.unpack_from("<Q", raw, e_shoff + e_shstrndx * e_shentsize + 24)[0]
bid = ""
for i in range(e_shnum):
    off = e_shoff + i * e_shentsize
    sh_name, _, _, _, sh_offset, sh_size = struct.unpack_from("<IIQQQQ", raw, off)[:6]
    end = raw.find(b"\x00", shstr_off + sh_name)
    if raw[shstr_off + sh_name:end] == b".note.gnu.build-id" and sh_size >= 20:
        bid = "-" + raw[sh_offset+16:sh_offset+19].hex()
        break
print(f"{ver}{bid}")
' "$SO_PATH")"

    if [ "$ELF_TAG" = "INVALID" ]; then
        echo "[ERROR] $SO_PATH is not a 64-bit AArch64 (ELF64 EM_AARCH64) binary."
        exit 1
    fi
fi

OUT_DIR="${2:-${BUILD_DIR:-$REPO_ROOT/build-x86-64}}"
mkdir -p "$OUT_DIR" "$OUT_DIR/aot" "$OUT_DIR/jit_cache"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

if [ -x "$OUT_DIR/aot/horizon2llvm" ]; then
    PASS_DRIVER="$OUT_DIR/aot/horizon2llvm"
elif [ -x "$OUT_DIR/horizon2llvm" ]; then
    PASS_DRIVER="$OUT_DIR/horizon2llvm"
else
    PASS_DRIVER="$OUT_DIR/aot/horizon2llvm"
fi

NEEDS_REBUILD=0
if [ ! -x "$PASS_DRIVER" ] || [ "$ROOT_DIR/horizon2llvm/mali_llvm_passes.cpp" -nt "$PASS_DRIVER" ]; then
    NEEDS_REBUILD=1
elif ldd "$PASS_DRIVER" 2>/dev/null | grep -q "not found"; then
    echo "[Build] Existing $PASS_DRIVER has missing shared library dependencies on this host; rebuilding..."
    NEEDS_REBUILD=1
fi

if [ "$NEEDS_REBUILD" -eq 1 ]; then
    echo "[Build] Compiling unified horizon2llvm -> $PASS_DRIVER..."
    g++ -std=c++20 -O2 -fPIC -DHORIZON2LLVM_USE_SYSTEM_LLVM_HEADERS=1 \
        -I"$ROOT_DIR/horizon2llvm" -I"$REPO_ROOT/third_party/prebuilts/llvm-21/include" -I"$LLVM21_INC_DIR" \
        "$ROOT_DIR/horizon2llvm/main.cpp" \
        "$ROOT_DIR/horizon2llvm/mali_llvm_passes.cpp" \
        "$ROOT_DIR/horizon2llvm/ko_lifter.cpp" \
        "$ROOT_DIR/horizon2llvm/llvm_backend.cpp" \
        "$ROOT_DIR/horizon2llvm/aarch64/aarch64_lifter.cpp" \
        "$ROOT_DIR/horizon2llvm/passes.cpp" \
        "$ROOT_DIR"/horizon2llvm/lowering/*.cpp \
        -Wl,--start-group "$LLVM21_LIB_DIR"/*.a -Wl,--end-group \
        -lz -l:libzstd.so.1 -ldl -lpthread -lm -o "$PASS_DRIVER"
fi

export MALI_AOT_CLOSURE_MODE="${MALI_AOT_CLOSURE_MODE:-minimal}"
export MALI_AOT_CLOSURE_DEPTH="${MALI_AOT_CLOSURE_DEPTH:-2}"

# Resolve incremental seed file (unhandled_jit.txt / aot_seeds.txt) unless MALI_AOT_RESET_SEEDS=1
ACTIVE_SEED_FILE="${MALI_AOT_SEED_FILE:-}"
if [ "${MALI_AOT_RESET_SEEDS:-0}" = "1" ]; then
    ACTIVE_SEED_FILE=""
elif [ -z "$ACTIVE_SEED_FILE" ]; then
    if [ -f "$REPO_ROOT/src/r54p3_vulkan_icd/aot_seeds.txt" ]; then
        ACTIVE_SEED_FILE="$REPO_ROOT/src/r54p3_vulkan_icd/aot_seeds.txt"
    elif [ -f "$REPO_ROOT/src/aot/aot_seeds.txt" ]; then
        ACTIVE_SEED_FILE="$REPO_ROOT/src/aot/aot_seeds.txt"
    elif [ -f "$REPO_ROOT/aot_seeds.txt" ]; then
        ACTIVE_SEED_FILE="$REPO_ROOT/aot_seeds.txt"
    fi
fi

echo "================================================================================"
echo " Mali 64-Bit AArch64 -> x86-64 Linux C++ LLVM 6-Pass AOT Translation Pipeline"
echo " Input Binary   : ${SO_PATH:-(prebuilt $OUT_DIR/r54p3_lifted.o)}"
echo " Driver Build   : $ELF_TAG (64-bit ELF64 EM_AARCH64)"
echo " Closure Mode   : $MALI_AOT_CLOSURE_MODE (depth=$MALI_AOT_CLOSURE_DEPTH, seed_file=${ACTIVE_SEED_FILE:-(core seeds only)})"
echo " LLVM Backend   : Static LLVM-21 (third_party/prebuilts/llvm-21)"
echo " Output Dir     : $OUT_DIR"
echo "================================================================================"

STAMP_FILE="$OUT_DIR/r54p3_lifted.o.stamp"
CUR_STAMP=""
if command -v sha256sum >/dev/null 2>&1; then
    SEED_HASH=""
    if [ -n "$ACTIVE_SEED_FILE" ] && [ -f "$ACTIVE_SEED_FILE" ]; then
        SEED_HASH="$(grep -v '^[[:space:]]*#' "$ACTIVE_SEED_FILE" | awk 'NF {print $1}' | sort -u | sha256sum | awk '{print $1}')"
    fi
    CUR_STAMP="$( { sha256sum "$ROOT_DIR/horizon2llvm/mali_llvm_passes.cpp" ${SO_PATH:+"$SO_PATH"}; echo "mode=$MALI_AOT_CLOSURE_MODE depth=$MALI_AOT_CLOSURE_DEPTH seeds=$SEED_HASH"; } | sha256sum | awk '{print $1}')"
fi
STAMP_OK=0
if [ -n "$CUR_STAMP" ] && [ -f "$STAMP_FILE" ] && [ "$(cat "$STAMP_FILE" 2>/dev/null || true)" = "$CUR_STAMP" ]; then
    STAMP_OK=1
fi

if [ -f "$OUT_DIR/r54p3_lifted.o" ] && [ "${MALI_AOT_FORCE_RELIFT:-0}" != "1" ] && [ "$STAMP_OK" -eq 1 ] && [ "$OUT_DIR/r54p3_lifted.o" -nt "$PASS_DRIVER" ] && [ "$OUT_DIR/r54p3_lifted.o" -nt "$ROOT_DIR/horizon2llvm/mali_llvm_passes.cpp" ]; then
    echo "[Cache] Reusing up-to-date $OUT_DIR/r54p3_lifted.o (set MALI_AOT_FORCE_RELIFT=1 to force re-lift)"
elif [ -n "$SO_PATH" ] && [ -f "$SO_PATH" ]; then
    export MALI_AOT_CHUNK_MB="${MALI_AOT_CHUNK_MB:-50}"
    export MALI_AOT_KEEP_LL="${MALI_AOT_KEEP_LL:-0}"
    SEED_ARGS=()
    if [ -n "$ACTIVE_SEED_FILE" ] && [ -f "$ACTIVE_SEED_FILE" ]; then
        SEED_ARGS=(--seed-file "$ACTIVE_SEED_FILE")
    fi
    MALI_AOT_JOBS=4 "$PASS_DRIVER" --lift-so "$SO_PATH" "$OUT_DIR" \
        --closure-mode "$MALI_AOT_CLOSURE_MODE" \
        --closure-depth "$MALI_AOT_CLOSURE_DEPTH" \
        "${SEED_ARGS[@]}" \
        --hook-Vulkan-calls --emit-ll-only
    export MALI_AOT_JOBS="20"
    "$PASS_DRIVER" --compile-ll-dir "$OUT_DIR/aot_ll_manifest.txt" "$OUT_DIR/r54p3_lifted.o" -j "$MALI_AOT_JOBS"
    if [ -n "$CUR_STAMP" ]; then
        echo "$CUR_STAMP" > "$STAMP_FILE"
    fi
elif [ ! -f "$OUT_DIR/r54p3_lifted.o" ]; then
    echo "[ERROR] Input $SO_ARG not found and $OUT_DIR/r54p3_lifted.o does not exist." >&2
    exit 1
fi

if [ "${MALI_AOT_KEEP_LL:-0}" = "0" ]; then
    rm -f "$OUT_DIR"/aot_*_unopt.ll \
          "$OUT_DIR"/aot_*_opt.ll \
          "$OUT_DIR"/aot_*.ll.opt.ll \
          "$OUT_DIR"/aot_worker_*.manifest \
          "$OUT_DIR/aot_ll_manifest.txt" \
          "$OUT_DIR/mali_mbs2_unopt.ll" "$OUT_DIR/mali_mbs2_opt.ll" \
          "$OUT_DIR"/mali_mbs2_chunk_*.ll "$OUT_DIR"/mali_mbs2_wrappers_*.ll
fi

echo "================================================================================"
echo " [SUCCESS] 64-Bit C++ LLVM AOT Lift Complete!"
echo "   x86-64 Object  : $OUT_DIR/r54p3_lifted.o"
echo "================================================================================"
