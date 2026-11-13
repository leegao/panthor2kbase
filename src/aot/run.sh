#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "Usage: $0 <path/to/libGLES_mali.so>"
    exit 1
fi

SO_PATH="$(realpath "$1")"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ ! -f "$SO_PATH" ]; then
    echo "[ERROR] File not found: $SO_PATH"
    exit 1
fi

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

BUILD_DIR="${2:-$ROOT_DIR/build/$ELF_TAG}"
RUNNER="$BUILD_DIR/mali_aot_runner"

if [ ! -x "$RUNNER" ] || [ "$ROOT_DIR/src/mali_llvm_passes.cpp" -nt "$RUNNER" ]; then
    "$ROOT_DIR/translate.sh" "$SO_PATH" "$BUILD_DIR"
elif [ "$ROOT_DIR/../r54p3_vulkan_icd/mali_aot_runner.cpp" -nt "$RUNNER" ] || [ "$ROOT_DIR/../r54p3_vulkan_icd/mali_kbase_csf_lvp_harness.hpp" -nt "$RUNNER" ]; then
    g++ -O3 -fno-strict-aliasing "$ROOT_DIR/../r54p3_vulkan_icd/mali_aot_runner.cpp" "$BUILD_DIR/mali_mbs2_aot.o" -ldl -o "$RUNNER"
fi

exec "$RUNNER" "$SO_PATH"
