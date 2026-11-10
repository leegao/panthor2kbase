#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
DEFAULT_SO="$ROOT_DIR/libGLES_mali.so"
if [ ! -f "$DEFAULT_SO" ] && [ -f "$REPO_ROOT/libGLES_mali.so" ]; then
    DEFAULT_SO="$REPO_ROOT/libGLES_mali.so"
fi

SO_PATH="${1:-$DEFAULT_SO}"
BACKEND_MODE="${2:-llvmpipe}"

if [ ! -f "$SO_PATH" ]; then
    echo "[ERROR] ARM64 driver binary not found: $SO_PATH"
    exit 1
fi

SO_PATH="$(realpath "$SO_PATH")"

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

BUILD_DIR="$ROOT_DIR/build/$ELF_TAG"
mkdir -p "$BUILD_DIR" "$ROOT_DIR/bin"

if [ ! -f "$BUILD_DIR/mali_mbs2_aot.o" ]; then
    "$ROOT_DIR/translate.sh" "$SO_PATH" "$BUILD_DIR"
fi

ICD_SO="$BUILD_DIR/libvulkan_mali_aot_icd.so"
ICD_JSON="$BUILD_DIR/mali_aot_icd.x86_64.json"
CLIENT_BIN="$ROOT_DIR/bin/test_vulkan_icd_client"
VKCUBE_BIN="$ROOT_DIR/bin/vkcube"

echo "[1/4] Building AOT Vulkan ICD ($ICD_SO)..."
g++ -O3 -std=c++17 -fno-strict-aliasing -shared -fPIC \
    "$ROOT_DIR/src/mali_vulkan_icd.cpp" \
    "$BUILD_DIR/mali_mbs2_aot.o" \
    -o "$ICD_SO" "$ROOT_DIR/../../libmali_kbase_emu.so" -Wl,-rpath,"$ROOT_DIR/../.." -ldl -lm

cat << JSON_EOF > "$ICD_JSON"
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "$ICD_SO",
        "api_version": "1.3.250"
    }
}
JSON_EOF

echo "[2/4] Building & Running E2E Vulkan ICD Client ($CLIENT_BIN)..."
g++ -O3 -std=c++17 -fno-strict-aliasing \
    "$ROOT_DIR/tests/test_vulkan_icd_client.cpp" \
    -o "$CLIENT_BIN" "$ROOT_DIR/../../libmali_kbase_emu.so" -Wl,-rpath,"$ROOT_DIR/../.." -ldl -lm

export VK_ICD_FILENAMES="$ICD_JSON"
export MALI_AOT_DRIVER_SO="$SO_PATH"
export MALI0_EMU_BACKEND="$BACKEND_MODE"

"$CLIENT_BIN" "$ICD_JSON" "$SO_PATH"

echo ""
echo "[3/4] Building & Running Khronos WSI 3D vkcube ($VKCUBE_BIN)..."
g++ -O3 -std=c++17 -fno-strict-aliasing -I"$ROOT_DIR/include" \
    "$ROOT_DIR/tests/vkcube_wsi_e2e.cpp" \
    -o "$VKCUBE_BIN" -ldl -lpthread -lm
"$VKCUBE_BIN" "$ICD_JSON" "$SO_PATH"

echo ""
echo "[4/4] Building decompiled r54p3_vulkan_icd & running 10-suite Khronos Vulkan-Samples compliance..."
"$ROOT_DIR/r54p3_vulkan_icd/build.sh"
if [ ! -f "$ROOT_DIR/bin/test_vulkan_samples_compliance" ] || [ "$ROOT_DIR/tests/test_vulkan_samples_compliance.cpp" -nt "$ROOT_DIR/bin/test_vulkan_samples_compliance" ]; then
    g++ -O2 -std=c++17 -I"$ROOT_DIR/include" -I"$ROOT_DIR/r54p3_vulkan_icd/include" \
        "$ROOT_DIR/tests/test_vulkan_samples_compliance.cpp" -ldl -lpthread -o "$ROOT_DIR/bin/test_vulkan_samples_compliance"
fi
VK_ICD_FILENAMES="$ROOT_DIR/r54p3_vulkan_icd/lib/r54p3_decompiled_icd.x86_64.json" "$ROOT_DIR/bin/test_vulkan_samples_compliance" "$ROOT_DIR/r54p3_vulkan_icd/lib/r54p3_decompiled_icd.x86_64.json"
VK_ICD_FILENAMES="$ICD_JSON" "$ROOT_DIR/bin/test_vulkan_samples_compliance" "$ICD_JSON"
