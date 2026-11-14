#!/usr/bin/env python3
"""Auto-generate Vulkan ICD wrappers from specs/vk.xml into r54p3_lifted.o (aot_vk*)."""

from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
VK_XML = ROOT / "specs" / "vk.xml"
PHYSDEV_OUT = ROOT / "src" / "r54p3_vulkan_icd" / "generated" / "mali_physdev_wrappers_autogen.inc"
DEVICE_OUT = ROOT / "src" / "r54p3_vulkan_icd" / "generated" / "mali_device_wrappers_autogen.inc"

# Keyed purely by Vulkan command name; signatures come from specs/vk.xml.
PHYSDEV_COMMANDS = [
    "vkGetPhysicalDeviceFeatures",
    "vkGetPhysicalDeviceFeatures2",
    "vkGetPhysicalDeviceQueueFamilyProperties",
    "vkGetPhysicalDeviceQueueFamilyProperties2",
    "vkGetPhysicalDeviceMemoryProperties",
    "vkGetPhysicalDeviceMemoryProperties2",
    "vkGetPhysicalDeviceFormatProperties",
    "vkGetPhysicalDeviceFormatProperties2",
    "vkGetPhysicalDeviceImageFormatProperties",
    "vkGetPhysicalDeviceImageFormatProperties2",
    "vkEnumerateDeviceLayerProperties",
    "vkGetPhysicalDeviceSparseImageFormatProperties",
    "vkGetPhysicalDeviceSparseImageFormatProperties2",
    "vkGetPhysicalDeviceExternalBufferProperties",
    "vkGetPhysicalDeviceExternalFenceProperties",
    "vkGetPhysicalDeviceExternalSemaphoreProperties",
    "vkEnumeratePhysicalDeviceGroups",
]

# Manually implemented in src/r54p3_vulkan_icd/mali_icd_physdev.cpp, but we still
# auto-generate `aot_forward_<Name>(...)` helpers from specs/vk.xml so the manual
# wrappers don't duplicate runtime init / lock / AAPCS64 thunking boilerplate:
#   - vkEnumerateDeviceExtensionProperties: appends VK_KHR_swapchain when libGLES_mali.so omits it.
#   - vkGetPhysicalDeviceProperties: clears output struct before forwarding to native VkPhysicalDevice.
#   - vkGetPhysicalDeviceProperties2: clears/forwards to native VkPhysicalDevice.
MANUAL_PHYSDEV_FORWARDERS: list[str] = [
    "vkEnumerateDeviceExtensionProperties",
    "vkGetPhysicalDeviceProperties",
    "vkGetPhysicalDeviceProperties2",
]

DEVICE_COMMANDS = [
    "vkCreateDevice",
    "vkDestroyDevice",
    "vkGetDeviceQueue",
    "vkDeviceWaitIdle",
    "vkQueueWaitIdle",
    "vkQueueSubmit",
    "vkQueueSubmit2",
    "vkAllocateMemory",
    "vkFreeMemory",
    "vkMapMemory",
    "vkUnmapMemory",
    "vkMapMemory2",
    "vkUnmapMemory2",
    "vkCreateBuffer",
    "vkDestroyBuffer",
    "vkGetBufferMemoryRequirements",
    "vkBindBufferMemory",
    "vkCreateImage",
    "vkDestroyImage",
    "vkGetImageMemoryRequirements",
    "vkBindImageMemory",
    "vkCreateImageView",
    "vkDestroyImageView",
    "vkCreateSampler",
    "vkDestroySampler",
    "vkCreateRenderPass",
    "vkDestroyRenderPass",
    "vkCreateFramebuffer",
    "vkDestroyFramebuffer",
    "vkCreateGraphicsPipelines",
    "vkCreateComputePipelines",
    "vkDestroyPipeline",
    "vkAllocateCommandBuffers",
    "vkFreeCommandBuffers",
    "vkBeginCommandBuffer",
    "vkEndCommandBuffer",
    "vkResetCommandBuffer",
    "vkCmdBindPipeline",
    "vkCmdBindDescriptorSets",
    "vkCmdBindVertexBuffers",
    "vkCmdBindIndexBuffer",
    "vkCmdDraw",
    "vkCmdDrawIndexed",
    "vkCmdDrawIndirect",
    "vkCmdDrawIndexedIndirect",
    "vkCmdDispatch",
    "vkCreatePipelineCache",
    "vkDestroyPipelineCache",
    "vkGetPipelineCacheData",
    "vkMergePipelineCaches",
    "vkCreateEvent",
    "vkDestroyEvent",
    "vkGetEventStatus",
    "vkSetEvent",
    "vkResetEvent",
    "vkCmdSetEvent",
    "vkCmdResetEvent",
    "vkCmdWaitEvents",
    "vkCmdPipelineBarrier",
    "vkCmdPipelineBarrier2",
    "vkCreateFence",
    "vkDestroyFence",
    "vkResetFences",
    "vkGetFenceStatus",
    "vkWaitForFences",
    "vkCreateSemaphore",
    "vkDestroySemaphore",
    "vkGetSemaphoreCounterValue",
    "vkSignalSemaphore",
    "vkWaitSemaphores",
    "vkCreateCommandPool",
    "vkDestroyCommandPool",
    "vkResetCommandPool",
    "vkCreateDescriptorPool",
    "vkDestroyDescriptorPool",
    "vkResetDescriptorPool",
    "vkCreateDescriptorSetLayout",
    "vkDestroyDescriptorSetLayout",
    "vkAllocateDescriptorSets",
    "vkFreeDescriptorSets",
    "vkUpdateDescriptorSets",
    "vkCreatePipelineLayout",
    "vkDestroyPipelineLayout",
    "vkCreateShaderModule",
    "vkDestroyShaderModule",
    "vkFlushMappedMemoryRanges",
    "vkInvalidateMappedMemoryRanges",
    "vkCreateQueryPool",
    "vkDestroyQueryPool",
    "vkGetQueryPoolResults",
    "vkCmdBeginRenderPass",
    "vkCmdEndRenderPass",
    "vkCmdNextSubpass",
    "vkCmdCopyBuffer",
    "vkCmdCopyImage",
    "vkCmdBlitImage",
    "vkCmdCopyBufferToImage",
    "vkCmdCopyImageToBuffer",
    "vkCmdFillBuffer",
    "vkCmdUpdateBuffer",
    "vkCmdPushConstants",
    "vkCmdBeginQuery",
    "vkCmdEndQuery",
    "vkCmdResetQueryPool",
    "vkCmdWriteTimestamp",
]


def load_vk_commands(xml_path: Path) -> dict[str, tuple[str, list[tuple[str, str]]]]:
    tree = ET.parse(xml_path)
    root = tree.getroot()
    cmds: dict[str, tuple[str, list[tuple[str, str]]]] = {}
    aliases: list[tuple[str, str]] = []

    for cmd in root.findall(".//commands/command"):
        api = cmd.get("api")
        if api is not None and "vulkan" not in api.split(","):
            continue
        alias = cmd.get("alias")
        name_attr = cmd.get("name")
        if alias and name_attr:
            aliases.append((name_attr, alias))
            continue
        proto = cmd.find("proto")
        if proto is None:
            continue
        name_el = proto.find("name")
        if name_el is None or not name_el.text:
            continue
        cmd_name = name_el.text.strip()
        proto_text = "".join(proto.itertext()).strip()
        ret_type = proto_text[: proto_text.rfind(cmd_name)].strip()

        params: list[tuple[str, str]] = []
        for p in cmd.findall("param"):
            papi = p.get("api")
            if papi is not None and "vulkan" not in papi.split(","):
                continue
            pname_el = p.find("name")
            if pname_el is None or not pname_el.text:
                continue
            pname = pname_el.text.strip()
            ptext = " ".join("".join(p.itertext()).split())
            idx = ptext.rfind(pname)
            ptype = ptext[:idx].strip() if idx >= 0 else ptext
            # Preserve array brackets if present after name
            suffix = ptext[idx + len(pname):].strip() if idx >= 0 else ""
            if suffix.startswith("["):
                pname = pname + suffix
            params.append((ptype, pname))
        cmds[cmd_name] = (ret_type, params)

    for name_attr, alias in aliases:
        if alias in cmds and name_attr not in cmds:
            cmds[name_attr] = cmds[alias]
    return cmds


def emit_physdev_wrappers(cmds: dict[str, tuple[str, list[tuple[str, str]]]]) -> None:
    lines = [
        "// ============================================================================",
        "// AUTO-GENERATED BY scripts/gen_r54p3_physdev_wrappers.py FROM specs/vk.xml",
        "// DO NOT EDIT. Direct delegation into r54p3_lifted.o (aot_vk*) with real",
        "// <vulkan/vulkan.h> types.",
        "// ============================================================================",
        "",
    ]
    for name in PHYSDEV_COMMANDS:
        ret_type, params = cmds[name]
        sig = ", ".join(f"{t} {a}" for t, a in params)
        first_type, first_arg = params[0]
        rest_args = [a for _, a in params[1:]]

        lines.append('__attribute__((visibility("default")))')
        lines.append(f"{ret_type} {name}({sig}) {{")
        lines.append("    g_aot_rt.ensureInitialized();")

        if first_type == "VkPhysicalDevice":
            lines.append(f"    auto* pdev = icdGetPhysicalDevice({first_arg});")
            lines.append(f"    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_{name} && pdev) {{")
            lines.append("        auto& tctx = maliGetThreadGuestContext();")
            lines.append("        GuestStackFrameGuard frame(tctx);")
            lines.append("        const size_t frame_stack_sz = static_cast<size_t>(tctx.cpu.sp - reinterpret_cast<uintptr_t>(tctx.stack_mem));")
            lines.append("        void* aot_pd = pdev->getAotHandle();")
            aot_first = "aot_pd"
        elif first_type == "VkInstance":
            lines.append(f"    auto* inst = icdGetInstance({first_arg});")
            lines.append(f"    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_{name} && inst) {{")
            lines.append("        auto& tctx = maliGetThreadGuestContext();")
            lines.append("        GuestStackFrameGuard frame(tctx);")
            lines.append("        const size_t frame_stack_sz = static_cast<size_t>(tctx.cpu.sp - reinterpret_cast<uintptr_t>(tctx.stack_mem));")
            lines.append("        void* aot_inst = inst->getAotHandle();")
            aot_first = "aot_inst"
        else:
            lines.append(f"    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_{name}) {{")
            lines.append("        auto& tctx = maliGetThreadGuestContext();")
            lines.append("        GuestStackFrameGuard frame(tctx);")
            lines.append("        const size_t frame_stack_sz = static_cast<size_t>(tctx.cpu.sp - reinterpret_cast<uintptr_t>(tctx.stack_mem));")
            aot_first = first_arg

        call_args = ", ".join([aot_first] + rest_args)
        if ret_type == "void":
            lines.append("        mali_aapcs64::invokeLiftedAapcs64<void>(")
            lines.append(f"            tctx.cpu, tctx.stack_mem, frame_stack_sz, aot_{name},")
            lines.append(f"            {call_args});")
            lines.append("        g_aot_rt.lifted_vk_calls_executed++;")
            lines.append("        return;")
        else:
            lines.append(f"        {ret_type} r = mali_aapcs64::invokeLiftedAapcs64<{ret_type}>(")
            lines.append(f"            tctx.cpu, tctx.stack_mem, frame_stack_sz, aot_{name},")
            lines.append(f"            {call_args});")
            lines.append("        g_aot_rt.lifted_vk_calls_executed++;")
            lines.append("        return r;")
        lines.append("    }")
        if ret_type == "VkResult":
            lines.append("    return VK_ERROR_INITIALIZATION_FAILED;")
        elif ret_type != "void":
            lines.append(f"    return {ret_type}{{}};")
        lines.append("}")
        lines.append("")

    for name in MANUAL_PHYSDEV_FORWARDERS:
        ret_type, params = cmds[name]
        sig = ", ".join(f"{t} {a}" for t, a in params)
        first_type, first_arg = params[0]
        rest_args = [a for _, a in params[1:]]
        assert first_type == "VkPhysicalDevice"
        call_args = ", ".join(["aot_pd"] + rest_args)

        lines.append(f"static inline {ret_type} aot_forward_{name}({sig}) {{")
        lines.append("    g_aot_rt.ensureInitialized();")
        lines.append(f"    auto* pdev = icdGetPhysicalDevice({first_arg});")
        lines.append(f"    if (!g_aot_rt.mapper.so_path_loaded.empty() && aot_{name} && pdev) {{")
        lines.append("        auto& tctx = maliGetThreadGuestContext();")
        lines.append("        GuestStackFrameGuard frame(tctx);")
        lines.append("        const size_t frame_stack_sz = static_cast<size_t>(tctx.cpu.sp - reinterpret_cast<uintptr_t>(tctx.stack_mem));")
        lines.append("        void* aot_pd = pdev->getAotHandle();")
        if ret_type == "void":
            lines.append("        mali_aapcs64::invokeLiftedAapcs64<void>(")
            lines.append(f"            tctx.cpu, tctx.stack_mem, frame_stack_sz, aot_{name},")
            lines.append(f"            {call_args});")
            lines.append("        g_aot_rt.lifted_vk_calls_executed++;")
            lines.append("        return;")
        else:
            lines.append(f"        {ret_type} r = mali_aapcs64::invokeLiftedAapcs64<{ret_type}>(")
            lines.append(f"            tctx.cpu, tctx.stack_mem, frame_stack_sz, aot_{name},")
            lines.append(f"            {call_args});")
            lines.append("        g_aot_rt.lifted_vk_calls_executed++;")
            lines.append("        return r;")
        lines.append("    }")
        if ret_type == "VkResult":
            lines.append("    return VK_ERROR_INITIALIZATION_FAILED;")
        elif ret_type != "void":
            lines.append(f"    return {ret_type}{{}};")
        lines.append("}")
        lines.append("")

    PHYSDEV_OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"Generated {len(PHYSDEV_COMMANDS)} physical-device wrappers -> {PHYSDEV_OUT}")


def emit_device_wrappers(cmds: dict[str, tuple[str, list[tuple[str, str]]]]) -> None:
    lines = [
        "// ============================================================================",
        "// AUTO-GENERATED BY scripts/gen_r54p3_physdev_wrappers.py FROM specs/vk.xml",
        "// DO NOT EDIT. Typed <vulkan/vulkan.h> forwarding thunks into r54p3_lifted.o.",
        "// ============================================================================",
        "",
    ]
    for name in DEVICE_COMMANDS:
        ret_type, params = cmds[name]
        sig = ", ".join(f"{t} {a}" for t, a in params)
        raw_names = [a.split("[")[0] for _, a in params]
        first_type, first_arg = params[0]
        if first_type in ("VkPhysicalDevice", "VkInstance", "VkDevice", "VkQueue", "VkCommandBuffer"):
            raw_names[0] = f"reinterpret_cast<{first_type}>(resolveAotHandle({first_arg}))"
        arg_names = ", ".join(raw_names)
        lines.append(f"static inline {ret_type} aot_forward_{name}({sig}) {{")
        lines.append("    g_aot_rt.ensureInitialized();")
        lines.append(f"    if (!aot_{name}) {{")
        if ret_type == "VkResult":
            lines.append("        return VK_ERROR_EXTENSION_NOT_PRESENT;")
        elif ret_type == "void":
            lines.append("        return;")
        else:
            lines.append(f"        return {ret_type}{{}};")
        lines.append("    }")
        lines.append("    auto& tctx = maliGetThreadGuestContext();")
        lines.append("    GuestStackFrameGuard frame(tctx);")
        lines.append("    const size_t frame_stack_sz = static_cast<size_t>(tctx.cpu.sp - reinterpret_cast<uintptr_t>(tctx.stack_mem));")
        if ret_type == "void":
            lines.append("    mali_aapcs64::invokeLiftedAapcs64<void>(")
            lines.append(f"        tctx.cpu, tctx.stack_mem, frame_stack_sz, aot_{name},")
            lines.append(f"        {arg_names});")
            lines.append("    g_aot_rt.lifted_vk_calls_executed++;")
        else:
            lines.append(f"    {ret_type} r = mali_aapcs64::invokeLiftedAapcs64<{ret_type}>(")
            lines.append(f"        tctx.cpu, tctx.stack_mem, frame_stack_sz, aot_{name},")
            lines.append(f"        {arg_names});")
            lines.append("    g_aot_rt.lifted_vk_calls_executed++;")
            lines.append("    return r;")
        lines.append("}")
        lines.append("")

    DEVICE_OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"Generated {len(DEVICE_COMMANDS)} typed device/cmd wrappers -> {DEVICE_OUT}")


def main() -> None:
    cmds = load_vk_commands(VK_XML)
    emit_physdev_wrappers(cmds)
    emit_device_wrappers(cmds)


if __name__ == "__main__":
    main()
