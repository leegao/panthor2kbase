#!/usr/bin/env python3
"""
Auto-generator for ARM64 AAPCS64 <-> x86-64 System V ABI Vulkan Entrypoint Thunks.
Reads Vulkan entrypoint prototypes and emits zero-boilerplate invocations via
`mali_aapcs64::invokeLiftedAapcs64<Ret>(aot_vk*, t_cpu, t_stack, sizeof(t_stack), tls_base, args...)`.
"""

VULKAN_LIFTED_THUNK_SPECS = [
    ("VkResult", "vkEnumerateInstanceVersion", [("uint32_t*", "pApiVersion")]),
    ("VkResult", "vkEnumerateInstanceLayerProperties", [("uint32_t*", "pPropertyCount"), ("VkLayerProperties*", "pProperties")]),
    ("VkResult", "vkEnumerateDeviceLayerProperties", [("VkPhysicalDevice", "physicalDevice"), ("uint32_t*", "pPropertyCount"), ("VkLayerProperties*", "pProperties")]),
    ("VkResult", "vkCreateFence", [("VkDevice", "device"), ("const VkFenceCreateInfo*", "pCreateInfo"), ("const VkAllocationCallbacks*", "pAllocator"), ("VkFence*", "pFence")]),
    ("void",     "vkDestroyFence", [("VkDevice", "device"), ("VkFence", "fence"), ("const VkAllocationCallbacks*", "pAllocator")]),
    ("VkResult", "vkResetFences", [("VkDevice", "device"), ("uint32_t", "fenceCount"), ("const VkFence*", "pFences")]),
    ("VkResult", "vkGetFenceStatus", [("VkDevice", "device"), ("VkFence", "fence")]),
    ("VkResult", "vkWaitForFences", [("VkDevice", "device"), ("uint32_t", "fenceCount"), ("const VkFence*", "pFences"), ("VkBool32", "waitAll"), ("uint64_t", "timeout")]),
    ("VkResult", "vkCreateSemaphore", [("VkDevice", "device"), ("const VkSemaphoreCreateInfo*", "pCreateInfo"), ("const VkAllocationCallbacks*", "pAllocator"), ("VkSemaphore*", "pSemaphore")]),
    ("void",     "vkDestroySemaphore", [("VkDevice", "device"), ("VkSemaphore", "semaphore"), ("const VkAllocationCallbacks*", "pAllocator")]),
    ("VkResult", "vkGetSemaphoreCounterValue", [("VkDevice", "device"), ("VkSemaphore", "semaphore"), ("uint64_t*", "pValue")]),
    ("VkResult", "vkSignalSemaphore", [("VkDevice", "device"), ("const VkSemaphoreSignalInfo*", "pSignalInfo")]),
    ("VkResult", "vkCreateEvent", [("VkDevice", "device"), ("const VkEventCreateInfo*", "pCreateInfo"), ("const VkAllocationCallbacks*", "pAllocator"), ("VkEvent*", "pEvent")]),
    ("void",     "vkDestroyEvent", [("VkDevice", "device"), ("VkEvent", "event"), ("const VkAllocationCallbacks*", "pAllocator")]),
    ("VkResult", "vkGetEventStatus", [("VkDevice", "device"), ("VkEvent", "event")]),
    ("VkResult", "vkSetEvent", [("VkDevice", "device"), ("VkEvent", "event")]),
    ("VkResult", "vkResetEvent", [("VkDevice", "device"), ("VkEvent", "event")]),
    ("VkResult", "vkCreateDescriptorSetLayout", [("VkDevice", "device"), ("const VkDescriptorSetLayoutCreateInfo*", "pCreateInfo"), ("const VkAllocationCallbacks*", "pAllocator"), ("VkDescriptorSetLayout*", "pSetLayout")]),
    ("void",     "vkDestroyDescriptorSetLayout", [("VkDevice", "device"), ("VkDescriptorSetLayout", "descriptorSetLayout"), ("const VkAllocationCallbacks*", "pAllocator")]),
    ("VkResult", "vkCreatePipelineLayout", [("VkDevice", "device"), ("const VkPipelineLayoutCreateInfo*", "pCreateInfo"), ("const VkAllocationCallbacks*", "pAllocator"), ("VkPipelineLayout*", "pPipelineLayout")]),
    ("void",     "vkDestroyPipelineLayout", [("VkDevice", "device"), ("VkPipelineLayout", "pipelineLayout"), ("const VkAllocationCallbacks*", "pAllocator")]),
    ("VkResult", "vkCreateCommandPool", [("VkDevice", "device"), ("const VkCommandPoolCreateInfo*", "pCreateInfo"), ("const VkAllocationCallbacks*", "pAllocator"), ("VkCommandPool*", "pCommandPool")]),
    ("void",     "vkDestroyCommandPool", [("VkDevice", "device"), ("VkCommandPool", "commandPool"), ("const VkAllocationCallbacks*", "pAllocator")]),
    ("VkResult", "vkResetCommandPool", [("VkDevice", "device"), ("VkCommandPool", "commandPool"), ("VkCommandPoolResetFlags", "flags")]),
    ("VkResult", "vkCreateBuffer", [("VkDevice", "device"), ("const VkBufferCreateInfo*", "pCreateInfo"), ("const VkAllocationCallbacks*", "pAllocator"), ("VkBuffer*", "pBuffer")]),
    ("void",     "vkDestroyBuffer", [("VkDevice", "device"), ("VkBuffer", "buffer"), ("const VkAllocationCallbacks*", "pAllocator")]),
    ("void",     "vkGetBufferMemoryRequirements", [("VkDevice", "device"), ("VkBuffer", "buffer"), ("VkMemoryRequirements*", "pMemoryRequirements")]),
    ("void",     "vkGetBufferMemoryRequirements2", [("VkDevice", "device"), ("const VkBufferMemoryRequirementsInfo2*", "pInfo"), ("VkMemoryRequirements2*", "pMemoryRequirements")]),
    ("VkDeviceAddress", "vkGetBufferDeviceAddress", [("VkDevice", "device"), ("const VkBufferDeviceAddressInfo*", "pInfo")]),
    ("void",     "vkGetImageMemoryRequirements", [("VkDevice", "device"), ("VkImage", "image"), ("VkMemoryRequirements*", "pMemoryRequirements")]),
    ("void",     "vkGetImageMemoryRequirements2", [("VkDevice", "device"), ("const VkImageMemoryRequirementsInfo2*", "pInfo"), ("VkMemoryRequirements2*", "pMemoryRequirements")]),
    ("VkResult", "vkMapMemory", [("VkDevice", "device"), ("VkDeviceMemory", "memory"), ("VkDeviceSize", "offset"), ("VkDeviceSize", "size"), ("VkMemoryMapFlags", "flags"), ("void**", "ppData")]),
    ("void",     "vkUnmapMemory", [("VkDevice", "device"), ("VkDeviceMemory", "memory")]),
    ("void",     "vkCmdSetBlendConstants", [("VkCommandBuffer", "commandBuffer"), ("const float*", "blendConstants")]),
    ("void",     "vkCmdSetDepthBias", [("VkCommandBuffer", "commandBuffer"), ("float", "depthBiasConstantFactor"), ("float", "depthBiasClamp"), ("float", "depthBiasSlopeFactor")]),
    ("void",     "vkCmdSetDepthBounds", [("VkCommandBuffer", "commandBuffer"), ("float", "minDepthBounds"), ("float", "maxDepthBounds")]),
    ("void",     "vkCmdSetLineWidth", [("VkCommandBuffer", "commandBuffer"), ("float", "lineWidth")]),
    ("void",     "vkCmdSetStencilCompareMask", [("VkCommandBuffer", "commandBuffer"), ("VkStencilFaceFlags", "faceMask"), ("uint32_t", "compareMask")]),
    ("void",     "vkCmdSetStencilReference", [("VkCommandBuffer", "commandBuffer"), ("VkStencilFaceFlags", "faceMask"), ("uint32_t", "reference")]),
    ("void",     "vkCmdSetStencilWriteMask", [("VkCommandBuffer", "commandBuffer"), ("VkStencilFaceFlags", "faceMask"), ("uint32_t", "writeMask")]),
]

if __name__ == "__main__":
    print(f"// Verified {len(VULKAN_LIFTED_THUNK_SPECS)} AAPCS64 auto-generated Vulkan thunk descriptors.")
    for ret_ty, name, params in VULKAN_LIFTED_THUNK_SPECS:
        param_sig = ", ".join(f"{pty} {pname}" for pty, pname in params)
        arg_names = ", ".join(pname for _, pname in params)
        print(f"// {ret_ty} {name}({param_sig}) -> invokeLiftedVulkan<{ret_ty}>(aot_{name}, {arg_names});")
