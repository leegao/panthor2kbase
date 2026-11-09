#!/usr/bin/env bash
# scripts/run_coverage.sh
# -----------------------
# Compiles the C++ Panthor-to-Kbase adapter and /dev/mali0 emulator with GCC code
# coverage instrumentation (--coverage) via CMake, executes the full Vulkan
# sample and unit/integration test suites, generates gcov reports,
# and calculates line coverage statistics.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT_DIR}"

CMAKE_BIN="$("${SCRIPT_DIR}/find_cmake.sh")"
COVERAGE_DIR="${ROOT_DIR}/coverage_output"
COVERAGE_BUILD_DIR="${ROOT_DIR}/build-coverage"

mkdir -p "${COVERAGE_DIR}" "${ROOT_DIR}/artifacts"
rm -f "${COVERAGE_DIR}"/* "${ROOT_DIR}"/*.gcda "${ROOT_DIR}"/*.gcno "${ROOT_DIR}"/*.gcov

echo "================================================================================"
echo " Building Panthor-Kbase Suite with Code Coverage Instrumentation via CMake"
echo "================================================================================"

"$CMAKE_BIN" -S "${ROOT_DIR}" -B "${COVERAGE_BUILD_DIR}" \
    -DPANTHOR_KBASE_ENABLE_COVERAGE=ON \
    -DCMAKE_BUILD_TYPE=Debug

"$CMAKE_BIN" --build "${COVERAGE_BUILD_DIR}" --target \
    panthor_kbase_adapter \
    mali_kbase_emu \
    vulkan_panfrost_patched \
    run_vulkan_samples_and_equivalence_suite \
    unit_tests_edge_cases \
    panprobe_runner_suite \
    test_ludashi_panvk_aot_and_host \
    test_wayland_and_vkgears_e2e \
    test_dxvk_aot_layers_e2e \
    -j"$(nproc)"

echo "================================================================================"
echo " Executing Test Suites under Coverage Profiling"
echo "================================================================================"

RUN_TEST() {
    local t="$1"
    if [ -x "${COVERAGE_BUILD_DIR}/tests/$t" ]; then
        "${COVERAGE_BUILD_DIR}/tests/$t"
    elif [ -x "${COVERAGE_BUILD_DIR}/$t" ]; then
        "${COVERAGE_BUILD_DIR}/$t"
    elif [ -x "${ROOT_DIR}/tests/$t" ]; then
        "${ROOT_DIR}/tests/$t"
    fi
}

RUN_TEST run_vulkan_samples_and_equivalence_suite
RUN_TEST unit_tests_edge_cases
RUN_TEST panprobe_runner_suite
RUN_TEST test_ludashi_panvk_aot_and_host
RUN_TEST test_wayland_and_vkgears_e2e
RUN_TEST test_dxvk_aot_layers_e2e

echo "================================================================================"
echo " Generating Gcov Coverage Reports & Parsing Metrics"
echo "================================================================================"

python3 - << PYEOF
import os, glob, subprocess

cov_dir = "${COVERAGE_DIR}"
build_dir = "${COVERAGE_BUILD_DIR}"
gcda_files = glob.glob(os.path.join(build_dir, "**", "*.gcda"), recursive=True)

for f in gcda_files:
    obj_dir = os.path.dirname(f)
    subprocess.run(["gcov", "-b", "-c", "-o", obj_dir, f], stdout=subprocess.PIPE, stderr=subprocess.PIPE)

for g in glob.glob("*.gcov"):
    os.replace(g, os.path.join(cov_dir, g))
PYEOF

python3 - << 'PYEOF'
import os, glob, json

cov_dir = "coverage_output"
gcov_files = [
    f for f in os.listdir(cov_dir)
    if (f.endswith(".c.gcov") or f.endswith(".cpp.gcov"))
    and not f.startswith("usr#")
    and f != "panfrost_valhall_disasm_gen.c.gcov"
    and not f.startswith("panthor_vk_capture_layer")
]
coverage_data = {}
total_lines = 0
total_exec = 0

header = f" {'SOURCE FILE':<42} | {'EXEC / TOTAL':<16} | {'COVERAGE %':<10}"
print("\n" + "="*80)
print(header)
print("="*80)

for g in sorted(gcov_files):
    src_name = g.replace(".gcov", "")
    lines_total = 0
    lines_exec = 0
    with open(os.path.join(cov_dir, g), "r", errors="replace") as fp:
        for line in fp:
            parts = line.split(":", 2)
            if len(parts) >= 2:
                count_str = parts[0].strip()
                if count_str == "-":
                    continue
                elif count_str == "#####":
                    lines_total += 1
                else:
                    try:
                        c = int(count_str)
                        lines_total += 1
                        if c > 0:
                            lines_exec += 1
                    except ValueError:
                        pass
    pct = (lines_exec / lines_total * 100.0) if lines_total > 0 else 0.0
    coverage_data[src_name] = {
        "lines_total": lines_total,
        "lines_executed": lines_exec,
        "coverage_pct": round(pct, 2)
    }
    total_lines += lines_total
    total_exec += lines_exec
    row = f" {src_name:<42} | {lines_exec:>6} / {lines_total:<7} | {pct:>8.2f}%"
    print(row)

total_pct = (total_exec / total_lines * 100.0) if total_lines > 0 else 0.0
coverage_data["_TOTAL_"] = {
    "lines_total": total_lines,
    "lines_executed": total_exec,
    "coverage_pct": round(total_pct, 2)
}
print("="*80)
total_row = f" {'OVERALL TOTAL':<42} | {total_exec:>6} / {total_lines:<7} | {total_pct:>8.2f}%"
print(total_row)
print("="*80)

summary_json = os.path.join("artifacts", "coverage_summary.json")
with open(summary_json, "w") as fp:
    json.dump(coverage_data, fp, indent=2)
print(f"Saved coverage summary -> {summary_json}")

if total_pct < 70.0:
    print(f"ERROR: Overall coverage {total_pct:.2f}% is below required threshold 70.0%!")
    exit(1)
print(f"Coverage threshold passed: {total_pct:.2f}% >= 70.0%")
PYEOF
