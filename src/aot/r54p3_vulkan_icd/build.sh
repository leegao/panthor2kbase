#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$ROOT_DIR/lib" "$ROOT_DIR/bin"

echo "[1/2] Compiling decompiled r54p3 Vulkan 1.3 ICD (libr54p3_vulkan_icd.so & libr54p3_vulkan_icd.a)..."
g++ -std=c++17 -O2 -fPIC -shared     -I"$ROOT_DIR/include"     "$ROOT_DIR/src/r54p3_vulkan_icd.cpp"     "$ROOT_DIR/../../../libmali_kbase_emu.so" -Wl,-rpath,"$ROOT_DIR/../../.." -ldl -lpthread     -o "$ROOT_DIR/lib/libr54p3_vulkan_icd.so"

g++ -std=c++17 -O2 -fPIC -c     -I"$ROOT_DIR/include"     "$ROOT_DIR/src/r54p3_vulkan_icd.cpp"     -o "$ROOT_DIR/lib/r54p3_vulkan_icd.o"
ar rcs "$ROOT_DIR/lib/libr54p3_vulkan_icd.a" "$ROOT_DIR/lib/r54p3_vulkan_icd.o"

cat << JSON_EOF > "$ROOT_DIR/lib/r54p3_decompiled_icd.x86_64.json"
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "$ROOT_DIR/lib/libr54p3_vulkan_icd.so",
        "api_version": "1.3.250"
    }
}
JSON_EOF
echo "[2/2] Built $ROOT_DIR/lib/libr54p3_vulkan_icd.so and r54p3_decompiled_icd.x86_64.json"
