#!/usr/bin/env bash
# ============================================================================
# run_all_tests.sh
# Master End-to-End Verification & Artifact Generation Runner.
# Executes every unit test, 20-sample Vulkan suite, vkcube WSI test, Ludashi vs.
# Adapter ring-slot & UAPI diff, OR-Tools CP-SAT formal proof, Z3 symbolic
# equivalence proof, and AArch64 machine-code lifter audit.
# ============================================================================
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

mkdir -p artifacts

echo "================================================================================"
echo " [Suite 1/8] Edge-Case Unit Tests & mali_kbase.ko Flag Invariants               "
echo "================================================================================"
./tests/unit_tests_edge_cases

echo "================================================================================"
echo " [Suite 2/8] 20 Vulkan-Samples & Host PanVK Equivalence Suite (/dev/mali0 Emu)  "
echo "================================================================================"
./tests/run_vulkan_samples_and_equivalence_suite

echo "================================================================================"
echo " [Suite 3/8] Ludashi panvk-kbase-android (v26.3.1) & Host Patched PanVK Harness "
echo "================================================================================"
./tests/test_ludashi_panvk_aot_and_host
./tests/panprobe/panprobe_runner_suite

echo "================================================================================"
echo " [Suite 4/8] Khronos WSI 3D vkcube End-to-End (AOT & Decompiled r54p3 ICDs)    "
echo "================================================================================"
./src/aot/bin/vkcube
./src/aot/bin/vkcube src/aot/r54p3_vulkan_icd/lib/r54p3_decompiled_icd.x86_64.json
./tests/test_wayland_and_vkgears_e2e

echo "================================================================================"
echo " [Suite 5/8] Khronos 20 Vulkan-Samples Compliance Harness (System Loader)      "
echo "================================================================================"
./src/aot/bin/test_vulkan_samples_compliance
./src/aot/bin/test_vulkan_samples_compliance src/aot/r54p3_vulkan_icd/lib/r54p3_decompiled_icd.x86_64.json

echo "================================================================================"
echo " [Suite 6/8] UAPI & CSF Ring Trace Diff (winlator_log.txt vs. Ludashi panvk)   "
echo "================================================================================"
python3 scripts/uapi_trace_and_diff.py

echo "================================================================================"
echo " [Suite 7/8] OR-Tools CP-SAT & Z3 Symbolic Equivalence Verifiers               "
echo "================================================================================"
python3 src/verifier/ortools_uapi_verifier.py
python3 src/verifier/kbase_kernel_symbolic_verifier.py
if [ -f mali_kbase.ko ]; then
    python3 src/verifier/aarch64_mc_lifter.py mali_kbase.ko
fi

echo "================================================================================"
echo " [Suite 8/8] Conformance Report Generation                                      "
echo "================================================================================"
./scripts/run_conformance.sh

echo "================================================================================"
echo " [ALL SUITES PASSED] Every Test, Verifier, and Equivalence Artifact Succeeded! "
echo "================================================================================"
