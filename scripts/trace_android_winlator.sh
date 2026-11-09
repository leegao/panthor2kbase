#!/usr/bin/env bash
# ============================================================================
# scripts/trace_android_winlator.sh
# Helper utility to push `libmali_uapi_tracer_android_aarch64.so` to an Android
# device via `adb` and trace `/dev/mali0` UAPI + CSF ring buffers under Winlator
# (works with Ludashi `panvk-kbase-android`, our patched `panvk`, or `libGLES_mali.so`),
# then pull the trace and diff it against `winlator_log.txt`.
# ============================================================================
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$ROOT_DIR/build-android/scratch"

FORCE_PUSH=0
if [ "${1:-}" = "--force" ] || [ "${1:-}" = "-f" ]; then
    FORCE_PUSH=1
fi

TRACER_SO="$ROOT_DIR/build-android/libmali_uapi_tracer.so"
[ ! -f "$TRACER_SO" ] && [ -f "$ROOT_DIR/build-android/adapter/libmali_uapi_tracer.so" ] && TRACER_SO="$ROOT_DIR/build-android/adapter/libmali_uapi_tracer.so"
REMOTE_DIR="/data/local/tmp"
REMOTE_TRACE="/sdcard/Download/mali_uapi_trace.jsonl"

if [ ! -f "$TRACER_SO" ]; then
    echo "[Info] Building Android aarch64 tracer library first..."
    "$ROOT_DIR/build_all.sh"
fi

push() {
    local file="$1"
    if [ ! -f "$file" ]; then
        return 0
    fi
    local tag
    tag="$(basename "$file").pushed.cksum"
    local cur_sum
    cur_sum="$(cksum "$file" | awk '{print $1 "-" $2}')"
    if [ "$FORCE_PUSH" -eq 0 ] && [ -f "$ROOT_DIR/build-android/scratch/$tag" ] && [ "$(cat "$ROOT_DIR/build-android/scratch/$tag")" = "$cur_sum" ]; then
        echo "  [push] $file unchanged (cksum $cur_sum); skipping adb push (use --force to override)"
        return 0
    fi
    adb push "$file" "$REMOTE_DIR/"
    if adb shell run-as com.ludashi.benchmark id >/dev/null 2>&1; then
        adb shell run-as com.ludashi.benchmark "cp $REMOTE_DIR/$(basename "$file") files/imagefs/usr/lib/$(basename "$file")"
        adb shell run-as com.ludashi.benchmark "ls -lh files/imagefs/usr/lib/$(basename "$file")"
    fi
    echo "$cur_sum" > "$ROOT_DIR/build-android/scratch/$tag"
}

echo "================================================================================"
echo " Android / Winlator /dev/mali0 UAPI + CSF Ring Tracer Utility"
echo "================================================================================"
echo " 1. Pushing tracer to device ($REMOTE_DIR/libmali_uapi_tracer.so):"
push "$TRACER_SO"
echo ""
echo " 2. In Winlator Container Environment Variables, add:"
echo "      MALI_UAPI_TRACE_OUT=$REMOTE_TRACE"
echo "      LD_PRELOAD=$REMOTE_DIR/libmali_uapi_tracer.so"
echo "    (Or for our patched panvk verbose log:)"
echo "      PANVK_ADAPTER_LOG_LEVEL=TRACE"
echo "      PANVK_ADAPTER_LOG_FILE=/sdcard/Download/winlator_log.txt"
echo ""
echo " 3. After running vkcube.exe in Winlator, pull and diff the trace:"
echo "      adb pull $REMOTE_TRACE $ROOT_DIR/artifacts/device_mali_uapi_trace.jsonl"
echo "      python3 $ROOT_DIR/scripts/uapi_trace_and_diff.py /path/to/winlator_log.txt"
echo "================================================================================"
