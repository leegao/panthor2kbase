#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ $# -lt 1 ]; then
    echo "Usage: $0 <path/to/libGLES_mali.so> [more_drivers.so ...]"
    exit 1
fi

exec python3 "$ROOT_DIR/scripts/auto_bringup.py" "$@"
