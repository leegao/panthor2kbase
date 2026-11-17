#!/usr/bin/env bash

# ==============================================================================
# File: scripts/bootstrap_agent_sandbox.sh
# Title: Complete Offline Bootstrap & Verification Suite for Gemini Task Agents
# Date: 2026-11-08
# ==============================================================================

set -euo pipefail

echo "======================================================================"
echo "[+] Starting panthor2kbase Sandbox Environment Bootstrap"
echo "======================================================================"

# **1. Environment & Path Auto-Discovery**

export WORKSPACE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${WORKSPACE_DIR}"

echo "[*] Workspace Root: ${WORKSPACE_DIR}"

# **Discover NDK Path**

if [ -z "${ANDROID_NDK_HOME:-}" ]; then
  if [ -d "/tmp/android-ndk-r27d" ]; then
    export ANDROID_NDK_HOME="/tmp/android-ndk-r27d"
  elif [ -d "${WORKSPACE_DIR}/../android-ndk-r27d" ]; then
    export ANDROID_NDK_HOME="$(cd "${WORKSPACE_DIR}/../android-ndk-r27d" && pwd)"
  else
    echo "[!] WARNING: ANDROID_NDK_HOME not found. AArch64 targets will be skipped."
  fi
fi
if [ -n "${ANDROID_NDK_HOME:-}" ]; then
  echo "[*] Using ANDROID_NDK_HOME: ${ANDROID_NDK_HOME}"
  export PATH="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin:${PATH}"
fi

# **Locate LLVM Shared Library for AOT Pass Engine**

HOST_LLVM_SO=$(find /usr/lib/llvm-*/lib /usr/lib/x86_64-linux-gnu -name "libLLVM*.so*" 2>/dev/null | head -n 1 || true)
if [ -n "${HOST_LLVM_SO}" ]; then
  echo "[*] Discovered Host LLVM Shared Library: ${HOST_LLVM_SO}"
  export MALI_LLVM_SO_PATH="${HOST_LLVM_SO}"
else
  echo "[!] WARNING: No host libLLVM.so found. AOT dynamic passes will fall back to stubs."
fi

# **Locate Mesa Lavapipe ICD**

if [ -f "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json" ]; then
  export VK_DRIVER_FILES="/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
  echo "[*] Configured Software Vulkan ICD: ${VK_DRIVER_FILES}"
fi

# **2. Workspace Clean-Up (Enforce <7.0 MiB Payload Limit Safeguard)**

echo "----------------------------------------------------------------------"
echo "[*] Cleaning legacy intermediate build artifacts..."
find . -name "*.o" -o -name "*.unopt.ll" -o -name "*.bc" | xargs rm -f

# **3. Trigger Full Project Build**

echo "----------------------------------------------------------------------"
echo "[*] Executing ./build_all.sh..."
chmod +x ./build_all.sh ./run_all_tests.sh
./build_all.sh

# **4. Execute Full Verification Suite**

echo "----------------------------------------------------------------------"
echo "[*] Executing ./run_all_tests.sh..."
./run_all_tests.sh

# **5. Report Final Status**

echo "======================================================================"
echo "[+] SUCCESS: Offline Sandbox Bootstrap & Verification Completed Cleanly!"
echo "======================================================================"
