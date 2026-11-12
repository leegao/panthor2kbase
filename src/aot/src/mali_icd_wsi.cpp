// ============================================================================
// src/aot/src/mali_icd_wsi.cpp
// Desktop Linux WSI Bridge (VK_KHR_xcb_surface, VK_KHR_xlib_surface,
// VK_KHR_wayland_surface, VK_KHR_display, VK_EXT_headless_surface,
// VK_KHR_swapchain) over /dev/mali0 GPU backbuffers.
// ============================================================================
#include "mali_icd_internal.h"

extern "C" {

static constexpr uint64_t MALI_ICD_SURFACE_MAGIC = 0x5552464D414C4900ULL;

struct MaliXcbLib {
    void* handle = nullptr;
    typedef uint32_t (*pfn_xcb_generate_id)(void* c);
    typedef struct { unsigned int sequence; } xcb_void_cookie_t;
    typedef struct { unsigned int sequence; } xcb_get_geometry_cookie_t;
    struct xcb_get_geometry_reply_t {
        uint8_t  response_type;
        uint8_t  depth;
        uint16_t sequence;
        uint32_t length;
        uint32_t root;
        int16_t  x, y;
        uint16_t width, height;
        uint16_t border_width;
        uint8_t  pad0[2];
    };
    typedef xcb_get_geometry_cookie_t (*pfn_xcb_get_geometry)(void* c, uint32_t drawable);
    typedef xcb_get_geometry_reply_t* (*pfn_xcb_get_geometry_reply)(void* c, xcb_get_geometry_cookie_t cookie, void** e);
    typedef xcb_void_cookie_t (*pfn_xcb_create_gc)(void* c, uint32_t cid, uint32_t drawable, uint32_t value_mask, const void* value_list);
    typedef xcb_void_cookie_t (*pfn_xcb_free_gc)(void* c, uint32_t gc);
    typedef xcb_void_cookie_t (*pfn_xcb_put_image)(void* c, uint8_t format, uint32_t drawable, uint32_t gc,
                                                   uint16_t width, uint16_t height, int16_t dst_x, int16_t dst_y,
                                                   uint8_t left_pad, uint8_t depth, uint32_t data_len, const uint8_t* data);
    typedef int (*pfn_xcb_flush)(void* c);

    pfn_xcb_generate_id        generate_id = nullptr;
    pfn_xcb_get_geometry       get_geometry = nullptr;
    pfn_xcb_get_geometry_reply get_geometry_reply = nullptr;
    pfn_xcb_create_gc          create_gc = nullptr;
    pfn_xcb_free_gc            free_gc = nullptr;
    pfn_xcb_put_image          put_image = nullptr;
    pfn_xcb_flush              flush = nullptr;

    bool ensureLoaded() {
        if (handle) return true;
        handle = dlopen("libxcb.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (!handle) handle = dlopen("libxcb.so", RTLD_LAZY | RTLD_LOCAL);
        if (!handle) return false;
        generate_id        = reinterpret_cast<pfn_xcb_generate_id>(dlsym(handle, "xcb_generate_id"));
        get_geometry       = reinterpret_cast<pfn_xcb_get_geometry>(dlsym(handle, "xcb_get_geometry"));
        get_geometry_reply = reinterpret_cast<pfn_xcb_get_geometry_reply>(dlsym(handle, "xcb_get_geometry_reply"));
        create_gc          = reinterpret_cast<pfn_xcb_create_gc>(dlsym(handle, "xcb_create_gc"));
        free_gc            = reinterpret_cast<pfn_xcb_free_gc>(dlsym(handle, "xcb_free_gc"));
        put_image          = reinterpret_cast<pfn_xcb_put_image>(dlsym(handle, "xcb_put_image"));
        flush              = reinterpret_cast<pfn_xcb_flush>(dlsym(handle, "xcb_flush"));
        return generate_id && get_geometry && get_geometry_reply && create_gc && put_image && flush;
    }

    bool queryWindowGeometry(void* conn, uint32_t win, uint32_t* out_w, uint32_t* out_h, uint8_t* out_depth) {
        if (!conn || !win || !ensureLoaded()) return false;
        auto cookie = get_geometry(conn, win);
        auto* reply = get_geometry_reply(conn, cookie, nullptr);
        if (!reply) return false;
        if (out_w && reply->width > 0)  *out_w = reply->width;
        if (out_h && reply->height > 0) *out_h = reply->height;
        if (out_depth && reply->depth > 0) *out_depth = reply->depth;
        std::free(reply);
        return true;
    }
};

static MaliXcbLib g_xcb_lib;

struct MaliWaylandLib {
    void* handle = nullptr;
    typedef int   (*pfn_wl_display_roundtrip)(void* display);
    typedef int   (*pfn_wl_display_flush)(void* display);
    typedef int   (*pfn_wl_display_dispatch_pending)(void* display);
    typedef void  (*pfn_wl_proxy_marshal)(void* p, uint32_t opcode, ...);
    typedef void* (*pfn_wl_proxy_marshal_constructor)(void* proxy, uint32_t opcode, const void* interface, ...);
    typedef void* (*pfn_wl_proxy_marshal_constructor_versioned)(void* proxy, uint32_t opcode, const void* interface, uint32_t version, ...);
    typedef int   (*pfn_wl_proxy_add_listener)(void* proxy, void (**implementation)(void), void* data);
    typedef void  (*pfn_wl_proxy_destroy)(void* proxy);

    pfn_wl_display_roundtrip                     display_roundtrip = nullptr;
    pfn_wl_display_flush                         display_flush = nullptr;
    pfn_wl_display_dispatch_pending              display_dispatch_pending = nullptr;
    pfn_wl_proxy_marshal                         proxy_marshal = nullptr;
    pfn_wl_proxy_marshal_constructor             proxy_marshal_constructor = nullptr;
    pfn_wl_proxy_marshal_constructor_versioned   proxy_marshal_constructor_versioned = nullptr;
    pfn_wl_proxy_add_listener                    proxy_add_listener = nullptr;
    pfn_wl_proxy_destroy                         proxy_destroy = nullptr;
    const void*                                  registry_interface = nullptr;
    const void*                                  shm_interface = nullptr;
    const void*                                  shm_pool_interface = nullptr;
    const void*                                  buffer_interface = nullptr;

    bool ensureLoaded() {
        if (handle) return true;
        handle = dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_LOCAL);
        if (!handle) handle = dlopen("libwayland-client.so", RTLD_LAZY | RTLD_LOCAL);
        if (!handle) return false;
        display_roundtrip                   = reinterpret_cast<pfn_wl_display_roundtrip>(dlsym(handle, "wl_display_roundtrip"));
        display_flush                       = reinterpret_cast<pfn_wl_display_flush>(dlsym(handle, "wl_display_flush"));
        display_dispatch_pending            = reinterpret_cast<pfn_wl_display_dispatch_pending>(dlsym(handle, "wl_display_dispatch_pending"));
        proxy_marshal                       = reinterpret_cast<pfn_wl_proxy_marshal>(dlsym(handle, "wl_proxy_marshal"));
        proxy_marshal_constructor           = reinterpret_cast<pfn_wl_proxy_marshal_constructor>(dlsym(handle, "wl_proxy_marshal_constructor"));
        proxy_marshal_constructor_versioned = reinterpret_cast<pfn_wl_proxy_marshal_constructor_versioned>(dlsym(handle, "wl_proxy_marshal_constructor_versioned"));
        proxy_add_listener                  = reinterpret_cast<pfn_wl_proxy_add_listener>(dlsym(handle, "wl_proxy_add_listener"));
        proxy_destroy                       = reinterpret_cast<pfn_wl_proxy_destroy>(dlsym(handle, "wl_proxy_destroy"));
        registry_interface                  = dlsym(handle, "wl_registry_interface");
        shm_interface                       = dlsym(handle, "wl_shm_interface");
        shm_pool_interface                  = dlsym(handle, "wl_shm_pool_interface");
        buffer_interface                    = dlsym(handle, "wl_buffer_interface");
        return display_roundtrip && display_flush && proxy_marshal &&
               proxy_marshal_constructor && proxy_marshal_constructor_versioned &&
               proxy_add_listener && registry_interface && shm_interface &&
               shm_pool_interface && buffer_interface;
    }

    static void onRegistryGlobal(void* data, void* registry, uint32_t name, const char* interface, uint32_t version);
    static void onRegistryGlobalRemove(void* /*data*/, void* /*registry*/, uint32_t /*name*/) {}
};

static MaliWaylandLib g_wayland_lib;

void MaliWaylandLib::onRegistryGlobal(void* data, void* registry, uint32_t name, const char* interface, uint32_t /*version*/) {
    void** out_shm = static_cast<void**>(data);
    if (out_shm && !*out_shm && interface && std::strcmp(interface, "wl_shm") == 0) {
        *out_shm = g_wayland_lib.proxy_marshal_constructor_versioned(
            registry, 0U /* WL_REGISTRY_BIND */, g_wayland_lib.shm_interface, 1U,
            name, "wl_shm", 1U, nullptr);
    }
}

struct MaliIcdSurface {
    uint64_t magic      = MALI_ICD_SURFACE_MAGIC;
    uint32_t width      = (getenv("MALI_VKCUBE_HIRES") != nullptr) ? 500U : 64U;
    uint32_t height     = (getenv("MALI_VKCUBE_HIRES") != nullptr) ? 500U : 64U;
    void*    xcb_conn   = nullptr;
    uint32_t xcb_window = 0;
    uint8_t  xcb_depth  = 24;
    bool     is_wayland = false;
    void*    wl_display = nullptr;
    void*    wl_surface = nullptr;
};

static void* xlibDisplayToXcbConnection(void* dpy) {
    if (!dpy) return nullptr;
    static void* s_x11_xcb = nullptr;
    typedef void* (*pfn_XGetXCBConnection)(void*);
    static pfn_XGetXCBConnection s_fn = nullptr;
    if (!s_fn) {
        s_x11_xcb = dlopen("libX11-xcb.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (!s_x11_xcb) s_x11_xcb = dlopen("libX11-xcb.so", RTLD_LAZY | RTLD_LOCAL);
        if (s_x11_xcb)
            s_fn = reinterpret_cast<pfn_XGetXCBConnection>(dlsym(s_x11_xcb, "XGetXCBConnection"));
    }
    return s_fn ? s_fn(dpy) : nullptr;
}

// Unwrap either our own MaliIcdSurface* OR Khronos loader's VkIcdSurfaceXcb* (platform=3),
// VkIcdSurfaceXlib* (platform=4), or VkIcdSurfaceWayland* (platform=1).
static void resolveSurfaceInfo(uint64_t surface, uint32_t* out_w, uint32_t* out_h,
                               void** out_xcb_conn, uint32_t* out_xcb_win, uint8_t* out_xcb_depth,
                               bool* out_is_wayland = nullptr,
                               void** out_wl_display = nullptr, void** out_wl_surface = nullptr) {
    uint32_t default_dim = (getenv("MALI_VKCUBE_HIRES") != nullptr) ? 500U : 64U;
    if (out_w) *out_w = default_dim;
    if (out_h) *out_h = default_dim;
    if (out_xcb_conn)   *out_xcb_conn = nullptr;
    if (out_xcb_win)    *out_xcb_win = 0;
    if (out_xcb_depth)  *out_xcb_depth = 24;
    if (out_is_wayland) *out_is_wayland = false;
    if (out_wl_display) *out_wl_display = nullptr;
    if (out_wl_surface) *out_wl_surface = nullptr;
    if (!surface) return;

    const uint64_t* ptr64 = reinterpret_cast<const uint64_t*>(surface);
    if (ptr64[0] == MALI_ICD_SURFACE_MAGIC) {
        const auto* ms = reinterpret_cast<const MaliIcdSurface*>(surface);
        uint32_t w = ms->width, h = ms->height;
        uint8_t depth = ms->xcb_depth;
        if (ms->xcb_conn && ms->xcb_window) {
            g_xcb_lib.queryWindowGeometry(ms->xcb_conn, ms->xcb_window, &w, &h, &depth);
        }
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        if (out_xcb_conn)   *out_xcb_conn = ms->xcb_conn;
        if (out_xcb_win)    *out_xcb_win = ms->xcb_window;
        if (out_xcb_depth)  *out_xcb_depth = depth;
        if (out_is_wayland) *out_is_wayland = ms->is_wayland;
        if (out_wl_display) *out_wl_display = ms->wl_display;
        if (out_wl_surface) *out_wl_surface = ms->wl_surface;
        return;
    }

    uint32_t platform = *reinterpret_cast<const uint32_t*>(surface);
    if (platform == 3U) { // VK_ICD_WSI_PLATFORM_XCB
        void* conn = reinterpret_cast<void*>(ptr64[1]);
        uint32_t win = *reinterpret_cast<const uint32_t*>(surface + 16);
        uint32_t w = 300U, h = 300U;
        uint8_t depth = 24U;
        g_xcb_lib.queryWindowGeometry(conn, win, &w, &h, &depth);
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        if (out_xcb_conn)  *out_xcb_conn = conn;
        if (out_xcb_win)   *out_xcb_win = win;
        if (out_xcb_depth) *out_xcb_depth = depth;
    } else if (platform == 4U) { // VK_ICD_WSI_PLATFORM_XLIB
        void* dpy = reinterpret_cast<void*>(ptr64[1]);
        uint32_t win = static_cast<uint32_t>(ptr64[2]);
        void* conn = xlibDisplayToXcbConnection(dpy);
        uint32_t w = 300U, h = 300U;
        uint8_t depth = 24U;
        if (conn && win)
            g_xcb_lib.queryWindowGeometry(conn, win, &w, &h, &depth);
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        if (out_xcb_conn)  *out_xcb_conn = conn;
        if (out_xcb_win)   *out_xcb_win = win;
        if (out_xcb_depth) *out_xcb_depth = depth;
    } else if (platform == 1U) { // VK_ICD_WSI_PLATFORM_WAYLAND
        if (out_w) *out_w = 500U;
        if (out_h) *out_h = 500U;
        if (out_is_wayland) *out_is_wayland = true;
        if (out_wl_display) *out_wl_display = reinterpret_cast<void*>(ptr64[1]);
        if (out_wl_surface) *out_wl_surface = reinterpret_cast<void*>(ptr64[2]);
    }
}

struct MaliIcdSwapchain {
    MaliIcdDevice*             device = nullptr;
    uint32_t                   width  = 64;
    uint32_t                   height = 64;
    uint32_t                   format = 37; // VK_FORMAT_R8G8B8A8_UNORM
    uint32_t                   next_image_idx = 0;
    void*                      xcb_conn   = nullptr;
    uint32_t                   xcb_window = 0;
    uint32_t                   xcb_gc     = 0;
    uint8_t                    xcb_depth  = 24;
    bool                       is_wayland = false;
    void*                      wl_display = nullptr;
    void*                      wl_surface = nullptr;
    void*                      wl_shm     = nullptr;
    void*                      wl_pool    = nullptr;
    void*                      wl_buffer  = nullptr;
    int                        wl_shm_fd  = -1;
    uint32_t*                  wl_shm_map = nullptr;
    size_t                     wl_shm_sz  = 0;
    std::vector<MaliIcdImage*> images;
    std::vector<int>           dmabuf_fds;
};


// ============================================================================
// VK_KHR_display & VK_EXT_debug_utils Support for Official Khronos vkcube (--wsi display)
// ============================================================================
struct VkDisplayPropertiesKHRIcd {
    uint64_t    display;
    const char* displayName;
    uint32_t    physicalDimensions[2];
    uint32_t    physicalResolution[2];
    uint32_t    supportedTransforms;
    uint32_t    planeReorderPossible;
    uint32_t    persistentContent;
};

struct VkDisplayPlanePropertiesKHRIcd {
    uint64_t currentDisplay;
    uint32_t currentStackIndex;
};

struct VkDisplayModeParametersKHRIcd {
    uint32_t visibleRegion[2];
    uint32_t refreshRate;
};

struct VkDisplayModePropertiesKHRIcd {
    uint64_t                      displayMode;
    VkDisplayModeParametersKHRIcd parameters;
};

struct VkDisplayPlaneCapabilitiesKHRIcd {
    uint32_t supportedAlpha;
    int32_t  minSrcPosition[2];
    int32_t  maxSrcPosition[2];
    uint32_t minSrcExtent[2];
    uint32_t maxSrcExtent[2];
    int32_t  minDstPosition[2];
    int32_t  maxDstPosition[2];
    uint32_t minDstExtent[2];
    uint32_t maxDstExtent[2];
};

__attribute__((visibility("default")))
int32_t vkGetPhysicalDeviceDisplayPropertiesKHR(void* /*physicalDevice*/, uint32_t* pPropertyCount, void* pProperties) {
    if (!pPropertyCount) return -3;
    if (!pProperties) { *pPropertyCount = 1U; return 0; }
    if (*pPropertyCount == 0U) return 5;
    auto* p = static_cast<VkDisplayPropertiesKHRIcd*>(pProperties);
    p->display = 0xD1590001ULL;
    p->displayName = "Mali-G615 Virtual Kbase Display (500x500)";
    p->physicalDimensions[0] = 500U; p->physicalDimensions[1] = 500U;
    uint32_t disp_res = (getenv("MALI_VKCUBE_HIRES") != nullptr) ? 500U : 64U;
    p->physicalResolution[0] = disp_res;  p->physicalResolution[1] = disp_res;
    p->supportedTransforms = 1U; // VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
    p->planeReorderPossible = 0U;
    p->persistentContent = 0U;
    *pPropertyCount = 1U;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkGetPhysicalDeviceDisplayPlanePropertiesKHR(void* /*physicalDevice*/, uint32_t* pPropertyCount, void* pProperties) {
    if (!pPropertyCount) return -3;
    if (!pProperties) { *pPropertyCount = 1U; return 0; }
    if (*pPropertyCount == 0U) return 5;
    auto* p = static_cast<VkDisplayPlanePropertiesKHRIcd*>(pProperties);
    p->currentDisplay = 0xD1590001ULL;
    p->currentStackIndex = 0U;
    *pPropertyCount = 1U;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkGetDisplayPlaneSupportedDisplaysKHR(void* /*physicalDevice*/, uint32_t /*planeIndex*/, uint32_t* pDisplayCount, uint64_t* pDisplays) {
    if (!pDisplayCount) return -3;
    if (!pDisplays) { *pDisplayCount = 1U; return 0; }
    if (*pDisplayCount == 0U) return 5;
    pDisplays[0] = 0xD1590001ULL;
    *pDisplayCount = 1U;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkGetDisplayModePropertiesKHR(void* /*physicalDevice*/, uint64_t /*display*/, uint32_t* pPropertyCount, void* pProperties) {
    if (!pPropertyCount) return -3;
    if (!pProperties) { *pPropertyCount = 1U; return 0; }
    if (*pPropertyCount == 0U) return 5;
    auto* p = static_cast<VkDisplayModePropertiesKHRIcd*>(pProperties);
    p->displayMode = 0xD15900DEULL;
    uint32_t disp_res = (getenv("MALI_VKCUBE_HIRES") != nullptr) ? 500U : 64U;
    p->parameters.visibleRegion[0] = disp_res;
    p->parameters.visibleRegion[1] = disp_res;
    p->parameters.refreshRate = 60000U;
    *pPropertyCount = 1U;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkCreateDisplayModeKHR(void* /*physicalDevice*/, uint64_t /*display*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pMode) {
    if (!pMode) return -3;
    *pMode = 0xD15900DEULL;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkGetDisplayPlaneCapabilitiesKHR(void* /*physicalDevice*/, uint64_t /*mode*/, uint32_t /*planeIndex*/, void* pCapabilities) {
    if (!pCapabilities) return -3;
    auto* c = static_cast<VkDisplayPlaneCapabilitiesKHRIcd*>(pCapabilities);
    std::memset(c, 0, sizeof(*c));
    c->supportedAlpha = 1U; // VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR
    c->minSrcExtent[0] = 16U; c->minSrcExtent[1] = 16U;
    c->maxSrcExtent[0] = 4096U; c->maxSrcExtent[1] = 4096U;
    c->minDstExtent[0] = 16U; c->minDstExtent[1] = 16U;
    c->maxDstExtent[0] = 4096U; c->maxDstExtent[1] = 4096U;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkCreateDebugUtilsMessengerEXT(void* /*instance*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pMessenger) {
    if (pMessenger) *pMessenger = 0xDEB06001ULL;
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyDebugUtilsMessengerEXT(void* /*instance*/, uint64_t /*messenger*/, const void* /*pAllocator*/) {}

__attribute__((visibility("default")))
int32_t vkSetDebugUtilsObjectNameEXT(void* /*device*/, const void* /*pNameInfo*/) { return 0; }

__attribute__((visibility("default")))
void vkCmdBeginDebugUtilsLabelEXT(void* /*commandBuffer*/, const void* /*pLabelInfo*/) {}

__attribute__((visibility("default")))
void vkCmdEndDebugUtilsLabelEXT(void* /*commandBuffer*/) {}


__attribute__((visibility("default")))
int32_t vkCreatePipelineCache(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pPipelineCache) {
    if (pPipelineCache) *pPipelineCache = 0xCA000001ULL;
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyPipelineCache(void* /*device*/, uint64_t /*pipelineCache*/, const void* /*pAllocator*/) {}

__attribute__((visibility("default")))
int32_t vkGetPipelineCacheData(void* /*device*/, uint64_t /*pipelineCache*/, size_t* pDataSize, void* /*pData*/) {
    if (pDataSize) *pDataSize = 0;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkMergePipelineCaches(void* /*device*/, uint64_t /*dstCache*/, uint32_t /*srcCacheCount*/, const uint64_t* /*pSrcCaches*/) {
    return 0;
}

static inline void computeGenericVkFormatPitchAndSize(uint32_t format, uint32_t w, uint32_t h,
                                                      uint64_t* out_row_pitch, uint64_t* out_size) {
    uint32_t bw = 1U, bh = 1U, bpp = 4U;
    if (format >= 131U && format <= 146U) {
        // VK_FORMAT_BC1..BC7 (4x4 compressed blocks: 8B for BC1/BC4, 16B for BC2/BC3/BC5/BC6H/BC7)
        bw = 4U; bh = 4U;
        bpp = (format <= 134U || format == 139U || format == 140U) ? 8U : 16U;
    } else if (format == 9U || format == 13U) {
        bpp = 1U; // R8
    } else if (format == 16U || format == 20U || format == 70U || format == 76U) {
        bpp = 2U; // R8G8 / R16
    } else if (format >= 97U && format <= 99U) {
        bpp = 8U; // RGBA16F / RG32F
    } else if (format >= 106U && format <= 109U) {
        bpp = 16U; // RGBA32F / RGBA32U / RGBA32I
    }
    uint64_t blocks_x = (static_cast<uint64_t>(w) + bw - 1ULL) / bw;
    uint64_t blocks_y = (static_cast<uint64_t>(h) + bh - 1ULL) / bh;
    *out_row_pitch = blocks_x * bpp;
    *out_size      = (*out_row_pitch) * blocks_y;
}

__attribute__((visibility("default")))
void vkGetImageSubresourceLayout(void* device, uint64_t image, const void* pSubresource, void* pLayout) {
    if (!pLayout) return;
    const auto* img = reinterpret_cast<const MaliIcdImage*>(image);
    // 1. Defer directly to lifted libGLES_mali.so (aot_vkGetImageSubresourceLayout @ 0x988788) when backed by a native DDK handle
    if (img && img->aot_image_handle != 0) {
        typedef void (*PFN_aot_vkGetImageSubresourceLayout)(void*, uint64_t, const void*, void*);
        auto fn = reinterpret_cast<PFN_aot_vkGetImageSubresourceLayout>(
            dlsym(RTLD_DEFAULT, "aot_vkGetImageSubresourceLayout"));
        if (fn) {
            fn(device, img->aot_image_handle, pSubresource, pLayout);
            return;
        }
    }
    // 2. Generic Vulkan format-driven subresource layout calculation (uncompressed + BC1..BC7 block formats)
    struct SubLayout { uint64_t offset, size, rowPitch, arrayPitch, depthPitch; };
    auto* l = static_cast<SubLayout*>(pLayout);
    uint32_t w = (img && img->width > 0) ? img->width : 256U;
    uint32_t h = (img && img->height > 0) ? img->height : 256U;
    uint32_t fmt = img ? img->format : 37U;
    uint64_t pitch = 0, total_sz = 0;
    computeGenericVkFormatPitchAndSize(fmt, w, h, &pitch, &total_sz);
    l->offset     = 0;
    l->rowPitch   = pitch;
    l->size       = total_sz;
    l->arrayPitch = total_sz;
    l->depthPitch = total_sz;
}

__attribute__((visibility("default")))
int32_t vkFlushMappedMemoryRanges(void* /*device*/, uint32_t /*memoryRangeCount*/, const void* /*pMemoryRanges*/) { return 0; }

__attribute__((visibility("default")))
int32_t vkInvalidateMappedMemoryRanges(void* /*device*/, uint32_t /*memoryRangeCount*/, const void* /*pMemoryRanges*/) { return 0; }

__attribute__((visibility("default")))
int32_t vkGetFenceStatus(void* /*device*/, uint64_t fence) {
    if (fence && fence != 0xFE000001ULL) {
        return reinterpret_cast<MaliIcdFence*>(fence)->signaled ? 0 : 1 /* VK_NOT_READY */;
    }
    return 0;
}

__attribute__((visibility("default")))
int32_t vkCreateHeadlessSurfaceEXT(void* /*instance*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pSurface) {
    if (!pSurface) return -3;
    *pSurface = reinterpret_cast<uint64_t>(new MaliIcdSurface());
    return 0;
}

__attribute__((visibility("default")))
int32_t vkCreateDisplayPlaneSurfaceKHR(void* instance, const void* pCreateInfo, const void* pAllocator, uint64_t* pSurface) {
    return vkCreateHeadlessSurfaceEXT(instance, pCreateInfo, pAllocator, pSurface);
}

__attribute__((visibility("default")))
int32_t vkCreateXcbSurfaceKHR(void* /*instance*/, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pSurface) {
    if (!pSurface) return -3;
    auto* surf = new MaliIcdSurface();
    if (pCreateInfo) {
        struct VkXcbSurfaceCreateInfoKHR_Icd {
            uint32_t sType;
            const void* pNext;
            uint32_t flags;
            void* connection;
            uint32_t window;
        };
        const auto* xci = static_cast<const VkXcbSurfaceCreateInfoKHR_Icd*>(pCreateInfo);
        surf->xcb_conn   = xci->connection;
        surf->xcb_window = xci->window;
        uint32_t w = 500U, h = 500U;
        uint8_t depth = 24U;
        if (g_xcb_lib.queryWindowGeometry(surf->xcb_conn, surf->xcb_window, &w, &h, &depth)) {
            surf->width     = w;
            surf->height    = h;
            surf->xcb_depth = depth;
        }
    }
    *pSurface = reinterpret_cast<uint64_t>(surf);
    return 0;
}

__attribute__((visibility("default")))
int32_t vkCreateXlibSurfaceKHR(void* /*instance*/, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pSurface) {
    if (!pSurface) return -3;
    auto* surf = new MaliIcdSurface();
    if (pCreateInfo) {
        struct VkXlibSurfaceCreateInfoKHR_Icd {
            uint32_t sType; const void* pNext; uint32_t flags; void* dpy; unsigned long window;
        };
        const auto* xci = static_cast<const VkXlibSurfaceCreateInfoKHR_Icd*>(pCreateInfo);
        surf->xcb_conn   = xlibDisplayToXcbConnection(xci->dpy);
        surf->xcb_window = static_cast<uint32_t>(xci->window);
        uint32_t w = 300U, h = 300U;
        uint8_t depth = 24U;
        if (g_xcb_lib.queryWindowGeometry(surf->xcb_conn, surf->xcb_window, &w, &h, &depth)) {
            surf->width     = w;
            surf->height    = h;
            surf->xcb_depth = depth;
        }
    }
    mali_icd_log("vkCreateXlibSurfaceKHR -> surface=%p xcb_conn=%p win=0x%x (%ux%u)",
                 static_cast<void*>(surf), surf->xcb_conn, surf->xcb_window, surf->width, surf->height);
    *pSurface = reinterpret_cast<uint64_t>(surf);
    return 0;
}

__attribute__((visibility("default")))
int32_t vkCreateWaylandSurfaceKHR(void* /*instance*/, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pSurface) {
    if (!pSurface) return -3;
    auto* surf = new MaliIcdSurface();
    surf->width      = 500U;
    surf->height     = 500U;
    surf->is_wayland = true;
    if (pCreateInfo) {
        struct VkWaylandSurfaceCreateInfoKHR_Icd {
            uint32_t sType;
            const void* pNext;
            uint32_t flags;
            void* display;
            void* surface;
        };
        const auto* wci = static_cast<const VkWaylandSurfaceCreateInfoKHR_Icd*>(pCreateInfo);
        surf->wl_display = wci->display;
        surf->wl_surface = wci->surface;
    }
    mali_icd_log("vkCreateWaylandSurfaceKHR -> surface=%p wl_display=%p wl_surface=%p",
                 static_cast<void*>(surf), surf->wl_display, surf->wl_surface);
    *pSurface = reinterpret_cast<uint64_t>(surf);
    return 0;
}

__attribute__((visibility("default")))
uint32_t vkGetPhysicalDeviceXcbPresentationSupportKHR(void* /*physicalDevice*/, uint32_t /*queueFamilyIndex*/, void* /*connection*/, uint32_t /*visual_id*/) {
    return 1U; // VK_TRUE
}

__attribute__((visibility("default")))
uint32_t vkGetPhysicalDeviceXlibPresentationSupportKHR(void* /*physicalDevice*/, uint32_t /*queueFamilyIndex*/, void* /*dpy*/, uint64_t /*visualID*/) {
    return 1U; // VK_TRUE
}

__attribute__((visibility("default")))
uint32_t vkGetPhysicalDeviceWaylandPresentationSupportKHR(void* /*physicalDevice*/, uint32_t /*queueFamilyIndex*/, void* /*display*/) {
    return 1U; // VK_TRUE
}

__attribute__((visibility("default")))
void vkDestroySurfaceKHR(void* /*instance*/, uint64_t surface, const void* /*pAllocator*/) {
    if (!surface) return;
    const uint64_t* ptr64 = reinterpret_cast<const uint64_t*>(surface);
    if (ptr64[0] == MALI_ICD_SURFACE_MAGIC) {
        delete reinterpret_cast<MaliIcdSurface*>(surface);
    }
}

__attribute__((visibility("default")))
int32_t vkGetPhysicalDeviceSurfaceSupportKHR(void* /*physicalDevice*/, uint32_t /*queueFamilyIndex*/, uint64_t /*surface*/, uint32_t* pSupported) {
    if (!pSupported) return -3;
    *pSupported = 1U;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkGetPhysicalDeviceSurfaceCapabilitiesKHR(void* /*physicalDevice*/, uint64_t surface, void* pSurfaceCapabilities) {
    if (!pSurfaceCapabilities) return -3;
    uint32_t w = 64U, h = 64U;
    bool is_wayland = false;
    resolveSurfaceInfo(surface, &w, &h, nullptr, nullptr, nullptr, &is_wayland);
    mali_icd_log("vkGetPhysicalDeviceSurfaceCapabilitiesKHR(surface=0x%lx) -> %ux%u (is_wayland=%d)",
                 static_cast<unsigned long>(surface), w, h, is_wayland ? 1 : 0);
    struct SurfCaps {
        uint32_t minImageCount, maxImageCount;
        uint32_t currentExtent[2], minImageExtent[2], maxImageExtent[2];
        uint32_t maxImageArrayLayers, supportedTransforms, currentTransform;
        uint32_t supportedCompositeAlpha, supportedUsageFlags;
    };
    auto* caps = static_cast<SurfCaps*>(pSurfaceCapabilities);
    caps->minImageCount = 2U;
    caps->maxImageCount = 4U;
    caps->currentExtent[0] = w; caps->currentExtent[1] = h;
    caps->minImageExtent[0] = 16U; caps->minImageExtent[1] = 16U;
    caps->maxImageExtent[0] = 4096U; caps->maxImageExtent[1] = 4096U;
    caps->maxImageArrayLayers = 1U;
    caps->supportedTransforms = 1U; // VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
    caps->currentTransform = 1U;
    caps->supportedCompositeAlpha = 1U; // VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
    caps->supportedUsageFlags = 0x9FU;  // COLOR_ATTACHMENT | TRANSFER_SRC | TRANSFER_DST | SAMPLED | STORAGE
    return 0;
}

__attribute__((visibility("default")))
int32_t vkGetPhysicalDeviceSurfaceFormatsKHR(void* /*physicalDevice*/, uint64_t /*surface*/, uint32_t* pSurfaceFormatCount, void* pSurfaceFormats) {
    if (!pSurfaceFormatCount) return -3;
    struct SurfFmt { uint32_t format; uint32_t colorSpace; };
    static const SurfFmt kFmts[] = {
        {37U /* VK_FORMAT_R8G8B8A8_UNORM */, 0U},
        {44U /* VK_FORMAT_B8G8R8A8_UNORM */, 0U},
        {50U /* VK_FORMAT_B8G8R8A8_SRGB  */, 0U},
        {43U /* VK_FORMAT_R8G8B8A8_SRGB  */, 0U}
    };
    constexpr uint32_t kTotalFmts = sizeof(kFmts) / sizeof(kFmts[0]);
    if (!pSurfaceFormats) {
        *pSurfaceFormatCount = kTotalFmts;
        return 0;
    }
    uint32_t n = std::min<uint32_t>(*pSurfaceFormatCount, kTotalFmts);
    std::memcpy(pSurfaceFormats, kFmts, n * sizeof(SurfFmt));
    *pSurfaceFormatCount = n;
    return (n < kTotalFmts) ? 5 : 0;
}

__attribute__((visibility("default")))
int32_t vkGetPhysicalDeviceSurfacePresentModesKHR(void* /*physicalDevice*/, uint64_t /*surface*/, uint32_t* pPresentModeCount, uint32_t* pPresentModes) {
    if (!pPresentModeCount) return -3;
    static const uint32_t kModes[] = {2U /* VK_PRESENT_MODE_FIFO_KHR */, 0U /* VK_PRESENT_MODE_IMMEDIATE_KHR */};
    if (!pPresentModes) {
        *pPresentModeCount = 2U;
        return 0;
    }
    uint32_t n = std::min<uint32_t>(*pPresentModeCount, 2U);
    std::memcpy(pPresentModes, kModes, n * sizeof(uint32_t));
    *pPresentModeCount = n;
    return (n < 2U) ? 5 : 0;
}

__attribute__((visibility("default")))
int32_t vkCreateSwapchainKHR(void* device, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pSwapchain) {
    if (!device || !pCreateInfo || !pSwapchain) return -3;
    auto* dev = static_cast<MaliIcdDevice*>(device);
    struct SwapchainCreateInfo {
        uint32_t sType; const void* pNext; uint32_t flags; uint64_t surface;
        uint32_t minImageCount; uint32_t imageFormat; uint32_t imageColorSpace;
        uint32_t imageExtent[2]; uint32_t imageArrayLayers; uint32_t imageUsage;
    };
    const auto* sci = static_cast<const SwapchainCreateInfo*>(pCreateInfo);
    auto* sc = new MaliIcdSwapchain();
    sc->device = dev;
    uint32_t surf_w = 64U, surf_h = 64U;
    resolveSurfaceInfo(sci->surface, &surf_w, &surf_h, &sc->xcb_conn, &sc->xcb_window, &sc->xcb_depth,
                       &sc->is_wayland, &sc->wl_display, &sc->wl_surface);
    sc->width  = std::max<uint32_t>(16U, sci->imageExtent[0] ? sci->imageExtent[0] : surf_w);
    sc->height = std::max<uint32_t>(16U, sci->imageExtent[1] ? sci->imageExtent[1] : surf_h);
    sc->format = sci->imageFormat ? sci->imageFormat : 37U;
    uint32_t img_count = std::max<uint32_t>(2U, sci->minImageCount);
    uint64_t byte_sz = std::max<uint64_t>(4096ULL, (uint64_t(sc->width) * sc->height * 4ULL + 4095ULL) & ~4095ULL);

    // Allocate WSI swapchain images via /dev/dma_heap/system + /dev/mali0 UMM import (aot_kbase_mem_import)
    int heap_fd = static_cast<int>(bionic_mali_open_stub(reinterpret_cast<uint64_t>("/dev/dma_heap/system"), 2));
    for (uint32_t i = 0; i < img_count; ++i) {
        DmaHeapAllocationData dreq{byte_sz, 0, 2, 0};
        bionic_mali_kbase_ioctl_stub(heap_fd, DMA_HEAP_IOCTL_ALLOC, reinterpret_cast<uint64_t>(&dreq));
        int dmabuf_fd = static_cast<int>(dreq.fd);
        sc->dmabuf_fds.push_back(dmabuf_fd);

        uint64_t umm_flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA;
        uint64_t w_gpu_va = 0;
        if (aot_kbase_mem_import && !dev->kctx_mem.empty()) {
            std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
            auto& cpu = g_aot_rt.cpu;
            uint64_t va_pages = 0;
            uint64_t out_flags = umm_flags;
            cpu.x[0] = reinterpret_cast<uint64_t>(dev->kctx_mem.data());
            cpu.x[1] = 2ULL; // BASE_MEM_IMPORT_TYPE_UMM
            cpu.x[2] = static_cast<uint32_t>(dmabuf_fd);
            cpu.x[3] = reinterpret_cast<uint64_t>(&va_pages);
            cpu.x[4] = reinterpret_cast<uint64_t>(&w_gpu_va);
            cpu.x[5] = reinterpret_cast<uint64_t>(&out_flags);
            aot_kbase_mem_import(&cpu);
        }
        if (w_gpu_va == 0) {
            KbaseIoctlMemImport imp{};
            imp.in.flags = umm_flags;
            imp.in.phandle = reinterpret_cast<uint64_t>(&dmabuf_fd);
            imp.in.type = BASE_MEM_IMPORT_TYPE_UMM;
            bionic_mali_kbase_ioctl_stub(100, 0xc0188016U, reinterpret_cast<uint64_t>(&imp));
            w_gpu_va = bionic_mali_mmap_stub(0, imp.out.va_pages * 4096ULL, PROT_READ | PROT_WRITE, MAP_SHARED, 100, imp.out.gpu_va);
        }

        auto* img = new MaliIcdImage();
        img->width        = sc->width;
        img->height       = sc->height;
        img->format       = sc->format;
        img->byte_size    = byte_sz;
        img->bound_gpu_va = w_gpu_va;
        sc->images.push_back(img);
        {
            std::lock_guard<std::mutex> lk(g_icd_mutex);
            g_images[reinterpret_cast<uint64_t>(img)] = {img->bound_gpu_va, img->width, img->height};
        }
    }
    mali_icd_log("vkCreateSwapchainKHR -> swapchain=%p extent=%ux%u format=%u images=%u xcb_conn=%p xcb_win=0x%x is_wayland=%d wl_display=%p wl_surface=%p",
                 static_cast<void*>(sc), sc->width, sc->height, sc->format,
                 static_cast<unsigned>(sc->images.size()), sc->xcb_conn, sc->xcb_window,
                 sc->is_wayland ? 1 : 0, sc->wl_display, sc->wl_surface);
    *pSwapchain = reinterpret_cast<uint64_t>(sc);
    return 0;
}

__attribute__((visibility("default")))
void vkDestroySwapchainKHR(void* /*device*/, uint64_t swapchain, const void* /*pAllocator*/) {
    auto* sc = reinterpret_cast<MaliIcdSwapchain*>(swapchain);
    if (!sc) return;
    for (auto* img : sc->images) {
        {
            std::lock_guard<std::mutex> lk(g_icd_mutex);
            g_images.erase(reinterpret_cast<uint64_t>(img));
        }
        delete img;
    }
    delete sc;
}

__attribute__((visibility("default")))
int32_t vkGetSwapchainImagesKHR(void* /*device*/, uint64_t swapchain, uint32_t* pSwapchainImageCount, uint64_t* pSwapchainImages) {
    if (!swapchain || !pSwapchainImageCount) return -3;
    auto* sc = reinterpret_cast<MaliIcdSwapchain*>(swapchain);
    uint32_t total = static_cast<uint32_t>(sc->images.size());
    if (!pSwapchainImages) {
        *pSwapchainImageCount = total;
        return 0;
    }
    uint32_t n = std::min(*pSwapchainImageCount, total);
    for (uint32_t i = 0; i < n; ++i) {
        pSwapchainImages[i] = reinterpret_cast<uint64_t>(sc->images[i]);
    }
    *pSwapchainImageCount = n;
    return (n < total) ? 5 : 0;
}

__attribute__((visibility("default")))
int32_t vkAcquireNextImageKHR(void* /*device*/, uint64_t swapchain, uint64_t /*timeout*/, uint64_t /*semaphore*/, uint64_t /*fence*/, uint32_t* pImageIndex) {
    if (!swapchain || !pImageIndex) return -3;
    auto* sc = reinterpret_cast<MaliIcdSwapchain*>(swapchain);
    *pImageIndex = sc->next_image_idx;
    sc->next_image_idx = (sc->next_image_idx + 1U) % static_cast<uint32_t>(sc->images.size());
    return 0;
}

__attribute__((visibility("default")))
int32_t vkAcquireNextImage2KHR(void* device, const void* pAcquireInfo, uint32_t* pImageIndex) {
    if (!pAcquireInfo || !pImageIndex) return -3;
    struct Acq2 { uint32_t sType; const void* pNext; uint64_t swapchain; uint64_t timeout; uint64_t sem; uint64_t fence; uint32_t mask; };
    const auto* a2 = static_cast<const Acq2*>(pAcquireInfo);
    return vkAcquireNextImageKHR(device, a2->swapchain, a2->timeout, a2->sem, a2->fence, pImageIndex);
}

__attribute__((visibility("default")))
int32_t vkQueuePresentKHR(void* /*queue*/, const void* pPresentInfo) {
    if (!pPresentInfo) return -3;
    struct PresInfo {
        uint32_t sType; const void* pNext;
        uint32_t waitSemCount; const uint64_t* pWaitSems;
        uint32_t swapchainCount; const uint64_t* pSwapchains;
        const uint32_t* pImageIndices; int32_t* pResults;
    };
    const auto* pi = static_cast<const PresInfo*>(pPresentInfo);
    static uint32_t s_present_frame_idx = 0;
    const char* dump_ppm = getenv("MALI_VKCUBE_DUMP_PPM");
    const char* dump_dir = getenv("MALI_VKCUBE_DUMP_DIR");
    for (uint32_t i = 0; i < pi->swapchainCount; ++i) {
        auto* sc = reinterpret_cast<MaliIcdSwapchain*>(pi->pSwapchains[i]);
        uint32_t idx = pi->pImageIndices ? pi->pImageIndices[i] : 0U;
        std::string target_ppm;
        if (dump_dir && dump_dir[0]) {
            target_ppm = std::string(dump_dir) + "/vkcube_frame_" + std::to_string(s_present_frame_idx++) + ".ppm";
        } else if (dump_ppm && dump_ppm[0]) {
            target_ppm = dump_ppm;
        }
        if (sc && idx < sc->images.size()) {
            const auto* img = sc->images[idx];
            if (img && img->bound_gpu_va) {
                // 1a. Live Wayland wl_shm window presentation (vkcube --wsi wayland / vkgears -wayland)
                if (sc->is_wayland && sc->wl_display && sc->wl_surface && g_wayland_lib.ensureLoaded()) {
                    if (!sc->wl_buffer) {
                        void* reg = g_wayland_lib.proxy_marshal_constructor(
                            sc->wl_display, 1U /* WL_DISPLAY_GET_REGISTRY */,
                            g_wayland_lib.registry_interface, nullptr);
                        if (reg) {
                            static void (*s_reg_listener[2])(void) = {
                                reinterpret_cast<void (*)(void)>(&MaliWaylandLib::onRegistryGlobal),
                                reinterpret_cast<void (*)(void)>(&MaliWaylandLib::onRegistryGlobalRemove)
                            };
                            g_wayland_lib.proxy_add_listener(reg, s_reg_listener, &sc->wl_shm);
                            g_wayland_lib.display_roundtrip(sc->wl_display);
                            if (g_wayland_lib.proxy_destroy) g_wayland_lib.proxy_destroy(reg);
                        }
                        if (sc->wl_shm) {
                            sc->wl_shm_sz = static_cast<size_t>(img->width) * img->height * 4U;
                            sc->wl_shm_fd = memfd_create("mali_aot_wl_shm", 0);
                            if (sc->wl_shm_fd >= 0 && ftruncate(sc->wl_shm_fd, static_cast<off_t>(sc->wl_shm_sz)) == 0) {
                                void* mp = mmap(nullptr, sc->wl_shm_sz, PROT_READ | PROT_WRITE, MAP_SHARED, sc->wl_shm_fd, 0);
                                if (mp != MAP_FAILED) {
                                    sc->wl_shm_map = static_cast<uint32_t*>(mp);
                                    sc->wl_pool = g_wayland_lib.proxy_marshal_constructor(
                                        sc->wl_shm, 0U /* WL_SHM_CREATE_POOL */,
                                        g_wayland_lib.shm_pool_interface, nullptr,
                                        sc->wl_shm_fd, static_cast<int32_t>(sc->wl_shm_sz));
                                    if (sc->wl_pool) {
                                        sc->wl_buffer = g_wayland_lib.proxy_marshal_constructor(
                                            sc->wl_pool, 0U /* WL_SHM_POOL_CREATE_BUFFER */,
                                            g_wayland_lib.buffer_interface, nullptr,
                                            0, static_cast<int32_t>(img->width), static_cast<int32_t>(img->height),
                                            static_cast<int32_t>(img->width * 4U), 1U /* WL_SHM_FORMAT_XRGB8888 */);
                                    }
                                }
                            }
                        }
                    }
                    if (sc->wl_buffer && sc->wl_shm_map) {
                        const uint32_t* src_px = reinterpret_cast<const uint32_t*>(img->bound_gpu_va);
                        uint32_t total_px = img->width * img->height;
                        // Internal /dev/mali0 MFBD tile writeback is canonical RGBA8 (byte0=R, byte1=G, byte2=B, byte3=A);
                        // convert to WL_SHM_FORMAT_XRGB8888 ((0xFF << 24) | (R << 16) | (G << 8) | B).
                        for (uint32_t p = 0; p < total_px; ++p) {
                            uint32_t v = src_px[p];
                            uint32_t r = v & 0xFFU;
                            uint32_t g = (v >> 8) & 0xFFU;
                            uint32_t b = (v >> 16) & 0xFFU;
                            sc->wl_shm_map[p] = (0xFFU << 24) | (r << 16) | (g << 8) | b;
                        }
                        g_wayland_lib.proxy_marshal(sc->wl_surface, 1U /* WL_SURFACE_ATTACH */, sc->wl_buffer, 0, 0);
                        g_wayland_lib.proxy_marshal(sc->wl_surface, 2U /* WL_SURFACE_DAMAGE */, 0, 0,
                                                    static_cast<int32_t>(img->width), static_cast<int32_t>(img->height));
                        g_wayland_lib.proxy_marshal(sc->wl_surface, 6U /* WL_SURFACE_COMMIT */);
                        g_wayland_lib.display_flush(sc->wl_display);
                    }
                }

                // 1b. Live XCB window presentation (vkcube --wsi xcb / vkgears)
                if (sc->xcb_conn && sc->xcb_window && g_xcb_lib.ensureLoaded()) {
                    if (sc->xcb_gc == 0) {
                        sc->xcb_gc = g_xcb_lib.generate_id(sc->xcb_conn);
                        g_xcb_lib.create_gc(sc->xcb_conn, sc->xcb_gc, sc->xcb_window, 0, nullptr);
                    }
                    const uint32_t* src_px = reinterpret_cast<const uint32_t*>(img->bound_gpu_va);
                    std::vector<uint32_t> bgra_px(img->width * img->height);
                    for (uint32_t p = 0; p < img->width * img->height; ++p) {
                        uint32_t v = src_px[p];
                        uint32_t r = v & 0xFFU;
                        uint32_t g = (v >> 8) & 0xFFU;
                        uint32_t b = (v >> 16) & 0xFFU;
                        uint32_t a = (v >> 24) & 0xFFU;
                        bgra_px[p] = (a << 24) | (r << 16) | (g << 8) | b;
                    }
                    const uint32_t* x11_px = bgra_px.data();
                    // Send in horizontal bands so large windows (e.g. 500x500 = 1 MB) never exceed X11 max request size (256 KiB)
                    uint32_t rows_per_band = std::max<uint32_t>(1U, (65536U) / (img->width * 4U));
                    for (uint32_t y = 0; y < img->height; y += rows_per_band) {
                        uint32_t band_h = std::min<uint32_t>(rows_per_band, img->height - y);
                        g_xcb_lib.put_image(sc->xcb_conn, 2 /* XCB_IMAGE_FORMAT_Z_PIXMAP */,
                                            sc->xcb_window, sc->xcb_gc,
                                            static_cast<uint16_t>(img->width), static_cast<uint16_t>(band_h),
                                            0, static_cast<int16_t>(y), 0, sc->xcb_depth ? sc->xcb_depth : 24,
                                            img->width * band_h * 4U,
                                            reinterpret_cast<const uint8_t*>(x11_px + y * img->width));
                    }
                    g_xcb_lib.flush(sc->xcb_conn);
                }

                // 2. Optional PPM frame dump
                if (!target_ppm.empty()) {
                    std::ofstream out(target_ppm, std::ios::binary);
                    if (out) {
                        out << "P6\n" << img->width << " " << img->height << "\n255\n";
                        const uint32_t* px = reinterpret_cast<const uint32_t*>(img->bound_gpu_va);
                        for (uint32_t p = 0; p < img->width * img->height; ++p) {
                            uint8_t rgb[3] = {
                                static_cast<uint8_t>(px[p] & 0xFFU),
                                static_cast<uint8_t>((px[p] >> 8) & 0xFFU),
                                static_cast<uint8_t>((px[p] >> 16) & 0xFFU)
                            };
                            out.write(reinterpret_cast<const char*>(rgb), 3);
                        }
                    }
                }
            }
        }
        if (pi->pResults) pi->pResults[i] = 0;
    }
    static uint64_t s_total_presents = 0;
    ++s_total_presents;
    if (s_total_presents <= 5 || (s_total_presents % 100) == 0) {
        mali_icd_log("vkQueuePresentKHR frame=%lu swapchains=%u",
                     static_cast<unsigned long>(s_total_presents), pi->swapchainCount);
    }
    return 0;
}

__attribute__((visibility("default")))
int32_t vkQueueWaitIdle(void* /*queue*/) { return 0; }

__attribute__((visibility("default")))
int32_t vkDeviceWaitIdle(void* /*device*/) { return 0; }


__attribute__((visibility("default")))
void vkCmdClearAttachments(void* commandBuffer, uint32_t attachmentCount, const void* pAttachments, uint32_t /*rectCount*/, const void* /*pRects*/) {
    if (!commandBuffer || attachmentCount == 0 || !pAttachments) return;
    auto* cb = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    struct ClearAtt { uint32_t aspectMask; uint32_t colorAttachment; float clearVal[4]; };
    const auto* atts = static_cast<const ClearAtt*>(pAttachments);
    for (uint32_t i = 0; i < attachmentCount; ++i) {
        if (atts[i].aspectMask & 1U /* VK_IMAGE_ASPECT_COLOR_BIT */) {
            uint32_t r8 = static_cast<uint32_t>(std::clamp(atts[i].clearVal[0], 0.0f, 1.0f) * 255.0f + 0.5f);
            uint32_t g8 = static_cast<uint32_t>(std::clamp(atts[i].clearVal[1], 0.0f, 1.0f) * 255.0f + 0.5f);
            uint32_t b8 = static_cast<uint32_t>(std::clamp(atts[i].clearVal[2], 0.0f, 1.0f) * 255.0f + 0.5f);
            uint32_t a8 = static_cast<uint32_t>(std::clamp(atts[i].clearVal[3], 0.0f, 1.0f) * 255.0f + 0.5f);
            MaliIcdRecordedCmd cmd{};
            cmd.type        = MaliIcdRecordedCmd::CMD_CLEAR_ATTACHMENTS;
            cmd.framebuffer = cb->active_fb;
            cmd.clear_rgba8 = (a8 << 24) | (b8 << 16) | (g8 << 8) | r8;
            cb->cmds.push_back(cmd);
        }
    }
}
__attribute__((visibility("default")))
void vkCmdClearColorImage(void* commandBuffer, uint64_t image, uint32_t /*imageLayout*/, const void* pColor, uint32_t /*rangeCount*/, const void* /*pRanges*/) {
    if (!commandBuffer || !image || !pColor) return;
    auto* cb = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    const float* cf = static_cast<const float*>(pColor);
    uint32_t r8 = static_cast<uint32_t>(std::clamp(cf[0], 0.0f, 1.0f) * 255.0f + 0.5f);
    uint32_t g8 = static_cast<uint32_t>(std::clamp(cf[1], 0.0f, 1.0f) * 255.0f + 0.5f);
    uint32_t b8 = static_cast<uint32_t>(std::clamp(cf[2], 0.0f, 1.0f) * 255.0f + 0.5f);
    uint32_t a8 = static_cast<uint32_t>(std::clamp(cf[3], 0.0f, 1.0f) * 255.0f + 0.5f);
    MaliIcdRecordedCmd cmd{};
    cmd.type        = MaliIcdRecordedCmd::CMD_CLEAR_COLOR_IMAGE;
    cmd.dst_image   = reinterpret_cast<MaliIcdImage*>(image);
    cmd.clear_rgba8 = (a8 << 24) | (b8 << 16) | (g8 << 8) | r8;
    cb->cmds.push_back(cmd);
}
__attribute__((visibility("default")))
void vkCmdClearDepthStencilImage(void* /*commandBuffer*/, uint64_t /*image*/, uint32_t /*imageLayout*/, const void* /*pDepthStencil*/, uint32_t /*rangeCount*/, const void* /*pRanges*/) {}
__attribute__((visibility("default")))
void vkCmdExecuteCommands(void* commandBuffer, uint32_t commandBufferCount, void* const* pCommandBuffers) {
    if (!commandBuffer || commandBufferCount == 0 || !pCommandBuffers) return;
    auto* primary = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    for (uint32_t i = 0; i < commandBufferCount; ++i) {
        const auto* sec = static_cast<const MaliIcdCommandBuffer*>(pCommandBuffers[i]);
        if (!sec) continue;
        for (auto cmd : sec->cmds) {
            if (!cmd.framebuffer) cmd.framebuffer = primary->active_fb;
            primary->cmds.push_back(cmd);
        }
    }
}
__attribute__((visibility("default")))
void vkCmdNextSubpass(void* /*commandBuffer*/, uint32_t /*contents*/) {}
__attribute__((visibility("default")))
int32_t vkCreateEvent(void* /*device*/, const void* /*pCreateInfo*/, const void* /*pAllocator*/, uint64_t* pEvent) {
    if (!pEvent) return -3;
    *pEvent = reinterpret_cast<uint64_t>(new MaliIcdEvent());
    return 0;
}
__attribute__((visibility("default")))
void vkDestroyEvent(void* /*device*/, uint64_t event, const void* /*pAllocator*/) {
    if (event && event != 0xE7000001ULL) delete reinterpret_cast<MaliIcdEvent*>(event);
}
__attribute__((visibility("default")))
int32_t vkGetEventStatus(void* /*device*/, uint64_t event) {
    if (event && event != 0xE7000001ULL)
        return reinterpret_cast<MaliIcdEvent*>(event)->signaled ? 3 /* VK_EVENT_SET */ : 4 /* VK_EVENT_RESET */;
    return 0;
}
__attribute__((visibility("default")))
int32_t vkSetEvent(void* /*device*/, uint64_t event) {
    if (event && event != 0xE7000001ULL) reinterpret_cast<MaliIcdEvent*>(event)->signaled = true;
    return 0;
}
__attribute__((visibility("default")))
int32_t vkResetEvent(void* /*device*/, uint64_t event) {
    if (event && event != 0xE7000001ULL) reinterpret_cast<MaliIcdEvent*>(event)->signaled = false;
    return 0;
}
__attribute__((visibility("default")))
void vkCmdSetEvent(void* /*commandBuffer*/, uint64_t /*event*/, uint32_t /*stageMask*/) {}
__attribute__((visibility("default")))
void vkCmdResetEvent(void* /*commandBuffer*/, uint64_t /*event*/, uint32_t /*stageMask*/) {}
__attribute__((visibility("default")))
void vkCmdWaitEvents(void* /*commandBuffer*/, uint32_t /*eventCount*/, const uint64_t* /*pEvents*/, uint32_t /*srcStageMask*/, uint32_t /*dstStageMask*/, uint32_t /*memoryBarrierCount*/, const void* /*pMemoryBarriers*/, uint32_t /*bufferMemoryBarrierCount*/, const void* /*pBufferMemoryBarriers*/, uint32_t /*imageMemoryBarrierCount*/, const void* /*pImageMemoryBarriers*/) {}
__attribute__((visibility("default")))
int32_t vkGetPhysicalDeviceSurfaceCapabilities2KHR(void* physicalDevice, const void* pSurfaceInfo, void* pSurfaceCapabilities) {
    if (!pSurfaceInfo || !pSurfaceCapabilities) return -3;
    uint64_t surf = *reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(pSurfaceInfo) + 16);
    return vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surf, static_cast<uint8_t*>(pSurfaceCapabilities) + 16);
}
__attribute__((visibility("default")))
int32_t vkGetPhysicalDeviceSurfaceFormats2KHR(void* physicalDevice, const void* pSurfaceInfo, uint32_t* pSurfaceFormatCount, void* pSurfaceFormats) {
    if (!pSurfaceInfo || !pSurfaceFormatCount) return -3;
    uint64_t surf = *reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(pSurfaceInfo) + 16);
    if (!pSurfaceFormats)
        return vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surf, pSurfaceFormatCount, nullptr);
    struct Fmt2 { uint32_t sType; void* pNext; uint32_t format; uint32_t colorSpace; };
    auto* out = static_cast<Fmt2*>(pSurfaceFormats);
    uint32_t n = std::min<uint32_t>(*pSurfaceFormatCount, 4U);
    if (n >= 1) { out[0].format = 37U; out[0].colorSpace = 0U; }
    if (n >= 2) { out[1].format = 44U; out[1].colorSpace = 0U; }
    if (n >= 3) { out[2].format = 50U; out[2].colorSpace = 0U; }
    if (n >= 4) { out[3].format = 43U; out[3].colorSpace = 0U; }
    *pSurfaceFormatCount = n;
    return (n < 4U) ? 5 : 0;
}


} // extern "C"
