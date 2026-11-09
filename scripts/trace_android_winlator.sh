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
TRACER_SO="$ROOT_DIR/build-android/libmali_uapi_tracer.so"
[ ! -f "$TRACER_SO" ] && [ -f "$ROOT_DIR/build-android/adapter/libmali_uapi_tracer.so" ] && TRACER_SO="$ROOT_DIR/build-android/adapter/libmali_uapi_tracer.so"
[ ! -f "$TRACER_SO" ] && [ -f "$ROOT_DIR/build-android-tools/libmali_uapi_tracer_android_aarch64.so" ] && TRACER_SO="$ROOT_DIR/build-android-tools/libmali_uapi_tracer_android_aarch64.so"
REMOTE_DIR="/data/local/tmp"
REMOTE_TRACE="/sdcard/Download/mali_uapi_trace.jsonl"

if [ ! -f "$TRACER_SO" ]; then
    echo "[Info] Building Android aarch64 tracer library first..."
    "$ROOT_DIR/build_all.sh"
fi

echo "================================================================================"
echo " Android / Winlator /dev/mali0 UAPI + CSF Ring Tracer Utility"
echo "================================================================================"
echo " 1. Pushing tracer to device ($REMOTE_DIR/libmali_uapi_tracer.so):"
echo "      adb push \"$TRACER_SO\" $REMOTE_DIR/libmali_uapi_tracer.so"
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
