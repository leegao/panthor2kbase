# ============================================================================
# Makefile (CMake-backed Master Build Driver)
# ============================================================================
BUILD_DIR ?= build-x86-64
CMAKE ?= $(shell ./scripts/find_cmake.sh 2>/dev/null || command -v cmake || echo cmake)

.PHONY: all clean test coverage conformance verify aot-targets

all:
	@mkdir -p $(BUILD_DIR)
	@$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	@$(CMAKE) --build $(BUILD_DIR) -j$(shell nproc 2>/dev/null || echo 4)

aot-targets:
	@mkdir -p $(BUILD_DIR)
	@$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	@$(CMAKE) --build $(BUILD_DIR) --target aot_targets -j$(shell nproc 2>/dev/null || echo 4)

test: all
	@mkdir -p artifacts
	@LD_LIBRARY_PATH="$(CURDIR)/$(BUILD_DIR):$(CURDIR)/$(BUILD_DIR)/adapter:$(CURDIR)/$(BUILD_DIR)/emulator:$(CURDIR)/$(BUILD_DIR)/aot:$${LD_LIBRARY_PATH:-}" \
		$(BUILD_DIR)/tests/unit_tests_edge_cases
	@LD_LIBRARY_PATH="$(CURDIR)/$(BUILD_DIR):$(CURDIR)/$(BUILD_DIR)/adapter:$(CURDIR)/$(BUILD_DIR)/emulator:$(CURDIR)/$(BUILD_DIR)/aot:$${LD_LIBRARY_PATH:-}" \
		$(BUILD_DIR)/tests/run_vulkan_samples_and_equivalence_suite

coverage:
	@./scripts/run_coverage.sh

conformance:
	@./scripts/run_conformance.sh

verify: test
	@python3 src/verifier/ortools_uapi_verifier.py
	@python3 src/verifier/kbase_kernel_symbolic_verifier.py
	@if [ -f kernel/mali_kbase_gs201_r47p0_csf_5_10_189.ko ]; then python3 src/verifier/aarch64_mc_lifter.py kernel/mali_kbase_gs201_r47p0_csf_5_10_189.ko; fi

clean:
	@if [ -d "$(BUILD_DIR)" ]; then $(CMAKE) --build $(BUILD_DIR) --target clean 2>/dev/null || true; fi
	@if [ -d "build-android" ]; then $(CMAKE) --build build-android --target clean 2>/dev/null || true; fi
	@rm -f libpanthor_kbase_adapter.so libmali_kbase_emu.so panthor_drm_replayer panthor_vk_replayer \
	      libvulkan_panfrost_patched.so libmali_uapi_tracer.so \
	      tests/run_vulkan_samples_and_equivalence_suite \
	      tests/unit_tests_edge_cases \
	      tests/test_wayland_and_vkgears_e2e \
	      tests/panprobe/panprobe_runner_suite \
	      tests/test_ludashi_panvk_aot_and_host \
	      build/test_dxvk_aot_layers_e2e \
	      *.o
	@rm -rf coverage_output/ build-coverage/

# Generic target fallback to CMake targets
%:
	@mkdir -p $(BUILD_DIR)
	@$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	@$(CMAKE) --build $(BUILD_DIR) --target $@ -j$(shell nproc 2>/dev/null || echo 4)
