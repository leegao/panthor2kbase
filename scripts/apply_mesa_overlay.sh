#!/usr/bin/env bash
# scripts/apply_mesa_overlay.sh
# Idempotently copies all modified Mesa files from mesa_overlay/ and the
# panthor_kbase_adapter subproject into mesa/ without using fragile .patch files.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
MESA_DIR="${1:-${ROOT_DIR}/mesa}"

if [ ! -d "${MESA_DIR}" ]; then
    echo "ERROR: Mesa directory '${MESA_DIR}' does not exist." >&2
    exit 1
fi

mkdir -p "${ROOT_DIR}/shims/include" && cp -rf "${ROOT_DIR}/src/mesa_shims/"* "${ROOT_DIR}/shims/include/"
cp -rf "${ROOT_DIR}/mesa_overlay/"* "${MESA_DIR}/"
mkdir -p "${MESA_DIR}/src/panfrost/uapi_adapter" \
         "${MESA_DIR}/subprojects/panthor_kbase_adapter/include" \
         "${MESA_DIR}/subprojects/panthor_kbase_adapter/src/adapter"
cp -rf "${ROOT_DIR}/include/"* "${MESA_DIR}/src/panfrost/uapi_adapter/"
cp -rf "${ROOT_DIR}/src/adapter/"* "${MESA_DIR}/src/panfrost/uapi_adapter/"
cp -rf "${ROOT_DIR}/include/"* "${MESA_DIR}/subprojects/panthor_kbase_adapter/include/"
cp -rf "${ROOT_DIR}/src/adapter/"* "${MESA_DIR}/subprojects/panthor_kbase_adapter/src/adapter/"
cp -f "${ROOT_DIR}/mesa_subproject/meson.build" "${MESA_DIR}/subprojects/panthor_kbase_adapter/meson.build"
echo "Idempotently applied mesa_overlay/ and subprojects/panthor_kbase_adapter/ -> ${MESA_DIR}"
