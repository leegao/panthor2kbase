#!/usr/bin/env bash
# scripts/find_cmake.sh
# Robust CMake binary locator with auto-unpack fallback for isolated sandbox environments
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# 1. Environment variable override
if [ -n "${CMAKE_BIN:-}" ] && [ -x "${CMAKE_BIN}" ]; then
    echo "${CMAKE_BIN}"
    exit 0
fi
if [ -n "${CMAKE:-}" ] && [ -x "${CMAKE}" ]; then
    echo "${CMAKE}"
    exit 0
fi

# 2. System PATH
if command -v cmake >/dev/null 2>&1; then
    command -v cmake
    exit 0
fi

# 3. Known system, Android SDK, and gVisor sandbox search locations
SEARCH_PATHS=(
    "$ROOT_DIR/.cache/cmake/3.22.1/bin/cmake"
    "$ROOT_DIR/.cache/cmake/bin/cmake"
    "$ROOT_DIR/tools/cmake/3.22.1/bin/cmake"
    "$ROOT_DIR/tools/cmake/bin/cmake"
    "${ANDROID_HOME:-}/cmake/"*/bin/cmake
    "${ANDROID_NDK_HOME:-}/../../cmake/"*/bin/cmake
    "$HOME/Android/Sdk/cmake/"*/bin/cmake
    "$ROOT_DIR/../work/cmake_root/"*/bin/cmake
    "/opt/cmake/"*/bin/cmake
    "/usr/local/bin/cmake"
)

for cand in "${SEARCH_PATHS[@]}"; do
    if [ -x "$cand" ]; then
        export PATH="$(dirname "$cand"):$PATH"
        echo "$cand"
        exit 0
    fi
done

# 4. Fallback: Auto-extract predownloaded/cmake.zip if available
if [ -f "${ROOT_DIR}/predownloaded/cmake.zip" ]; then
    CACHE_DIR="${ROOT_DIR}/.cache"
    EXTRACT_DIR="${CACHE_DIR}/cmake"
    if [ ! -x "${EXTRACT_DIR}/3.22.1/bin/cmake" ]; then
        mkdir -p "${CACHE_DIR}" "${EXTRACT_DIR}"
        unzip -q -o "${ROOT_DIR}/predownloaded/cmake.zip" -d "${CACHE_DIR}" 2>/dev/null || \
        python3 -c "import zipfile; zipfile.ZipFile('${ROOT_DIR}/predownloaded/cmake.zip').extractall('${CACHE_DIR}')" 2>/dev/null || true
        if [ -d "${EXTRACT_DIR}/cmake/3.22.1" ] && [ ! -d "${EXTRACT_DIR}/3.22.1" ]; then
            mv "${EXTRACT_DIR}/cmake/3.22.1" "${EXTRACT_DIR}/3.22.1" 2>/dev/null || true
        fi
        chmod +x "${EXTRACT_DIR}"/3.22.1/bin/* 2>/dev/null || true
    fi
    if [ -x "${EXTRACT_DIR}/3.22.1/bin/cmake" ]; then
        export PATH="${EXTRACT_DIR}/3.22.1/bin:$PATH"
        echo "${EXTRACT_DIR}/3.22.1/bin/cmake"
        exit 0
    fi
fi

echo "ERROR: cmake could not be found in PATH, Android SDK, or predownloaded/cmake.zip." >&2
exit 1
