# Building r54p3 Vulkan ICD

## Prerequisites

- Linux x86_64
- GCC >= 9.0 or Clang >= 10.0 (C++17 support)
- CMake >= 3.16 or GNU Make
- `libdl` and `pthread` (standard on all Linux distributions)
- Optional: Vulkan loader (`libvulkan1` / `vulkan-tools`) for running system test clients

## Build with CMake (Recommended)

```bash
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

To install into custom prefix:
```bash
cmake .. -DCMAKE_INSTALL_PREFIX=/opt/mali_icd
cmake --build . --target install
```

## Build with Make

```bash
make -j$(nproc)
```

## Build with Shell Script

```bash
bash build.sh
```

## Testing with Vulkan Loader

Export the generated ICD manifest to test with standard Vulkan tools:
```bash
export VK_ICD_FILENAMES="$(pwd)/lib/r54p3_decompiled_icd.x86_64.json"
vulkaninfo --summary
```
