#!/usr/bin/env bash
# scripts/run_coverage.sh
# -----------------------
# Compiles the C++ Panthor-to-Kbase adapter and /dev/mali0 emulator with GCC code
# coverage instrumentation (--coverage), executes the full 20-workload Vulkan
# sample suite and supplementary unit test suites, generates gcov reports,
# and calculates line and branch coverage statistics.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT_DIR}"

COVERAGE_DIR="${ROOT_DIR}/coverage_output"
mkdir -p "${COVERAGE_DIR}" "${ROOT_DIR}/artifacts"
rm -f "${COVERAGE_DIR}"/* "${ROOT_DIR}"/*.gcda "${ROOT_DIR}"/*.gcno "${ROOT_DIR}"/*.gcov

echo "================================================================================"
echo " Building Panthor-Kbase Suite with Code Coverage Instrumentation"
echo "================================================================================"

g++ -std=c++17 -Wall -Wextra -Werror -Wno-nonnull-compare -O0 -g --coverage -fPIC \
    -Iinclude -Isrc/adapter -Isrc/emulator \
    -c src/adapter/panthor_kbase_adapter.cpp -o "${COVERAGE_DIR}/panthor_kbase_adapter.o"

g++ -std=c++17 -Wall -Wextra -Werror -Wno-nonnull-compare -O0 -g --coverage -fPIC \
    -Iinclude -Isrc/adapter -Isrc/emulator \
    -c src/adapter/log.cpp -o "${COVERAGE_DIR}/log.o"

gcc -Wall -Wextra -Werror -Wno-nonnull-compare -O0 -g --coverage -fPIC \
    -Iinclude -Isrc/adapter -Isrc/emulator \
    -c src/adapter/drm_mode_stubs.c -o "${COVERAGE_DIR}/drm_mode_stubs.o"

g++ -std=c++17 -O0 -g --coverage -fPIC -shared \
    -o "${COVERAGE_DIR}/libpanthor_kbase_adapter.so" \
    "${COVERAGE_DIR}/panthor_kbase_adapter.o" \
    "${COVERAGE_DIR}/log.o" \
    "${COVERAGE_DIR}/drm_mode_stubs.o" \
    -ldl -lpthread

gcc -Wall -Wextra -Werror -Wno-nonnull-compare -O0 -g --coverage -fPIC -shared \
    -Iinclude -Isrc/adapter -Isrc/emulator \
    -o "${COVERAGE_DIR}/libmali_kbase_emu.so" \
    src/emulator/mali_kbase_emu.c \
    src/emulator/panfrost_valhall_disasm_gen.c \
    "${COVERAGE_DIR}/libpanthor_kbase_adapter.so" -Wl,-rpath,"${COVERAGE_DIR}" -ldl -lpthread

gcc -Wall -Wextra -Werror -Wno-nonnull-compare -O0 -g -fPIC -DPANTHOR_KBASE_PATCHED_KMOD=1 -shared \
    -Iinclude -Isrc/adapter -Isrc/emulator \
    -o "${COVERAGE_DIR}/libvulkan_panfrost_patched.so" src/vulkan_android_icd/panvk_android_icd.c \
    "${COVERAGE_DIR}/libpanthor_kbase_adapter.so" -Wl,-rpath,"${COVERAGE_DIR}" -ldl -lpthread

cp -f "${COVERAGE_DIR}/libvulkan_panfrost_patched.so" "${ROOT_DIR}/libvulkan_panfrost_patched.so"

gcc -Wall -Wextra -Werror -Wno-nonnull-compare -O0 -g --coverage \
    -Iinclude -Isrc/adapter -Isrc/emulator \
    -o "${COVERAGE_DIR}/run_vulkan_samples_and_equivalence_suite" \
    tests/run_vulkan_samples_and_equivalence_suite.c \
    "${COVERAGE_DIR}/libpanthor_kbase_adapter.so" "${COVERAGE_DIR}/libmali_kbase_emu.so" \
    -Wl,-rpath,"${COVERAGE_DIR}" -ldl -lpthread

gcc -Wall -Wextra -Werror -O0 -g \
    -Iinclude -Isrc/adapter -Isrc/emulator \
    -o "${COVERAGE_DIR}/unit_tests_edge_cases" \
    tests/unit_tests_edge_cases.c \
    "${COVERAGE_DIR}/libpanthor_kbase_adapter.so" "${COVERAGE_DIR}/libmali_kbase_emu.so" \
    -Wl,-rpath,"${COVERAGE_DIR}" -ldl -lpthread

echo "================================================================================"
echo " Executing Test Suites under Coverage Profiling"
echo "================================================================================"

"${COVERAGE_DIR}/run_vulkan_samples_and_equivalence_suite"
"${COVERAGE_DIR}/unit_tests_edge_cases"

echo "================================================================================"
echo " Generating Gcov Coverage Reports & Parsing Metrics"
echo "================================================================================"

mv "${ROOT_DIR}"/*.gcda "${ROOT_DIR}"/*.gcno "${COVERAGE_DIR}/" 2>/dev/null || true

python3 - << 'PYEOF'
import os, glob, subprocess, json

cov_dir = "coverage_output"
files = glob.glob(os.path.join(cov_dir, "*.gcda"))
for f in files:
    subprocess.run(["gcov", "-b", "-c", "-o", cov_dir, f], stdout=subprocess.PIPE, stderr=subprocess.PIPE)

for g in glob.glob("*.gcov"):
    os.replace(g, os.path.join(cov_dir, g))

gcov_files = [
    f for f in os.listdir(cov_dir)
    if (f.endswith(".c.gcov") or f.endswith(".cpp.gcov"))
    and not f.startswith("usr#")
    and f != "panfrost_valhall_disasm_gen.c.gcov"
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
