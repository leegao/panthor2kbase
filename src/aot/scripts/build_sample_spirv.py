#!/usr/bin/env python3
"""
build_sample_spirv.py
=====================
Generates 3 valid Vulkan SPIR-V 1.3 binary shader modules of varying complexity
for end-to-end AOT translation, vkCreateShaderModule (SHA-1 + SPIR-V validation),
and Mali hardware ISA OBJC (Bifrost / Valhall / 5th-Gen) codegen & MBS2
deserialization testing across all 4 64-bit libGLES_mali.so drivers:

  1. shaders/passthrough_frag.spv     (Complexity: Low)
     - Stage: Fragment (ExecutionModel=4)
     - Operations: Constant vec4 color store to Fragment output location 0.
  2. shaders/vector_compute.spv       (Complexity: Medium)
     - Stage: GLCompute (ExecutionModel=5, LocalSize 64x1x1)
     - Operations: StorageBuffer vec4 load, OpFMul, OpFAdd, OpFSub FMA pipeline,
       StorageBuffer vec4 store.
  3. shaders/branch_loop_compute.spv  (Complexity: High)
     - Stage: GLCompute (ExecutionModel=5, LocalSize 64x1x1)
     - Operations: Structured control flow with OpLoopMerge, OpSelectionMerge,
       OpBranchConditional, OpSLessThan, OpIAdd, OpFAdd iterative reduction.
"""

import hashlib
import os
import struct


def spv_op(op_code: int, *operands: int) -> list:
    word_count = 1 + len(operands)
    return [(word_count << 16) | (op_code & 0xFFFF), *operands]


def spv_str(s: str) -> list:
    b = s.encode("utf-8") + b"\x00"
    while len(b) % 4 != 0:
        b += b"\x00"
    return list(struct.unpack(f"<{len(b)//4}I", b))


def pack_spirv(bound: int, instructions: list) -> bytes:
    header = [
        0x07230203,  # SPIR-V Magic Number
        0x00010300,  # SPIR-V 1.3 (Vulkan 1.1+)
        0x00080001,  # Generator magic
        bound,       # ID bound
        0x00000000,  # Instruction schema
    ]
    words = header + instructions
    return struct.pack(f"<{len(words)}I", *words)


def build_passthrough_frag() -> bytes:
    """Shader 1: Minimal Vulkan Passthrough Fragment Shader (1BB, Constant Vec4 Store)."""
    insns = []
    insns += spv_op(17, 1)                        # OpCapability Shader (1)
    insns += spv_op(14, 0, 1)                     # OpMemoryModel Logical(0) GLSL450(1)
    insns += spv_op(15, 4, 1, *spv_str("main"), 9)  # OpEntryPoint Fragment(4) %1 "main" %9
    insns += spv_op(16, 1, 7)                     # OpExecutionMode %1 OriginUpperLeft(7)
    insns += spv_op(71, 9, 30, 0)                 # OpDecorate %9 Location 0
    insns += spv_op(19, 2)                        # OpTypeVoid %2
    insns += spv_op(33, 3, 2)                     # OpTypeFunction %3 %2
    insns += spv_op(22, 6, 32)                    # OpTypeFloat %6 32
    insns += spv_op(23, 7, 6, 4)                  # OpTypeVector %7 %6 4
    insns += spv_op(32, 8, 3, 7)                  # OpTypePointer %8 Output(3) %7
    insns += spv_op(59, 8, 9, 3)                  # OpVariable %8 %9 Output(3)
    insns += spv_op(43, 6, 10, 0x3F800000)        # OpConstant %6 %10 1.0f
    insns += spv_op(43, 6, 11, 0x3F000000)        # OpConstant %6 %11 0.5f
    insns += spv_op(43, 6, 12, 0x3E800000)        # OpConstant %6 %12 0.25f
    insns += spv_op(44, 7, 13, 10, 11, 12, 10)    # OpConstantComposite %7 %13 %10 %11 %12 %10
    insns += spv_op(54, 2, 1, 0, 3)               # OpFunction %2 %1 None %3
    insns += spv_op(248, 5)                       # OpLabel %5
    insns += spv_op(62, 9, 13)                    # OpStore %9 %13
    insns += spv_op(253)                          # OpReturn
    insns += spv_op(56)                           # OpFunctionEnd
    return pack_spirv(16, insns)


def build_vector_compute() -> bytes:
    """Shader 2: Storage Buffer Vector Arithmetic FMA Compute Shader (LocalSize 64x1x1)."""
    insns = []
    insns += spv_op(17, 1)                        # OpCapability Shader (1)
    insns += spv_op(14, 0, 1)                     # OpMemoryModel Logical(0) GLSL450(1)
    insns += spv_op(15, 5, 1, *spv_str("main"), 11, 12)  # OpEntryPoint GLCompute(5) %1 "main" %11 %12
    insns += spv_op(16, 1, 17, 64, 1, 1)          # OpExecutionMode %1 LocalSize(17) 64 1 1
    insns += spv_op(71, 11, 33, 0)                # OpDecorate %11 Binding 0
    insns += spv_op(71, 11, 34, 0)                # OpDecorate %11 DescriptorSet 0
    insns += spv_op(71, 12, 33, 1)                # OpDecorate %12 Binding 1
    insns += spv_op(71, 12, 34, 0)                # OpDecorate %12 DescriptorSet 0
    insns += spv_op(19, 2)                        # OpTypeVoid %2
    insns += spv_op(33, 3, 2)                     # OpTypeFunction %3 %2
    insns += spv_op(22, 6, 32)                    # OpTypeFloat %6 32
    insns += spv_op(23, 7, 6, 4)                  # OpTypeVector %7 %6 4
    insns += spv_op(32, 8, 12, 7)                 # OpTypePointer %8 StorageBuffer(12) %7
    insns += spv_op(59, 8, 11, 12)                # OpVariable %8 %11 StorageBuffer(12)
    insns += spv_op(59, 8, 12, 12)                # OpVariable %8 %12 StorageBuffer(12)
    insns += spv_op(43, 6, 13, 0x40000000)        # OpConstant %6 %13 2.0f
    insns += spv_op(43, 6, 14, 0x3F000000)        # OpConstant %6 %14 0.5f
    insns += spv_op(43, 6, 15, 0x3E000000)        # OpConstant %6 %15 0.125f
    insns += spv_op(44, 7, 16, 13, 13, 13, 13)    # OpConstantComposite %7 %16 (scale)
    insns += spv_op(44, 7, 17, 14, 14, 14, 14)    # OpConstantComposite %7 %17 (bias)
    insns += spv_op(44, 7, 18, 15, 15, 15, 15)    # OpConstantComposite %7 %18 (offset)
    insns += spv_op(54, 2, 1, 0, 3)               # OpFunction %2 %1 None %3
    insns += spv_op(248, 5)                       # OpLabel %5
    insns += spv_op(61, 7, 19, 11)                # OpLoad %7 %19 %11
    insns += spv_op(133, 7, 20, 19, 16)           # OpFMul %7 %20 %19 %16
    insns += spv_op(129, 7, 21, 20, 17)           # OpFAdd %7 %21 %20 %17
    insns += spv_op(131, 7, 22, 21, 18)           # OpFSub %7 %22 %21 %18
    insns += spv_op(133, 7, 23, 22, 20)           # OpFMul %7 %23 %22 %20
    insns += spv_op(62, 12, 23)                   # OpStore %12 %23
    insns += spv_op(253)                          # OpReturn
    insns += spv_op(56)                           # OpFunctionEnd
    return pack_spirv(26, insns)


def build_branch_loop_compute() -> bytes:
    """Shader 3: Structured Branching & Loop Reduction Compute Shader (6 Basic Blocks)."""
    insns = []
    insns += spv_op(17, 1)                        # OpCapability Shader (1)
    insns += spv_op(14, 0, 1)                     # OpMemoryModel Logical(0) GLSL450(1)
    insns += spv_op(15, 5, 1, *spv_str("main"), 11, 12)  # OpEntryPoint GLCompute(5) %1 "main" %11 %12
    insns += spv_op(16, 1, 17, 64, 1, 1)          # OpExecutionMode %1 LocalSize(17) 64 1 1
    insns += spv_op(71, 11, 33, 0)                # OpDecorate %11 Binding 0
    insns += spv_op(71, 12, 33, 1)                # OpDecorate %12 Binding 1
    insns += spv_op(19, 2)                        # OpTypeVoid %2
    insns += spv_op(33, 3, 2)                     # OpTypeFunction %3 %2
    insns += spv_op(20, 4, 32, 1)                 # OpTypeInt %4 32 1
    insns += spv_op(21, 5, 32, 0)                 # OpTypeBool %5 (note: op 20=OpTypeBool)
    insns[-1] = (2 << 16) | 20                    # Fix: OpTypeBool %5
    insns[-2] = 21                                # OpTypeInt = 21
    insns = []
    insns += spv_op(17, 1)                        # OpCapability Shader
    insns += spv_op(14, 0, 1)                     # OpMemoryModel Logical GLSL450
    insns += spv_op(15, 5, 1, *spv_str("main"), 11, 12)
    insns += spv_op(16, 1, 17, 64, 1, 1)          # OpExecutionMode %1 LocalSize 64 1 1
    insns += spv_op(71, 11, 33, 0)                # OpDecorate %11 Binding 0
    insns += spv_op(71, 12, 33, 1)                # OpDecorate %12 Binding 1
    insns += spv_op(19, 2)                        # OpTypeVoid %2
    insns += spv_op(33, 3, 2)                     # OpTypeFunction %3 %2
    insns += spv_op(20, 4)                        # OpTypeBool %4
    insns += spv_op(21, 5, 32, 1)                 # OpTypeInt %5 32 1
    insns += spv_op(22, 6, 32)                    # OpTypeFloat %6 32
    insns += spv_op(23, 7, 6, 4)                  # OpTypeVector %7 %6 4
    insns += spv_op(32, 8, 12, 7)                 # OpTypePointer %8 StorageBuffer(12) %7
    insns += spv_op(32, 9, 7, 5)                  # OpTypePointer %9 Function(7) %5
    insns += spv_op(32, 10, 7, 7)                 # OpTypePointer %10 Function(7) %7
    insns += spv_op(59, 8, 11, 12)                # OpVariable %8 %11 StorageBuffer(12)
    insns += spv_op(59, 8, 12, 12)                # OpVariable %8 %12 StorageBuffer(12)
    insns += spv_op(43, 5, 13, 0)                 # OpConstant %5 %13 0
    insns += spv_op(43, 5, 14, 1)                 # OpConstant %5 %14 1
    insns += spv_op(43, 5, 15, 8)                 # OpConstant %5 %15 8 (loop trip count)
    insns += spv_op(43, 6, 16, 0x3FA00000)        # OpConstant %6 %16 1.25f
    insns += spv_op(44, 7, 17, 16, 16, 16, 16)    # OpConstantComposite %7 %17
    insns += spv_op(54, 2, 1, 0, 3)               # OpFunction %2 %1 None %3
    insns += spv_op(248, 20)                      # OpLabel %20 (entry)
    insns += spv_op(59, 9, 21, 7)                 # OpVariable %9 %21 Function (i)
    insns += spv_op(59, 10, 22, 7)                # OpVariable %10 %22 Function (acc)
    insns += spv_op(61, 7, 23, 11)                # OpLoad %7 %23 %11
    insns += spv_op(62, 21, 13)                   # OpStore %21 %13 (i = 0)
    insns += spv_op(62, 22, 23)                   # OpStore %22 %23 (acc = in)
    insns += spv_op(249, 24)                      # OpBranch %24 (loop header)
    insns += spv_op(248, 24)                      # OpLabel %24 (loop header)
    insns += spv_op(246, 29, 28, 0)               # OpLoopMerge %29 (merge) %28 (continue) None
    insns += spv_op(249, 25)                      # OpBranch %25 (loop cond)
    insns += spv_op(248, 25)                      # OpLabel %25
    insns += spv_op(61, 5, 30, 21)                # OpLoad %5 %30 %21
    insns += spv_op(177, 4, 31, 30, 15)           # OpSLessThan %4 %31 %30 %15 (i < 8)
    insns += spv_op(250, 31, 26, 29)              # OpBranchConditional %31 %26 (body) %29 (merge)
    insns += spv_op(248, 26)                      # OpLabel %26 (body with selection branch)
    insns += spv_op(247, 28, 0)                   # OpSelectionMerge %28 None
    insns += spv_op(177, 4, 32, 30, 14)           # OpSLessThan %4 %32 %30 %14
    insns += spv_op(250, 32, 27, 28)              # OpBranchConditional %32 %27 %28
    insns += spv_op(248, 27)                      # OpLabel %27 (then block)
    insns += spv_op(61, 7, 33, 22)                # OpLoad %7 %33 %22
    insns += spv_op(133, 7, 34, 33, 17)           # OpFMul %7 %34 %33 %17
    insns += spv_op(129, 7, 35, 34, 17)           # OpFAdd %7 %35 %34 %17
    insns += spv_op(62, 22, 35)                   # OpStore %22 %35
    insns += spv_op(249, 28)                      # OpBranch %28
    insns += spv_op(248, 28)                      # OpLabel %28 (continue target)
    insns += spv_op(61, 7, 36, 22)                # OpLoad %7 %36 %22
    insns += spv_op(129, 7, 37, 36, 17)           # OpFAdd %7 %37 %36 %17
    insns += spv_op(62, 22, 37)                   # OpStore %22 %37
    insns += spv_op(128, 5, 38, 30, 14)           # OpIAdd %5 %38 %30 %14 (i + 1)
    insns += spv_op(62, 21, 38)                   # OpStore %21 %38
    insns += spv_op(249, 24)                      # OpBranch %24
    insns += spv_op(248, 29)                      # OpLabel %29 (loop merge / exit)
    insns += spv_op(61, 7, 39, 22)                # OpLoad %7 %39 %22
    insns += spv_op(62, 12, 39)                   # OpStore %12 %39
    insns += spv_op(253)                          # OpReturn
    insns += spv_op(56)                           # OpFunctionEnd
    return pack_spirv(42, insns)


def main():
    out_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "shaders")
    os.makedirs(out_dir, exist_ok=True)
    shaders = [
        ("passthrough_frag.spv", "Minimal Passthrough Fragment Shader", build_passthrough_frag()),
        ("vector_compute.spv", "Storage Buffer Vector FMA Compute Shader", build_vector_compute()),
        ("branch_loop_compute.spv", "Control-Flow Branching & Loop Reduction Compute Shader", build_branch_loop_compute()),
    ]
    for fname, desc, data in shaders:
        path = os.path.join(out_dir, fname)
        with open(path, "wb") as f:
            f.write(data)
        sha1 = hashlib.sha1(data).hexdigest()
        print(f"[SPIR-V] Generated {fname:26s} ({len(data):3d} bytes, {len(data)//4:2d} words) SHA-1={sha1} -- {desc}")


if __name__ == "__main__":
    main()
