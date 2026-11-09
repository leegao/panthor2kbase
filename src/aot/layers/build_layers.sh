#!/usr/bin/env bash
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
g++ -std=c++17 -O2 -fPIC -shared -Wl,-Bsymbolic -Wl,-Bsymbolic-functions \
    "$DIR/bcn_layer_host.cpp" -o "$DIR/libVkLayer_BCN_bcn_layer.so" -lpthread
g++ -std=c++17 -O2 -fPIC -shared -Wl,-Bsymbolic -Wl,-Bsymbolic-functions \
    "$DIR/compat_layer_host.cpp" -o "$DIR/libVkLayer_COMPAT_dxvk_mali_compat.so" -lpthread

cat << JSON_EOF > "$DIR/VkLayer_BCN_bcn_layer.json"
{
    "file_format_version": "1.2.0",
    "layer": {
        "name": "VK_LAYER_BCN_BCnLayer",
        "type": "GLOBAL",
        "library_path": "./libVkLayer_BCN_bcn_layer.so",
        "api_version": "1.3.0",
        "implementation_version": "1",
        "description": "BCn Emulation Layer for Mobile GPUs (Host AOT Build)"
    }
}
JSON_EOF

cat << JSON_EOF > "$DIR/VkLayer_COMPAT_dxvk_mali_compat.json"
{
    "file_format_version": "1.2.0",
    "layer": {
        "name": "VK_LAYER_COMPAT_DxvkMaliCompatLayer",
        "type": "GLOBAL",
        "library_path": "./libVkLayer_COMPAT_dxvk_mali_compat.so",
        "api_version": "1.3.0",
        "implementation_version": "1",
        "description": "DXVK 1.10.3 Feature & Limit Compatibility Layer for Mali r54p3 AOT ICD"
    }
}
JSON_EOF

g++ -std=c++17 -O2 -fPIC -shared -Wl,-Bsymbolic -Wl,-Bsymbolic-functions \
    -I"$DIR/../../../include" -I"$DIR/../include" \
    "$DIR/panthor_vk_capture_layer.cpp" -o "$DIR/libVkLayer_PANTHOR_capture.so" -ldl -lpthread
