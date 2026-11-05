CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -O2 -fPIC -Iinclude -Isrc/adapter -Isrc/emulator

.PHONY: all test coverage conformance verify cmake-host cmake-android clean

all: libpanthor_kbase_adapter.so libpanthor_kbase_runtime_hook.so libmali_kbase_emu.so \
     libvulkan_panfrost_patched.so libvulkan_panfrost_unpatched.so \
     tests/run_vulkan_samples_and_equivalence_suite tests/unit_tests_edge_cases tests/test_runtime_hook_unpatched

libpanthor_kbase_adapter.so: src/adapter/panthor_kbase_adapter.c src/adapter/drm_mode_stubs.c src/adapter/panthor_kbase_adapter.h
	$(CC) $(CFLAGS) -shared -o $@ src/adapter/panthor_kbase_adapter.c src/adapter/drm_mode_stubs.c -ldl -lpthread

libpanthor_kbase_runtime_hook.so: src/adapter/panthor_kbase_runtime_hook.c src/adapter/panthor_kbase_runtime_hook.h libpanthor_kbase_adapter.so
	$(CC) $(CFLAGS) -shared -o $@ src/adapter/panthor_kbase_runtime_hook.c ./libpanthor_kbase_adapter.so -Wl,-rpath,'$$ORIGIN' -ldl -lpthread

libmali_kbase_emu.so: src/emulator/mali_kbase_emu.c src/emulator/mali_kbase_emu.h libpanthor_kbase_adapter.so
	$(CC) $(CFLAGS) -shared -o $@ src/emulator/mali_kbase_emu.c ./libpanthor_kbase_adapter.so -Wl,-rpath,'$$ORIGIN' -ldl -lpthread

libvulkan_panfrost_patched.so: src/vulkan_android_icd/panvk_android_icd.c libpanthor_kbase_adapter.so
	$(CC) $(CFLAGS) -DPANTHOR_KBASE_PATCHED_KMOD=1 -shared -o $@ src/vulkan_android_icd/panvk_android_icd.c ./libpanthor_kbase_adapter.so -Wl,-rpath,'$$ORIGIN' -ldl -lpthread

libvulkan_panfrost_unpatched.so: src/vulkan_android_icd/panvk_android_icd.c libpanthor_kbase_runtime_hook.so libpanthor_kbase_adapter.so
	$(CC) $(CFLAGS) -DPANTHOR_KBASE_PATCHED_KMOD=0 -shared -o $@ src/vulkan_android_icd/panvk_android_icd.c ./libpanthor_kbase_runtime_hook.so ./libpanthor_kbase_adapter.so -Wl,-rpath,'$$ORIGIN' -ldl -lpthread

tests/run_vulkan_samples_and_equivalence_suite: tests/run_vulkan_samples_and_equivalence_suite.c libpanthor_kbase_adapter.so libmali_kbase_emu.so
	$(CC) $(CFLAGS) -o $@ tests/run_vulkan_samples_and_equivalence_suite.c ./libpanthor_kbase_adapter.so ./libmali_kbase_emu.so -Wl,-rpath,'$$ORIGIN/..' -ldl -lpthread

tests/unit_tests_edge_cases: tests/unit_tests_edge_cases.c libpanthor_kbase_adapter.so libmali_kbase_emu.so
	$(CC) $(CFLAGS) -o $@ tests/unit_tests_edge_cases.c ./libpanthor_kbase_adapter.so ./libmali_kbase_emu.so -Wl,-rpath,'$$ORIGIN/..' -ldl -lpthread

tests/test_runtime_hook_unpatched: tests/test_runtime_hook_unpatched.c libpanthor_kbase_runtime_hook.so libpanthor_kbase_adapter.so libmali_kbase_emu.so
	$(CC) $(CFLAGS) -o $@ tests/test_runtime_hook_unpatched.c ./libpanthor_kbase_runtime_hook.so ./libpanthor_kbase_adapter.so ./libmali_kbase_emu.so -Wl,-rpath,'$$ORIGIN/..' -ldl -lpthread

test: all
	mkdir -p artifacts
	./tests/unit_tests_edge_cases
	./tests/run_vulkan_samples_and_equivalence_suite
	./tests/test_runtime_hook_unpatched ./libvulkan_panfrost_patched.so ./libvulkan_panfrost_unpatched.so

coverage:
	./scripts/run_coverage.sh

conformance:
	./scripts/run_conformance.sh

verify: test
	python3 src/verifier/ortools_uapi_verifier.py
	python3 src/verifier/kbase_kernel_symbolic_verifier.py

clean:
	rm -f libpanthor_kbase_adapter.so libpanthor_kbase_runtime_hook.so libmali_kbase_emu.so \
	      libvulkan_panfrost_patched.so libvulkan_panfrost_unpatched.so \
	      tests/run_vulkan_samples_and_equivalence_suite tests/unit_tests_edge_cases tests/test_runtime_hook_unpatched
	rm -rf coverage_output/ *.gcda *.gcno *.gcov
