# r54p3 Vulkan 1.3 ICD (Decompiled x86-64 C++ Project)

This repository contains the decompiled, clean C++17 implementation of the ARM Mali `r54p3-00eac0-6eda07` Vulkan 1.3 Installable Client Driver (ICD).

## Key Highlights

- **456-Entry Dispatch Table**: Full implementation of the dispatch table at RVA `0x02f9bfb0` in `include/r54p3_dispatch_table.hpp`.
- **Compiler Boundary Decoupling**: Isolate compiler operations right at `cmpbe_v2_compile_multiple_shaders` (RVA `0x01c9ecb0`) in `include/r54p3_cmpbe_boundary.hpp`. An external shader compiler can be plugged in via `mali_r54p3::cmpbe::registerExternalCmpbeCompiler(...)`.
- **Standalone Valhall ISA Fallback**: Default backend generates valid Valhall 64-bit OBJC machine code for vertex and fragment pipelines, capable of driving `mali0_emu` directly.
- **Vulkan 1.3 Compliance**: Verified passing all 10 Khronos `Vulkan-Samples` categories (dynamic UBOs, instancing, compute SSBOs, depth peeling, queries, BDA).

See [ICD_ARCHITECTURE.md](ICD_ARCHITECTURE.md) for complete technical architecture documentation.

## Directory Structure

```
r54p3_vulkan_icd/
├── CMakeLists.txt              # Standard CMake build definition
├── Makefile                    # Standard GNU Makefile
├── build.sh                    # Automated build script
├── README.md                   # Project overview & documentation
├── BUILD.md                    # Detailed build instructions
├── ICD_ARCHITECTURE.md         # Comprehensive reverse engineering architecture doc
├── include/
│   ├── r54p3_dispatch_table.hpp # 456-entry dispatch table declarations
│   ├── r54p3_icd_types.hpp     # Internal driver structures & handles
│   ├── r54p3_cmpbe_boundary.hpp # Shader compiler boundary & registration hook
│   └── mali0_emu*.hpp          # Mali hardware & kernel emulation headers
├── src/
│   └── r54p3_vulkan_icd.cpp    # Full decompiled ICD entrypoints and dispatch
└── lib/
    ├── libr54p3_vulkan_icd.so  # Shared Vulkan ICD driver library
    ├── libr54p3_vulkan_icd.a   # Static library
    └── r54p3_decompiled_icd.x86_64.json # Vulkan loader ICD manifest
```

## Quick Start

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```
Or with GNU Make:
```bash
make -j$(nproc)
```
