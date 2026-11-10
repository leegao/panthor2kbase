#!/usr/bin/env bash
# scripts/apply_mesa_overlay.sh
# Idempotently performs 2-way synchronization between mesa_overlay/ and mesa/
# (via scripts/sync_mesa_overlay.py) and syncs include/, src/adapter/, and
# mesa_subproject/ into mesa/ (and shims/include) without modifying destination
# timestamps when contents are unchanged.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
MESA_DIR="${1:-${ROOT_DIR}/mesa}"

sync_tree_if_changed() {
   local src_dir="$1"
   local dst_dir="$2"
   [ -d "$src_dir" ] || return 0
   mkdir -p "$dst_dir"
   while IFS= read -r -d '' src_file; do
      local rel="${src_file#$src_dir/}"
      local dst_file="$dst_dir/$rel"
      local need_copy=0
      if [ ! -f "$dst_file" ]; then
         mkdir -p "$(dirname "$dst_file")"
         need_copy=1
      elif ! cmp -s "$src_file" "$dst_file"; then
         need_copy=1
      fi
      if [ "$need_copy" -eq 1 ]; then
         cp -f "$src_file" "$dst_file"
         touch "$dst_file"
         echo "  [sync] Updated $dst_file"
      fi
   done < <(find "$src_dir" -type f -print0)
}

# 1. Sync shims headers
mkdir -p "${ROOT_DIR}/shims/include"
sync_tree_if_changed "${ROOT_DIR}/src/mesa_shims" "${ROOT_DIR}/shims/include"
sync_tree_if_changed "${ROOT_DIR}/include" "${ROOT_DIR}/shims/include"

# 2. 2-way sync mesa_overlay/ <-> mesa/ and apply subprojects into mesa/
if [ -d "${MESA_DIR}" ]; then
   if [ -f "${SCRIPT_DIR}/sync_mesa_overlay.py" ] && command -v python3 >/dev/null 2>&1; then
      python3 "${SCRIPT_DIR}/sync_mesa_overlay.py" --mesa-dir "${MESA_DIR}" --overlay-dir "${ROOT_DIR}/mesa_overlay" --mode auto --quiet
   fi
   sync_tree_if_changed "${ROOT_DIR}/mesa_overlay" "${MESA_DIR}"

   mkdir -p "${MESA_DIR}/src/panfrost/uapi_adapter" \
            "${MESA_DIR}/subprojects/panthor_kbase_adapter/include" \
            "${MESA_DIR}/subprojects/panthor_kbase_adapter/src/adapter"

   sync_tree_if_changed "${ROOT_DIR}/include" "${MESA_DIR}/src/panfrost/uapi_adapter"
   sync_tree_if_changed "${ROOT_DIR}/src/adapter" "${MESA_DIR}/src/panfrost/uapi_adapter"
   sync_tree_if_changed "${ROOT_DIR}/include" "${MESA_DIR}/subprojects/panthor_kbase_adapter/include"
   sync_tree_if_changed "${ROOT_DIR}/src/adapter" "${MESA_DIR}/subprojects/panthor_kbase_adapter/src/adapter"

   if [ -f "${ROOT_DIR}/mesa_subproject/meson.build" ]; then
      target_meson="${MESA_DIR}/subprojects/panthor_kbase_adapter/meson.build"
      if [ ! -f "$target_meson" ] || ! cmp -s "${ROOT_DIR}/mesa_subproject/meson.build" "$target_meson"; then
         cp -f "${ROOT_DIR}/mesa_subproject/meson.build" "$target_meson"
         touch "$target_meson"
         echo "  [sync] Updated $target_meson"
      fi
   fi
   echo "Idempotently applied mesa_overlay and subproject adapter -> ${MESA_DIR}"
elif [ $# -ge 1 ]; then
   echo "ERROR: Mesa directory '${MESA_DIR}' does not exist." >&2
   exit 1
fi
