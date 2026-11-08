#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "Usage: $0 <path/to/libGLES_mali.so> [output_dir]"
    exit 1
fi

SO_PATH="$(realpath "$1")"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ ! -f "$SO_PATH" ]; then
    echo "[ERROR] File not found: $SO_PATH"
    exit 1
fi

# Auto-detect installed host libLLVM shared library across Arch, Ubuntu, Debian, Fedora (LLVM 14..22+)
detect_host_libllvm() {
    if [ -n "${MALI_HOST_LIBLLVM:-}" ] && [ -f "${MALI_HOST_LIBLLVM}" ]; then
        echo "${MALI_HOST_LIBLLVM}"
        return 0
    fi
    for cfg in llvm-config llvm-config-22 llvm-config-21 llvm-config-20 llvm-config-19 llvm-config-18 llvm-config-17 llvm-config-16 llvm-config-15 llvm-config-14; do
        if command -v "$cfg" >/dev/null 2>&1; then
            local libdir
            libdir="$("$cfg" --libdir 2>/dev/null || true)"
            if [ -n "$libdir" ] && [ -d "$libdir" ]; then
                local found
                found="$(ls -1 "$libdir"/libLLVM*.so* 2>/dev/null | head -n 1 || true)"
                if [ -n "$found" ]; then
                    echo "$found"
                    return 0
                fi
            fi
        fi
    done
    local cand
    cand="$(ls -1 /usr/lib/libLLVM*.so* /usr/lib64/libLLVM*.so* /usr/lib/x86_64-linux-gnu/libLLVM*.so* /usr/lib/llvm-*/lib/libLLVM*.so* 2>/dev/null | sort -V | tail -n 1 || true)"
    if [ -n "$cand" ]; then
        echo "$cand"
        return 0
    fi
    return 1
}

HOST_LIBLLVM="$(detect_host_libllvm || true)"
if [ -n "$HOST_LIBLLVM" ]; then
    export MALI_HOST_LIBLLVM="$HOST_LIBLLVM"
    echo "[LLVM] Auto-detected host LLVM library: $MALI_HOST_LIBLLVM"
else
    echo "[WARN] No host libLLVM*.so* detected in standard paths; mali_llvm_pass_driver will probe via dlopen()."
fi

# Verify ELF64 EM_AARCH64 and extract unique <driver_version>-<build_id_short> tag
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

OUT_DIR="${2:-$ROOT_DIR/build/$ELF_TAG}"
mkdir -p "$OUT_DIR" "$ROOT_DIR/bin"

PASS_DRIVER="$ROOT_DIR/bin/mali_llvm_pass_driver"
NEEDS_REBUILD=0
if [ ! -x "$PASS_DRIVER" ] || [ "$ROOT_DIR/src/mali_llvm_passes.cpp" -nt "$PASS_DRIVER" ]; then
    NEEDS_REBUILD=1
elif ldd "$PASS_DRIVER" 2>/dev/null | grep -q "not found"; then
    echo "[Build] Existing $PASS_DRIVER has missing shared library dependencies on this host; rebuilding..."
    NEEDS_REBUILD=1
fi

if [ "$NEEDS_REBUILD" -eq 1 ]; then
    echo "[Build] Compiling 6-Pass C++ LLVM Lifter & Lowerer (src/mali_llvm_passes.cpp -> bin/mali_llvm_pass_driver)..."
    g++ -O3 "$ROOT_DIR/src/mali_llvm_passes.cpp" -ldl -o "$PASS_DRIVER"
fi

echo "================================================================================"
echo " Mali 64-Bit AArch64 -> x86-64 Linux C++ LLVM 6-Pass AOT Translation Pipeline"
echo " Input Binary   : $SO_PATH"
echo " Driver Build   : $ELF_TAG (64-bit ELF64 EM_AARCH64)"
echo " Host libLLVM   : ${MALI_HOST_LIBLLVM:-dynamic dlopen}"
echo " Output Dir     : $OUT_DIR"
echo "================================================================================"

"$PASS_DRIVER" --lift-so "$SO_PATH" "$OUT_DIR"

g++ -O3 -fno-strict-aliasing "$ROOT_DIR/src/mali_aot_runner.cpp" "$OUT_DIR/mali_mbs2_aot.o" -L"$ROOT_DIR/../.." -lmali_kbase_emu -Wl,-rpath,\$ORIGIN/../../..:\$ORIGIN -ldl -o "$OUT_DIR/mali_aot_runner"
g++ -O3 -std=c++17 -fno-strict-aliasing -shared -fPIC "$ROOT_DIR/src/mali_vulkan_icd.cpp" "$OUT_DIR/mali_mbs2_aot.o" -L"$ROOT_DIR/../.." -lmali_kbase_emu -Wl,-rpath,\$ORIGIN/../../..:\$ORIGIN -ldl -lm -o "$OUT_DIR/libvulkan_mali_aot_icd.so"
cat << JSON_EOF > "$OUT_DIR/mali_aot_icd.x86_64.json"
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "./libvulkan_mali_aot_icd.so",
        "api_version": "1.3.250"
    }
}
JSON_EOF

echo "================================================================================"
echo " [SUCCESS] 64-Bit C++ LLVM AOT Translation Complete!"
echo "   Unoptimized IR : $OUT_DIR/mali_mbs2_unopt.ll"
echo "   Optimized IR   : $OUT_DIR/mali_mbs2_opt.ll"
echo "   x86-64 Object  : $OUT_DIR/mali_mbs2_aot.o"
echo "   Runner Binary  : $OUT_DIR/mali_aot_runner"
echo "   Vulkan ICD     : $OUT_DIR/libvulkan_mali_aot_icd.so"
echo "================================================================================"
