# Scoped-down Makefile for src/aot/horizon2llvm (AArch64-to-LLVM-IR Lifter & Core Lowering Passes)

THIS_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT := $(THIS_DIR)
LIFTER_DIR := $(THIS_DIR)
BUILD_DIR := $(THIS_DIR)/build
OBJ_DIR := $(BUILD_DIR)/lifter_obj
RUNTIME_DIR := $(LIFTER_DIR)/runtime

ifneq ($(wildcard $(LIFTER_DIR)/third-party/llvm/llvm/include),)
LLVM_DIR := $(LIFTER_DIR)/third-party/llvm/llvm
else
LLVM_DIR := $(LIFTER_DIR)/third-party/llvm
endif
LLVM_INC := -I$(LLVM_DIR)/include -I$(LLVM_DIR)/include/llvm -I$(LIFTER_DIR)/third-party/llvm/include -I$(LIFTER_DIR)/third-party/llvm/include/llvm
LLVM_LIB_DIR := $(LLVM_DIR)/prebuilts/x86-64
LLVM_LIB := $(LLVM_LIB_DIR)/libLLVM.so

ifneq ($(shell which llvm-config 2>/dev/null),)
LLVM_INC := $(shell llvm-config --cxxflags)
LLVM_LIB := $(shell llvm-config --libs core engine scalaropts passes bitwriter native)
LLVM_LIB_FLAG := $(shell llvm-config --ldflags)
else
LLVM_LIB_FLAG := -Wl,-rpath,$(LLVM_LIB_DIR)
endif

CXX ?= g++
CC ?= gcc
CXXFLAGS := -std=c++17 -O3 -Wall -Wextra -Wno-unused-parameter -DH2L_HAVE_LLVM_HEADERS=1 -I$(LIFTER_DIR) -I$(RUNTIME_DIR) $(LLVM_INC) $(EXTRA_CXXFLAGS) -pthread
CFLAGS := -O2 -fPIC -fno-strict-aliasing -I$(LIFTER_DIR) -I$(RUNTIME_DIR)
LDFLAGS := $(LLVM_LIB_FLAG) -pthread -ldl

CORE_SRCS := \
	llvm_backend.cpp \
	aarch64/aarch64_lifter.cpp \
	passes.cpp \
	lowering/pass_guest_stack_alignment.cpp \
	lowering/pass_aarch64_idiom_deconstruct.cpp \
	lowering/pass_jump_table_lowering.cpp \
	lowering/pass_fcmp_predicate_fusion.cpp \
	lowering/pass_nzcv_predicate_fusion.cpp \
	lowering/pass_nzcv_dead_flag_elim.cpp \
	lowering/pass_link_register_elision.cpp \
	lowering/pass_tail_call_devirt.cpp \
	lowering/pass_aapcs64_abi_contract.cpp \
	lowering/pass_cpu_state_promotion.cpp

CORE_OBJS := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(CORE_SRCS))
MAIN_OBJS := $(OBJ_DIR)/main.o
SIMD_RT_OBJ := $(OBJ_DIR)/simd_rt.o

LIFTER_BIN := $(BUILD_DIR)/horizon2llvm
UNIT_BIN   := $(BUILD_DIR)/lifter_unit_tests

.PHONY: all tests clean horizon2llvm

all: $(LIFTER_BIN) $(UNIT_BIN)

tests: $(UNIT_BIN)
	$(UNIT_BIN)

$(OBJ_DIR)/%.o: $(LIFTER_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -MMD -MP $(CXXFLAGS) -c $< -o $@

$(SIMD_RT_OBJ): $(RUNTIME_DIR)/simd_rt.c
	@mkdir -p $(dir $@)
	$(CC) -MMD -MP $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/test_main_snippets.o: $(LIFTER_DIR)/tests/test_main_snippets.cpp
	@mkdir -p $(dir $@)
	$(CXX) -MMD -MP $(CXXFLAGS) -I$(LIFTER_DIR)/tests -c $< -o $@

$(LIFTER_BIN): $(CORE_OBJS) $(MAIN_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $^ $(LLVM_LIB) $(LDFLAGS) -o $@

$(UNIT_BIN): $(CORE_OBJS) $(SIMD_RT_OBJ) $(OBJ_DIR)/test_main_snippets.o
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $^ $(LLVM_LIB) $(LDFLAGS) -lm -o $@

horizon2llvm: $(LIFTER_BIN)

clean:
	rm -rf $(BUILD_DIR)

-include $(OBJ_DIR)/*.d $(OBJ_DIR)/lowering/*.d
