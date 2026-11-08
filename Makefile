CC ?= gcc
CXX ?= g++
CFLAGS ?= -Wall -Wextra -Werror -O2 -fPIC -Iinclude -Isrc/adapter -Isrc/emulator
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O2 -fPIC -Iinclude -Isrc/adapter -Isrc/emulator
VK_INC := $(shell if [ -f mesa/include/vulkan/vulkan_core.h ]; then echo "-Imesa/include"; elif [ -f /working_dir/c_3106ac7496559798/work/mesa_upstream/mesa-5a07217f034b3e50d8c7c7794f97a2df1742613b/include/vulkan/vulkan_core.h ]; then echo "-I/working_dir/c_3106ac7496559798/work/mesa_upstream/mesa-5a07217f034b3e50d8c7c7794f97a2df1742613b/include"; fi)

.PHONY: all aot-targets test coverage conformance verify cmake-host cmake-android clean

all: libpanthor_kbase_adapter.so libmali_kbase_emu.so \
     libvulkan_panfrost_patched.so \
     tests/run_vulkan_samples_and_equivalence_suite \
     tests/unit_tests_edge_cases \
     tests/test_wayland_and_vkgears_e2e \
     tests/panprobe/panprobe_runner_suite \
     tests/test_ludashi_panvk_aot_and_host \
     build/test_dxvk_aot_layers_e2e \
     aot-targets

libpanthor_kbase_adapter.so: src/adapter/panthor_kbase_adapter.cpp src/adapter/log.cpp src/adapter/log.h src/adapter/drm_mode_stubs.c src/adapter/panthor_kbase_adapter.h
	$(CXX) $(CXXFLAGS) -c src/adapter/panthor_kbase_adapter.cpp -o panthor_kbase_adapter.o
	$(CXX) $(CXXFLAGS) -c src/adapter/log.cpp -o log.o
	$(CC) $(CFLAGS) -c src/adapter/drm_mode_stubs.c -o drm_mode_stubs.o
	$(CXX) $(CXXFLAGS) -shared -Wl,-soname,libpanthor_kbase_adapter.so -Wl,-z,nodelete -o $@ panthor_kbase_adapter.o log.o drm_mode_stubs.o -ldl -lpthread
	rm -f panthor_kbase_adapter.o log.o drm_mode_stubs.o

libmali_kbase_emu.so: src/emulator/mali_kbase_emu.c src/emulator/panfrost_valhall_disasm_gen.c src/emulator/mali_kbase_emu.h include/valhall_disasm_lift.h libpanthor_kbase_adapter.so
	$(CC) $(CFLAGS) -shared -Wl,-soname,libmali_kbase_emu.so -Wl,-z,nodelete -o $@ src/emulator/mali_kbase_emu.c src/emulator/panfrost_valhall_disasm_gen.c -L. -lpanthor_kbase_adapter -Wl,-rpath,'$$ORIGIN' -ldl -lpthread

libvulkan_panfrost_patched.so: src/vulkan_android_icd/panvk_android_icd.c libpanthor_kbase_adapter.so
	$(CC) $(CFLAGS) -DPANTHOR_KBASE_PATCHED_KMOD=1 -shared -Wl,-soname,libvulkan_panfrost_patched.so -Wl,-z,nodelete -o $@ src/vulkan_android_icd/panvk_android_icd.c -L. -lpanthor_kbase_adapter -Wl,-rpath,'$$ORIGIN' -ldl -lpthread

tests/run_vulkan_samples_and_equivalence_suite: tests/run_vulkan_samples_and_equivalence_suite.c libpanthor_kbase_adapter.so libmali_kbase_emu.so libvulkan_panfrost_patched.so
	$(CC) $(CFLAGS) -o $@ tests/run_vulkan_samples_and_equivalence_suite.c ./libpanthor_kbase_adapter.so ./libmali_kbase_emu.so -Wl,-rpath,'$$ORIGIN/..' -ldl -lpthread

tests/unit_tests_edge_cases: tests/unit_tests_edge_cases.c libpanthor_kbase_adapter.so libmali_kbase_emu.so
	$(CC) $(CFLAGS) -o $@ tests/unit_tests_edge_cases.c ./libpanthor_kbase_adapter.so ./libmali_kbase_emu.so -Wl,-rpath,'$$ORIGIN/..' -ldl -lpthread

tests/test_wayland_and_vkgears_e2e: tests/test_wayland_and_vkgears_e2e.cpp libpanthor_kbase_adapter.so libmali_kbase_emu.so
	$(CXX) -std=c++17 -O2 $(VK_INC) -Iinclude -Isrc/aot/include tests/test_wayland_and_vkgears_e2e.cpp -o $@ /usr/lib/x86_64-linux-gnu/libvulkan.so.1 -Wl,-rpath,'$$ORIGIN/..' -ldl -lpthread

tests/panprobe/panprobe_runner_suite: tests/panprobe/panprobe_runner_suite.cpp libpanthor_kbase_adapter.so libmali_kbase_emu.so
	$(CXX) -std=c++17 -O2 -Iinclude -Isrc/emulator -Isrc/adapter tests/panprobe/panprobe_runner_suite.cpp ./libpanthor_kbase_adapter.so ./libmali_kbase_emu.so -Wl,-rpath,'$$ORIGIN/../..' -ldl -lpthread -o $@

tests/test_ludashi_panvk_aot_and_host: tests/test_ludashi_panvk_aot_and_host.cpp libpanthor_kbase_adapter.so libmali_kbase_emu.so
	$(CXX) -std=c++17 -O2 -Iinclude -Isrc/emulator -Isrc/adapter tests/test_ludashi_panvk_aot_and_host.cpp ./libpanthor_kbase_adapter.so ./libmali_kbase_emu.so -Wl,-rpath,'$$ORIGIN/..' -ldl -lpthread -o $@

build/test_dxvk_aot_layers_e2e: tests/test_dxvk_aot_layers_e2e.cpp libpanthor_kbase_adapter.so libmali_kbase_emu.so
	./src/aot/layers/build_layers.sh
	mkdir -p build
	$(CXX) -std=c++17 -O2 -Iinclude -Iinclude/uapi -Isrc/aot/include tests/test_dxvk_aot_layers_e2e.cpp -L. -lpanthor_kbase_adapter -lmali_kbase_emu -ldl -lpthread -Wl,-rpath,'$$ORIGIN/..' -o $@

aot-targets: libmali_kbase_emu.so
	mkdir -p src/aot/bin src/aot/r54p3_vulkan_icd/lib
	$(CXX) -std=c++17 -O2 src/aot/tests/vkcube_wsi_e2e.cpp -Wl,-rpath,'$$ORIGIN/../../..:$$ORIGIN' -ldl -lpthread -o src/aot/bin/vkcube
	$(CXX) -std=c++20 -O2 -Isrc/aot/include src/aot/tests/test_vulkan_samples_compliance.cpp -Wl,-rpath,'$$ORIGIN/../../..:$$ORIGIN' -ldl -lpthread -o src/aot/bin/test_vulkan_samples_compliance
	$(CXX) -std=c++17 -O2 src/aot/tests/test_vulkan_icd_client.cpp -Wl,-rpath,'$$ORIGIN/../../..:$$ORIGIN' -ldl -lpthread -o src/aot/bin/test_vulkan_icd_client
	$(CXX) -O3 -std=c++17 -shared -fPIC -Wl,-Bsymbolic -Wl,-Bsymbolic-functions -Isrc/aot/r54p3_vulkan_icd/include src/aot/r54p3_vulkan_icd/src/r54p3_vulkan_icd.cpp -L. -lmali_kbase_emu -Wl,-rpath,'$$ORIGIN/../../../..:$$ORIGIN' -ldl -lpthread -o src/aot/r54p3_vulkan_icd/lib/libr54p3_vulkan_icd.so
	./src/aot/translate.sh

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
	      tests/run_vulkan_samples_and_equivalence_suite \
	      tests/unit_tests_edge_cases \
	      tests/test_wayland_and_vkgears_e2e \
	      tests/panprobe/panprobe_runner_suite \
	      tests/test_ludashi_panvk_aot_and_host \
	      build/test_dxvk_aot_layers_e2e \
	 	      *.o
	rm -rf coverage_output/ *.gcda *.gcno *.gcov
