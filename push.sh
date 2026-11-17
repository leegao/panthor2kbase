#!/usr/bin/env bash
set -euo pipefail

FORCE_PUSH=0
if [ "${1:-}" = "--force" ] || [ "${1:-}" = "-f" ]; then
	FORCE_PUSH=1
fi

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$ROOT_DIR/build-android/scratch"

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
	adb push "$file" /data/local/tmp/
	adb shell run-as com.ludashi.benchmark "cp /data/local/tmp/$(basename "$file") files/imagefs/usr/lib/$(basename "$file")"
	adb shell run-as com.ludashi.benchmark "ls -lh files/imagefs/usr/lib/$(basename "$file")"
	echo "$cur_sum" > "$ROOT_DIR/build-android/scratch/$tag"
}

push_vvl() {
	adb push "$1" /data/local/tmp/
	adb shell run-as com.ludashi.benchmark "cp /data/local/tmp/$1 files/imagefs/usr/share/vulkan/explicit_layer.d/$1"
	adb shell run-as com.ludashi.benchmark "ls -lh files/imagefs/usr/share/vulkan/explicit_layer.d/$1"
}

bash compile.sh

if [ -f "$ROOT_DIR/build-android/libvulkan_panfrost.so" ]; then
	push "$ROOT_DIR/build-android/libvulkan_panfrost.so"
elif [ -d "$ROOT_DIR/build-android/mesa/src/panfrost/vulkan" ]; then
	cd "$ROOT_DIR/build-android/mesa/src/panfrost/vulkan"
	push libvulkan_panfrost.so
	cd - >/dev/null
elif [ -d "$ROOT_DIR/build-android/src/panfrost/vulkan" ]; then
	cd "$ROOT_DIR/build-android/src/panfrost/vulkan"
	push libvulkan_panfrost.so
	cd - >/dev/null
fi
# if [ -f libvulkan_wrapper.so ]; then
# 	push libvulkan_wrapper.so
# fi
