#!/usr/bin/env python3
"""Auto-generate physical-device Vulkan ICD forwarding wrappers into r54p3_lifted.o."""

from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT_PATH = ROOT / "src" / "r54p3_vulkan_icd" / "generated" / "mali_physdev_wrappers_autogen.inc"

# Each entry: (ret_type, name, [(type, arg_name), ...], aot_args, non_null_checks, pre_call, post_check, fallback)
WRAPPERS = [
    (
        "void",
        "vkGetPhysicalDeviceFeatures",
        [("void*", "physicalDevice"), ("void*", "pFeatures")],
        ["pdev->aot_internal", "pFeatures"],
        ["pdev", "pFeatures"],
        "",
        "",
        "return;",
    ),
    (
        "void",
        "vkGetPhysicalDeviceFeatures2",
        [("void*", "physicalDevice"), ("void*", "pFeatures2")],
        ["pdev->aot_internal", "pFeatures2"],
        ["pdev", "pFeatures2"],
        "",
        "",
        "return;",
    ),
    (
        "void",
        "vkGetPhysicalDeviceQueueFamilyProperties",
        [("void*", "physicalDevice"), ("uint32_t*", "pQueueFamilyPropertyCount"), ("void*", "pQueueFamilyProperties")],
        ["pdev->aot_internal", "pQueueFamilyPropertyCount", "pQueueFamilyProperties"],
        ["pdev", "pQueueFamilyPropertyCount"],
        "if (!pQueueFamilyPropertyCount) return;",
        "",
        """if (!pQueueFamilyProperties) {
        *pQueueFamilyPropertyCount = 1;
        return;
    }
    if (*pQueueFamilyPropertyCount > 0) {
        auto* qf = static_cast<VkQueueFamilyProperties*>(pQueueFamilyProperties);
        qf[0].queueFlags                  = 0x17U;
        qf[0].queueCount                  = 2U;
        qf[0].timestampValidBits          = 64U;
        qf[0].minImageTransferGranularity = {1U, 1U, 1U};
        *pQueueFamilyPropertyCount        = 1;
    }""",
    ),
    (
        "void",
        "vkGetPhysicalDeviceQueueFamilyProperties2",
        [("void*", "physicalDevice"), ("uint32_t*", "pQueueFamilyPropertyCount"), ("void*", "pQueueFamilyProperties2")],
        ["pdev->aot_internal", "pQueueFamilyPropertyCount", "pQueueFamilyProperties2"],
        ["pdev", "pQueueFamilyPropertyCount"],
        "if (!pQueueFamilyPropertyCount) return;",
        "",
        """if (!pQueueFamilyProperties2) {
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, pQueueFamilyPropertyCount, nullptr);
        return;
    }
    if (*pQueueFamilyPropertyCount > 0) {
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, pQueueFamilyPropertyCount, static_cast<uint8_t*>(pQueueFamilyProperties2) + 16);
    }""",
    ),
    (
        "void",
        "vkGetPhysicalDeviceMemoryProperties",
        [("void*", "physicalDevice"), ("void*", "pMemoryProperties")],
        ["pdev->aot_internal", "pMemoryProperties"],
        ["pdev", "pMemoryProperties"],
        "if (!pMemoryProperties) return;",
        "",
        """auto* mp = static_cast<VkPhysicalDeviceMemoryProperties*>(pMemoryProperties);
    std::memset(mp, 0, sizeof(VkPhysicalDeviceMemoryProperties));
    mp->memoryTypeCount = 3;
    mp->memoryTypes[0].propertyFlags = 0x07U;
    mp->memoryTypes[0].heapIndex     = 0;
    mp->memoryTypes[1].propertyFlags = 0x0BU;
    mp->memoryTypes[1].heapIndex     = 0;
    mp->memoryTypes[2].propertyFlags = 0x11U;
    mp->memoryTypes[2].heapIndex     = 0;
    mp->memoryHeapCount = 1;
    mp->memoryHeaps[0].size  = 8ULL << 30;
    mp->memoryHeaps[0].flags = 0x01U;""",
    ),
    (
        "void",
        "vkGetPhysicalDeviceMemoryProperties2",
        [("void*", "physicalDevice"), ("void*", "pMemoryProperties2")],
        ["pdev->aot_internal", "pMemoryProperties2"],
        ["pdev", "pMemoryProperties2"],
        "if (!pMemoryProperties2) return;",
        "",
        "vkGetPhysicalDeviceMemoryProperties(physicalDevice, static_cast<uint8_t*>(pMemoryProperties2) + 16);",
    ),
    (
        "void",
        "vkGetPhysicalDeviceFormatProperties",
        [("void*", "physicalDevice"), ("uint32_t", "format"), ("void*", "pFormatProperties")],
        ["pdev->aot_internal", "format", "pFormatProperties"],
        ["pdev"],
        """if (!pFormatProperties) return;
    uint32_t* fp = static_cast<uint32_t*>(pFormatProperties);
    fp[0] = 0; fp[1] = 0; fp[2] = 0;""",
        "if ((fp[0] | fp[1] | fp[2]) != 0U || format == 0U) return;",
        """fp[0] = 0x7FFFFFFFU;
    fp[1] = 0x7FFFFFFFU;
    fp[2] = 0x7FFFFFFFU;""",
    ),
    (
        "void",
        "vkGetPhysicalDeviceFormatProperties2",
        [("void*", "physicalDevice"), ("uint32_t", "format"), ("void*", "pFormatProperties2")],
        ["pdev->aot_internal", "format", "pFormatProperties2"],
        ["pdev"],
        """if (!pFormatProperties2) return;
    uint32_t* fp = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pFormatProperties2) + 16);
    fp[0] = 0; fp[1] = 0; fp[2] = 0;""",
        "if ((fp[0] | fp[1] | fp[2]) != 0U || format == 0U) return;",
        "vkGetPhysicalDeviceFormatProperties(physicalDevice, format, static_cast<uint8_t*>(pFormatProperties2) + 16);",
    ),
    (
        "int32_t",
        "vkGetPhysicalDeviceImageFormatProperties",
        [
            ("void*", "physicalDevice"), ("uint32_t", "format"), ("uint32_t", "type"),
            ("uint32_t", "tiling"), ("uint32_t", "usage"), ("uint32_t", "flags"),
            ("void*", "pImageFormatProperties"),
        ],
        ["pdev->aot_internal", "format", "type", "tiling", "usage", "flags", "pImageFormatProperties"],
        ["pdev", "pImageFormatProperties"],
        "",
        "",
        """if (pImageFormatProperties) {
        uint32_t* ifp = static_cast<uint32_t*>(pImageFormatProperties);
        ifp[0] = 16384U; ifp[1] = 16384U; ifp[2] = 2048U;
        ifp[3] = 15U;
        ifp[4] = 2048U;
        ifp[5] = 0x0FU;
        *reinterpret_cast<uint64_t*>(ifp + 6) = (4ULL << 30);
    }
    return 0;""",
    ),
    (
        "int32_t",
        "vkGetPhysicalDeviceImageFormatProperties2",
        [("void*", "physicalDevice"), ("const void*", "pImageFormatInfo"), ("void*", "pImageFormatProperties2")],
        ["pdev->aot_internal", "pImageFormatInfo", "pImageFormatProperties2"],
        ["pdev", "pImageFormatInfo"],
        "if (!pImageFormatProperties2) return -3;",
        "",
        "return vkGetPhysicalDeviceImageFormatProperties(physicalDevice, 37U, 1U, 0U, 0x1FU, 0U, static_cast<uint8_t*>(pImageFormatProperties2) + 16);",
    ),
    (
        "int32_t",
        "vkEnumerateDeviceLayerProperties",
        [("void*", "physicalDevice"), ("uint32_t*", "pPropertyCount"), ("void*", "pProperties")],
        ["pdev->aot_internal", "pPropertyCount", "pProperties"],
        ["pdev", "pPropertyCount"],
        "",
        "",
        """if (pPropertyCount) *pPropertyCount = 0;
    return 0;""",
    ),
    (
        "void",
        "vkGetPhysicalDeviceSparseImageFormatProperties",
        [
            ("void*", "physicalDevice"), ("uint32_t", "format"), ("uint32_t", "type"),
            ("uint32_t", "samples"), ("uint32_t", "usage"), ("uint32_t", "tiling"),
            ("uint32_t*", "pPropertyCount"), ("void*", "pProperties"),
        ],
        ["pdev->aot_internal", "format", "type", "samples", "usage", "tiling", "pPropertyCount", "pProperties"],
        ["pdev", "pPropertyCount"],
        "",
        "",
        "if (pPropertyCount) *pPropertyCount = 0;",
    ),
    (
        "void",
        "vkGetPhysicalDeviceSparseImageFormatProperties2",
        [("void*", "physicalDevice"), ("const void*", "pFormatInfo"), ("uint32_t*", "pPropertyCount"), ("void*", "pProperties")],
        ["pdev->aot_internal", "pFormatInfo", "pPropertyCount", "pProperties"],
        ["pdev", "pFormatInfo", "pPropertyCount"],
        "",
        "",
        "if (pPropertyCount) *pPropertyCount = 0;",
    ),
    (
        "void",
        "vkGetPhysicalDeviceExternalBufferProperties",
        [("void*", "physicalDevice"), ("const void*", "pExternalBufferInfo"), ("void*", "pExternalBufferProperties")],
        ["pdev->aot_internal", "pExternalBufferInfo", "pExternalBufferProperties"],
        ["pdev", "pExternalBufferInfo"],
        "if (!pExternalBufferProperties) return;",
        "",
        """uint32_t* ext = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pExternalBufferProperties) + 16);
    ext[0] = 0x03U;
    ext[1] = 0U;
    ext[2] = 0U;""",
    ),
    (
        "void",
        "vkGetPhysicalDeviceExternalFenceProperties",
        [("void*", "physicalDevice"), ("const void*", "pExternalFenceInfo"), ("void*", "pExternalFenceProperties")],
        ["pdev->aot_internal", "pExternalFenceInfo", "pExternalFenceProperties"],
        ["pdev", "pExternalFenceInfo"],
        "if (!pExternalFenceProperties) return;",
        "",
        """uint32_t* ext = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pExternalFenceProperties) + 16);
    ext[0] = 0U; ext[1] = 0U; ext[2] = 0U;""",
    ),
    (
        "void",
        "vkGetPhysicalDeviceExternalSemaphoreProperties",
        [("void*", "physicalDevice"), ("const void*", "pExternalSemaphoreInfo"), ("void*", "pExternalSemaphoreProperties")],
        ["pdev->aot_internal", "pExternalSemaphoreInfo", "pExternalSemaphoreProperties"],
        ["pdev", "pExternalSemaphoreInfo"],
        "if (!pExternalSemaphoreProperties) return;",
        "",
        """uint32_t* ext = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pExternalSemaphoreProperties) + 16);
    ext[0] = 0U; ext[1] = 0U; ext[2] = 0U;""",
    ),
]


def main() -> None:
    lines = [
        "// ============================================================================",
        "// AUTO-GENERATED BY scripts/gen_r54p3_physdev_wrappers.py — DO NOT EDIT.",
        "// Forwarding physical-device entrypoints into r54p3_lifted.o (aot_vk*).",
        "// ============================================================================",
        "",
    ]
    for ret, name, params, aot_args, checks, pre, post_check, fallback in WRAPPERS:
        sig = ", ".join(f"{t} {a}" for t, a in params)
        cond = " && ".join([f"!g_aot_rt.mapper.so_path_loaded.empty()", f"aot_{name}"] + checks)
        args_str = ", ".join(aot_args)

        lines.append('__attribute__((visibility("default")))')
        lines.append(f"{ret} {name}({sig}) {{")
        if pre:
            lines.append(f"    {pre}")
        lines.append("    g_aot_rt.ensureInitialized();")
        lines.append("    auto* pdev = static_cast<MaliIcdPhysicalDevice*>(physicalDevice);")
        lines.append(f"    if ({cond}) {{")
        lines.append("        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);")
        lines.append("        pdev->initAotInternalLayout();")
        if ret == "void":
            lines.append("        mali_aapcs64::invokeLiftedAapcs64<void>(")
            lines.append(f"            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_{name},")
            lines.append(f"            {args_str});")
            lines.append("        g_aot_rt.lifted_vk_calls_executed++;")
            if post_check:
                lines.append(f"        {post_check}")
            else:
                lines.append("        return;")
        else:
            lines.append(f"        {ret} r = mali_aapcs64::invokeLiftedAapcs64<{ret}>(")
            lines.append(f"            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_{name},")
            lines.append(f"            {args_str});")
            lines.append("        g_aot_rt.lifted_vk_calls_executed++;")
            lines.append("        return r;")
        lines.append("    }")
        if fallback:
            lines.append(f"    {fallback}")
        lines.append("}")
        lines.append("")

    OUT_PATH.write_text("\n".join(lines), encoding="utf-8")
    print(f"Generated {len(WRAPPERS)} physical-device wrappers -> {OUT_PATH}")


if __name__ == "__main__":
    main()
