#!/usr/bin/env bash
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
g++ -std=c++17 -O2 -fPIC -shared -Wl,-Bsymbolic -Wl,-Bsymbolic-functions     "$DIR/bcn_layer_host.cpp" -o "$DIR/libVkLayer_BCN_bcn_layer.so" -lpthread
g++ -std=c++17 -O2 -fPIC -shared -Wl,-Bsymbolic -Wl,-Bsymbolic-functions     "$DIR/compat_layer_host.cpp" -o "$DIR/libVkLayer_COMPAT_dxvk_mali_compat.so" -lpthread
