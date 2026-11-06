CC ?= gcc
CXX ?= g++
CFLAGS ?= -Wall -Wextra -Werror -O2 -fPIC -Iinclude -Isrc/adapter -Isrc/emulator
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O2 -fPIC -Iinclude -Isrc/adapter -Isrc/emulator

.PHONY: all test coverage conformance verify cmake-host cmake-android clean

all: libpanthor_kbase_adapter.so libmali_kbase_emu.so \
     libvulkan_panfrost_patched.so \
     tests/run_vulkan_samples_and_equivalence_suite tests/unit_tests_edge_cases

libpanthor_kbase_adapter.so: src/adapter/panthor_kbase_adapter.cpp src/adapter/log.cpp src/adapter/log.h src/adapter/drm_mode_stubs.c src/adapter/panthor_kbase_adapter.h
	$(CXX) $(CXXFLAGS) -c src/adapter/panthor_kbase_adapter.cpp -o panthor_kbase_adapter.o
	$(CXX) $(CXXFLAGS) -c src/adapter/log.cpp -o log.o
	$(CC) $(CFLAGS) -c src/adapter/drm_mode_stubs.c -o drm_mode_stubs.o
	$(CXX) $(CXXFLAGS) -shared -Wl,-z,nodelete -o $@ panthor_kbase_adapter.o log.o drm_mode_stubs.o -ldl -lpthread
	rm -f panthor_kbase_adapter.o log.o drm_mode_stubs.o

libmali_kbase_emu.so: src/emulator/mali_kbase_emu.c src/emulator/panfrost_valhall_disasm_gen.c src/emulator/mali_kbase_emu.h include/valhall_disasm_lift.h libpanthor_kbase_adapter.so
	$(CC) $(CFLAGS) -shared -Wl,-z,nodelete -o $@ src/emulator/mali_kbase_emu.c src/emulator/panfrost_valhall_disasm_gen.c ./libpanthor_kbase_adapter.so -Wl,-rpath,'$$ORIGIN' -ldl -lpthread

libvulkan_panfrost_patched.so: src/vulkan_android_icd/panvk_android_icd.c libpanthor_kbase_adapter.so
	$(CC) $(CFLAGS) -DPANTHOR_KBASE_PATCHED_KMOD=1 -shared -Wl,-z,nodelete -o $@ src/vulkan_android_icd/panvk_android_icd.c ./libpanthor_kbase_adapter.so -Wl,-rpath,'$$ORIGIN' -ldl -lpthread

tests/run_vulkan_samples_and_equivalence_suite: tests/run_vulkan_samples_and_equivalence_suite.c libpanthor_kbase_adapter.so libmali_kbase_emu.so libvulkan_panfrost_patched.so
	$(CC) $(CFLAGS) -o $@ tests/run_vulkan_samples_and_equivalence_suite.c ./libpanthor_kbase_adapter.so ./libmali_kbase_emu.so -Wl,-rpath,'$$ORIGIN/..' -ldl -lpthread

tests/unit_tests_edge_cases: tests/unit_tests_edge_cases.c libpanthor_kbase_adapter.so libmali_kbase_emu.so
	$(CC) $(CFLAGS) -o $@ tests/unit_tests_edge_cases.c ./libpanthor_kbase_adapter.so ./libmali_kbase_emu.so -Wl,-rpath,'$$ORIGIN/..' -ldl -lpthread

test: all
	mkdir -p artifacts
	./tests/unit_tests_edge_cases
	./tests/run_vulkan_samples_and_equivalence_suite

coverage:
	./scripts/run_coverage.sh

conformance:
	./scripts/run_conformance.sh

verify: test
	python3 src/verifier/ortools_uapi_verifier.py
	python3 src/verifier/kbase_kernel_symbolic_verifier.py
	python3 src/verifier/aarch64_mc_lifter.py mali_kbase.ko

clean:
	rm -f libpanthor_kbase_adapter.so libmali_kbase_emu.so \
	      libvulkan_panfrost_patched.so \
	      tests/run_vulkan_samples_and_equivalence_suite tests/unit_tests_edge_cases \
	      *.o
	rm -rf coverage_output/ *.gcda *.gcno *.gcov
