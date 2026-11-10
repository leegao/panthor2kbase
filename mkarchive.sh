#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
REMOVE=true
SLIM=false
ARCHIVE=false
GEMINI_MODE=0
TARGET_FILE="archive.zip"

ARGS=()
for arg in "$@"; do
    case "$arg" in
        -k|--keep)
            REMOVE=false
            ;;
        -s|--slim)
            SLIM=true
            ;;
        -a|--archive)
            SLIM=true
            ARCHIVE=true
            TARGET_FILE="archives/archive-$TIMESTAMP.zip"
            mkdir -p archives/
            ;;
        -g|--gemini)
            GEMINI_MODE=1
            ;;
        *)
            ARGS+=("$arg")
            ;;
    esac
done

if [ "${#ARGS[@]}" -gt 0 ]; then
    TARGET_FILE="${ARGS[0]}"
fi
if $REMOVE; then
    rm -f "$TARGET_FILE"
fi

if [ "$GEMINI_MODE" -eq 1 ]; then
    python3 "$SCRIPT_DIR/tools/mkarchive.py" --gemini "$TARGET_FILE"
elif command -v zip >/dev/null 2>&1; then
    FLAGS=(
        -9 -y -r "$TARGET_FILE" .
        -x ".cache/*" "build*/*" "archives/*"
        "llvm.zip" "include.zip" "AGENTS.md" "CLAUDE.md" "External/*"
        "kittytoy/*" "screenshots/*" "mesa/*" "archive.zip" "*.zip" "*.so" "*.o" "*.whl" "*.ko"
        "sandbox_bundles/*" "predownloaded/*" "src/aot/bin/*" "src/aot/horizon2llvm/horizon2llvm"
    )
    if $SLIM; then
        FLAGS+=(-x "*.ko")
        FLAGS+=(-x "data/*")
        FLAGS+=(-x "*.pyc")
        FLAGS+=(-x "*.zip")
        FLAGS+=(-x ".git/*")
    fi
    zip "${FLAGS[@]}"
else
    python3 "$SCRIPT_DIR/tools/mkarchive.py" "$TARGET_FILE"
fi

ls -lh "$TARGET_FILE"
