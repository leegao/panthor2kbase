// ============================================================================
// src/aot/src/mali_icd_wsi.cpp
// Desktop Linux WSI Bridge (VK_KHR_xcb_surface, VK_KHR_xlib_surface,
// VK_KHR_wayland_surface, VK_KHR_display, VK_EXT_headless_surface,
// VK_KHR_swapchain) over /dev/mali0 GPU backbuffers.
// ============================================================================
#include "mali_icd_internal.h"
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

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
    typedef struct { unsigned int sequence; } xcb_shm_query_version_cookie_t;
    struct xcb_shm_query_version_reply_t {
        uint8_t  response_type;
        uint8_t  shared_pixmaps;
        uint16_t sequence;
        uint32_t length;
        uint16_t major_version;
        uint16_t minor_version;
        uint16_t uid;
        uint16_t gid;
        uint8_t  pixmap_format;
        uint8_t  pad0[15];
    };
    typedef xcb_shm_query_version_cookie_t (*pfn_xcb_shm_query_version)(void* c);
    typedef xcb_shm_query_version_reply_t* (*pfn_xcb_shm_query_version_reply)(void* c, xcb_shm_query_version_cookie_t cookie, void** e);
    typedef xcb_void_cookie_t (*pfn_xcb_shm_attach)(void* c, uint32_t shmseg, uint32_t shmid, uint8_t read_only);
    typedef xcb_void_cookie_t (*pfn_xcb_shm_detach)(void* c, uint32_t shmseg);
    typedef xcb_void_cookie_t (*pfn_xcb_shm_put_image)(void* c, uint32_t drawable, uint32_t gc,
                                                       uint16_t total_width, uint16_t total_height,
                                                       uint16_t src_x, uint16_t src_y,
                                                       uint16_t src_width, uint16_t src_height,
                                                       int16_t dst_x, int16_t dst_y,
                                                       uint8_t depth, uint8_t format, uint8_t send_event,
                                                       uint32_t shmseg, uint32_t offset);

    pfn_xcb_generate_id        generate_id = nullptr;
    pfn_xcb_get_geometry       get_geometry = nullptr;
    pfn_xcb_get_geometry_reply get_geometry_reply = nullptr;
    pfn_xcb_create_gc          create_gc = nullptr;
    pfn_xcb_free_gc            free_gc = nullptr;
    pfn_xcb_put_image          put_image = nullptr;
    pfn_xcb_flush              flush = nullptr;

    void*                           shm_handle = nullptr;
    bool                            shm_probed = false;
    pfn_xcb_shm_query_version       shm_query_version = nullptr;
    pfn_xcb_shm_query_version_reply shm_query_version_reply = nullptr;
    pfn_xcb_shm_attach              shm_attach = nullptr;
    pfn_xcb_shm_detach              shm_detach = nullptr;
    pfn_xcb_shm_put_image           shm_put_image = nullptr;

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

    bool ensureShmLoaded() {
        if (shm_probed) return shm_query_version && shm_query_version_reply && shm_attach && shm_detach && shm_put_image;
        shm_probed = true;
        if (!ensureLoaded()) return false;
        shm_handle = dlopen("libxcb-shm.so.0", RTLD_LAZY | RTLD_LOCAL);
        if (!shm_handle) shm_handle = dlopen("libxcb-shm.so", RTLD_LAZY | RTLD_LOCAL);
        if (!shm_handle) return false;
        shm_query_version       = reinterpret_cast<pfn_xcb_shm_query_version>(dlsym(shm_handle, "xcb_shm_query_version"));
        shm_query_version_reply = reinterpret_cast<pfn_xcb_shm_query_version_reply>(dlsym(shm_handle, "xcb_shm_query_version_reply"));
        shm_attach              = reinterpret_cast<pfn_xcb_shm_attach>(dlsym(shm_handle, "xcb_shm_attach"));
        shm_detach              = reinterpret_cast<pfn_xcb_shm_detach>(dlsym(shm_handle, "xcb_shm_detach"));
        shm_put_image           = reinterpret_cast<pfn_xcb_shm_put_image>(dlsym(shm_handle, "xcb_shm_put_image"));
        return shm_query_version && shm_query_version_reply && shm_attach && shm_detach && shm_put_image;
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
    uint32_t                   xcb_shm_seg = 0;
    int                        xcb_shmid   = -1;
    uint8_t*                   xcb_shm_addr = nullptr;
    size_t                     xcb_shm_sz   = 0;
    bool                       xcb_shm_checked = false;
    bool                       xcb_shm_ok      = false;
    bool                       is_wayland = false;
    void*                      wl_display = nullptr;
    void*                      wl_surface = nullptr;
    void*                      wl_shm     = nullptr;
    void*                      wl_pool    = nullptr;
    void*                      wl_buffer  = nullptr;
    int                        wl_shm_fd  = -1;
    uint32_t*                  wl_shm_map = nullptr;
    size_t                     wl_shm_sz  = 0;
    std::vector<void*>         wl_pools;
    std::vector<void*>         wl_buffers;
    std::vector<int>           wl_shm_fds;
    std::vector<uint32_t*>     wl_shm_maps;
    std::vector<size_t>        wl_shm_szs;
    std::vector<MaliIcdImage*> images;
    std::vector<int>           dmabuf_fds;
    std::vector<bool>          image_acquired;
    std::vector<uint64_t>      image_fences;
};


// ============================================================================
// VK_KHR_display & VK_EXT_debug_utils Support for Official Khronos vkcube (--wsi display)
// ============================================================================
__attribute__((visibility("default")))
int32_t vkGetPhysicalDeviceDisplayPropertiesKHR(void* /*physicalDevice*/, uint32_t* pPropertyCount, void* pProperties) {
    if (!pPropertyCount) return -3;
    if (!pProperties) { *pPropertyCount = 1U; return 0; }
    if (*pPropertyCount == 0U) return 5;
    auto* p = static_cast<VkDisplayPropertiesKHR*>(pProperties);
    p->display = reinterpret_cast<VkDisplayKHR>(0xD1590001ULL);
    p->displayName = "Mali-G615 Virtual Kbase Display (500x500)";
    p->physicalDimensions.width = 500U; p->physicalDimensions.height = 500U;
    uint32_t disp_res = (getenv("MALI_VKCUBE_HIRES") != nullptr) ? 500U : 64U;
    p->physicalResolution.width = disp_res;  p->physicalResolution.height = disp_res;
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
    auto* p = static_cast<VkDisplayPlanePropertiesKHR*>(pProperties);
    p->currentDisplay = reinterpret_cast<VkDisplayKHR>(0xD1590001ULL);
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
    auto* p = static_cast<VkDisplayModePropertiesKHR*>(pProperties);
    p->displayMode = reinterpret_cast<VkDisplayModeKHR>(0xD15900DEULL);
    uint32_t disp_res = (getenv("MALI_VKCUBE_HIRES") != nullptr) ? 500U : 64U;
    p->parameters.visibleRegion.width = disp_res;
    p->parameters.visibleRegion.height = disp_res;
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
    auto* c = static_cast<VkDisplayPlaneCapabilitiesKHR*>(pCapabilities);
    std::memset(c, 0, sizeof(*c));
    c->supportedAlpha = 1U; // VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR
    c->minSrcExtent.width = 16U; c->minSrcExtent.height = 16U;
    c->maxSrcExtent.width = 4096U; c->maxSrcExtent.height = 4096U;
    c->minDstExtent.width = 16U; c->minDstExtent.height = 16U;
    c->maxDstExtent.width = 4096U; c->maxDstExtent.height = 4096U;
    return 0;
}

__attribute__((visibility("default")))
int32_t vkCreateDebugUtilsMessengerEXT(void* instance, const void* pCreateInfo, const void* pAllocator, uint64_t* pMessenger) {
    if (!pMessenger) return -3;
    auto* inst = icdGetInstance(instance);
    if (inst && inst->native_instance && !g_aot_rt.mapper.so_path_loaded.empty() && aot_vkCreateDebugUtilsMessengerEXT && pCreateInfo) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        g_host_vk_allocators.registerCallbacks(pAllocator);
        uint64_t aot_msg = 0;
        int32_t r = mali_aapcs64::invokeLiftedAapcs64<int32_t>(
            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkCreateDebugUtilsMessengerEXT,
            inst->native_instance, pCreateInfo, pAllocator, &aot_msg);
        g_aot_rt.lifted_vk_calls_executed++;
        if (r == 0 && aot_msg != 0) { *pMessenger = aot_msg; return 0; }
    }
    *pMessenger = 0xDEB06001ULL;
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyDebugUtilsMessengerEXT(void* instance, uint64_t messenger, const void* pAllocator) {
    if (messenger != 0 && messenger != 0xDEB06001ULL && !g_aot_rt.mapper.so_path_loaded.empty() && aot_vkDestroyDebugUtilsMessengerEXT) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        mali_aapcs64::invokeLiftedAapcs64<void>(
            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkDestroyDebugUtilsMessengerEXT,
            instance, messenger, pAllocator);
        g_aot_rt.lifted_vk_calls_executed++;
    }
}

__attribute__((visibility("default")))
int32_t vkSetDebugUtilsObjectNameEXT(void* device, const void* pNameInfo) {
    auto* dev = static_cast<MaliIcdDevice*>(device);
    if (dev && !dev->aot_vk_device_mem.empty() && aot_vkSetDebugUtilsObjectNameEXT && pNameInfo) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        int32_t r = mali_aapcs64::invokeLiftedAapcs64<int32_t>(
            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkSetDebugUtilsObjectNameEXT,
            dev->aot_vk_device_mem.data(), pNameInfo);
        g_aot_rt.lifted_vk_calls_executed++;
        return r;
    }
    return 0;
}

__attribute__((visibility("default")))
void vkCmdBeginDebugUtilsLabelEXT(void* commandBuffer, const void* pLabelInfo) {
    auto* cb = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    if (cb && cb->aot_cmdbuf_mem.size() >= 256 && *reinterpret_cast<const uint64_t*>(cb->aot_cmdbuf_mem.data() + 0x68) != 0 &&
        aot_vkCmdBeginDebugUtilsLabelEXT && pLabelInfo) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        mali_aapcs64::invokeLiftedAapcs64<void>(
            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkCmdBeginDebugUtilsLabelEXT,
            cb->aot_cmdbuf_mem.data(), pLabelInfo);
        g_aot_rt.lifted_vk_calls_executed++;
    }
}

__attribute__((visibility("default")))
void vkCmdEndDebugUtilsLabelEXT(void* commandBuffer) {
    auto* cb = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    if (cb && cb->aot_cmdbuf_mem.size() >= 256 && *reinterpret_cast<const uint64_t*>(cb->aot_cmdbuf_mem.data() + 0x68) != 0 &&
        aot_vkCmdEndDebugUtilsLabelEXT) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        mali_aapcs64::invokeLiftedAapcs64<void>(
            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkCmdEndDebugUtilsLabelEXT,
            cb->aot_cmdbuf_mem.data());
        g_aot_rt.lifted_vk_calls_executed++;
    }
}

__attribute__((visibility("default")))
void vkCmdInsertDebugUtilsLabelEXT(void* commandBuffer, const void* pLabelInfo) {
    auto* cb = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    if (cb && cb->aot_cmdbuf_mem.size() >= 256 && *reinterpret_cast<const uint64_t*>(cb->aot_cmdbuf_mem.data() + 0x68) != 0 &&
        aot_vkCmdInsertDebugUtilsLabelEXT && pLabelInfo) {
        std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
        mali_aapcs64::invokeLiftedAapcs64<void>(
            g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkCmdInsertDebugUtilsLabelEXT,
            cb->aot_cmdbuf_mem.data(), pLabelInfo);
        g_aot_rt.lifted_vk_calls_executed++;
    }
}


__attribute__((visibility("default")))
int32_t vkCreatePipelineCache(void* device, const void* pCreateInfo, const void* /*pAllocator*/, uint64_t* pPipelineCache) {
    if (!pPipelineCache) return -3;
    auto* dev = static_cast<MaliIcdDevice*>(device);
    if (dev && aot_vkCreatePipelineCache && !dev->aot_vk_device_mem.empty()) {
        struct CleanPcCI {
            uint32_t sType; const void* pNext; uint32_t flags;
            size_t initialDataSize; const void* pInitialData;
        } clean_ci = {17U, nullptr, 0U, 0, nullptr};
        if (pCreateInfo) {
            const auto* src_ci = static_cast<const CleanPcCI*>(pCreateInfo);
            clean_ci.flags = src_ci->flags;
            clean_ci.initialDataSize = src_ci->initialDataSize;
            clean_ci.pInitialData = src_ci->pInitialData;
        }
        uint64_t aot_pc = 0;
        auto& tctx = maliGetThreadGuestContext();
        GuestStackFrameGuard frame(tctx);
        const size_t frame_stack_sz = static_cast<size_t>(tctx.cpu.sp - reinterpret_cast<uintptr_t>(tctx.stack_mem));
        int32_t res = mali_aapcs64::invokeLiftedAapcs64<int32_t>(
            tctx.cpu, tctx.stack_mem, frame_stack_sz, aot_vkCreatePipelineCache,
            dev->aot_vk_device_mem.data(), &clean_ci, uint64_t(0), &aot_pc);
        g_aot_rt.lifted_vk_calls_executed++;
        if (res == 0 && aot_pc != 0) {
            *pPipelineCache = aot_pc;
            return 0;
        }
    }
    auto* pc = new MaliIcdPipelineCache();
    if (pCreateInfo) {
        const auto* ci = static_cast<const VkPipelineCacheCreateInfo*>(pCreateInfo);
        if (ci->initialDataSize > 0 && ci->pInitialData) {
            const auto* raw = static_cast<const uint8_t*>(ci->pInitialData);
            pc->payload.assign(raw, raw + ci->initialDataSize);
        }
    }
    if (pc->payload.size() < 32) {
        pc->payload.assign(32, 0);
        uint32_t hdr[4] = {32U, 1U /* VK_PIPELINE_CACHE_HEADER_VERSION_ONE */, 0x13B5U, 0xb8031030U};
        std::memcpy(pc->payload.data(), hdr, sizeof(hdr));
        static const uint8_t kUuid[16] = {0x52,0x35,0x34,0x50,0x33,0x4D,0x41,0x4C,0x49,0x47,0x36,0x31,0x35,0x00,0x00,0x01};
        std::memcpy(pc->payload.data() + 16, kUuid, 16);
    }
    *pPipelineCache = reinterpret_cast<uint64_t>(pc);
    return 0;
}

__attribute__((visibility("default")))
void vkDestroyPipelineCache(void* device, uint64_t pipelineCache, const void* /*pAllocator*/) {
    if (!pipelineCache || pipelineCache == 0xCA000001ULL) return;
    auto* dev = static_cast<MaliIcdDevice*>(device);
    if (dev && aot_vkDestroyPipelineCache && !dev->aot_vk_device_mem.empty()) {
        auto& tctx = maliGetThreadGuestContext();
        GuestStackFrameGuard frame(tctx);
        const size_t frame_stack_sz = static_cast<size_t>(tctx.cpu.sp - reinterpret_cast<uintptr_t>(tctx.stack_mem));
        mali_aapcs64::invokeLiftedAapcs64<void>(
            tctx.cpu, tctx.stack_mem, frame_stack_sz, aot_vkDestroyPipelineCache,
            dev->aot_vk_device_mem.data(), pipelineCache, uint64_t(0));
        g_aot_rt.lifted_vk_calls_executed++;
        return;
    }
    auto* pc = reinterpret_cast<MaliIcdPipelineCache*>(pipelineCache);
    if (pc->magic == 0x50434143484531ULL) delete pc;
}

__attribute__((visibility("default")))
int32_t vkGetPipelineCacheData(void* device, uint64_t pipelineCache, size_t* pDataSize, void* pData) {
    if (!pDataSize) return -3;
    auto* dev = static_cast<MaliIcdDevice*>(device);
    if (dev && pipelineCache && pipelineCache != 0xCA000001ULL &&
        aot_vkGetPipelineCacheData && !dev->aot_vk_device_mem.empty()) {
        auto& tctx = maliGetThreadGuestContext();
        GuestStackFrameGuard frame(tctx);
        const size_t frame_stack_sz = static_cast<size_t>(tctx.cpu.sp - reinterpret_cast<uintptr_t>(tctx.stack_mem));
        int32_t res = mali_aapcs64::invokeLiftedAapcs64<int32_t>(
            tctx.cpu, tctx.stack_mem, frame_stack_sz, aot_vkGetPipelineCacheData,
            dev->aot_vk_device_mem.data(), pipelineCache, pDataSize, pData);
        g_aot_rt.lifted_vk_calls_executed++;
        return res;
    }
    if (!pipelineCache || pipelineCache == 0xCA000001ULL) {
        *pDataSize = 0;
        return 0;
    }
    const auto* pc = reinterpret_cast<const MaliIcdPipelineCache*>(pipelineCache);
    if (pc->magic != 0x50434143484531ULL) {
        *pDataSize = 0;
        return 0;
    }
    if (!pData) {
        *pDataSize = pc->payload.size();
        return 0;
    }
    size_t to_copy = std::min(*pDataSize, pc->payload.size());
    if (to_copy > 0) std::memcpy(pData, pc->payload.data(), to_copy);
    *pDataSize = to_copy;
    return (to_copy < pc->payload.size()) ? 5 /* VK_INCOMPLETE */ : 0;
}

__attribute__((visibility("default")))
int32_t vkMergePipelineCaches(void* device, uint64_t dstCache, uint32_t srcCacheCount, const uint64_t* pSrcCaches) {
    auto* dev = static_cast<MaliIcdDevice*>(device);
    if (dev && dstCache && dstCache != 0xCA000001ULL &&
        aot_vkMergePipelineCaches && !dev->aot_vk_device_mem.empty()) {
        auto& tctx = maliGetThreadGuestContext();
        GuestStackFrameGuard frame(tctx);
        const size_t frame_stack_sz = static_cast<size_t>(tctx.cpu.sp - reinterpret_cast<uintptr_t>(tctx.stack_mem));
        int32_t res = mali_aapcs64::invokeLiftedAapcs64<int32_t>(
            tctx.cpu, tctx.stack_mem, frame_stack_sz, aot_vkMergePipelineCaches,
            dev->aot_vk_device_mem.data(), dstCache, srcCacheCount, pSrcCaches);
        g_aot_rt.lifted_vk_calls_executed++;
        return res;
    }
    auto* dst = reinterpret_cast<MaliIcdPipelineCache*>(dstCache);
    if (dst && dst->magic == 0x50434143484531ULL && pSrcCaches) {
        for (uint32_t i = 0; i < srcCacheCount; ++i) {
            const auto* src = reinterpret_cast<const MaliIcdPipelineCache*>(pSrcCaches[i]);
            if (src && src != dst && src->magic == 0x50434143484531ULL && src->payload.size() > 32) {
                dst->payload.insert(dst->payload.end(), src->payload.begin() + 32, src->payload.end());
            }
        }
    }
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
    if (img && img->row_pitch > 0) {
        pitch = img->row_pitch;
        total_sz = pitch * h;
    }
    l->offset     = 0;
    l->rowPitch   = pitch;
    l->size       = total_sz;
    l->arrayPitch = total_sz;
    l->depthPitch = total_sz;
}

__attribute__((visibility("default")))
int32_t vkFlushMappedMemoryRanges(void* device, uint32_t memoryRangeCount, const void* pMemoryRanges) {
    auto* dev = static_cast<MaliIcdDevice*>(device);
    if (dev && !dev->aot_vk_device_mem.empty() && aot_vkFlushMappedMemoryRanges && memoryRangeCount > 0 && pMemoryRanges) {
        // Only delegate if memory handles are real lifted objects (not host MaliIcdDeviceMemory*)
        return 0;
    }
    return 0;
}

__attribute__((visibility("default")))
int32_t vkInvalidateMappedMemoryRanges(void* device, uint32_t memoryRangeCount, const void* pMemoryRanges) {
    auto* dev = static_cast<MaliIcdDevice*>(device);
    if (dev && !dev->aot_vk_device_mem.empty() && aot_vkInvalidateMappedMemoryRanges && memoryRangeCount > 0 && pMemoryRanges) {
        return 0;
    }
    return 0;
}

__attribute__((visibility("default")))
int32_t vkGetFenceStatus(void* device, uint64_t fence) {
    if (mali_kbase_emu_wait_csf_idle) mali_kbase_emu_wait_csf_idle(200000000ULL);
    auto* dev = static_cast<MaliIcdDevice*>(device);
    if (dev && dev->aot_cctx_mem.size() > 0xc1 && dev->aot_cctx_mem[0xc1] != 0) {
        return -4; /* VK_ERROR_DEVICE_LOST (checked at 0x00964fd0 in libGLES_mali.so) */
    }
    if (fence) {
        auto* f = reinterpret_cast<MaliIcdFence*>(fence);
        if (dev && aot_vkGetFenceStatus && !dev->aot_vk_device_mem.empty() && f->aot_fence_mem.size() > 593) {
            f->aot_fence_mem[593] = f->signaled ? 1U : 0U;
            std::lock_guard<std::mutex> lock(g_aot_rt.mtx);
            int32_t res = mali_aapcs64::invokeLiftedAapcs64<int32_t>(
                g_aot_rt.cpu, g_aot_rt.stack_mem, g_aot_rt.stack_size, aot_vkGetFenceStatus,
                dev->aot_vk_device_mem.data(), f->aot_fence_mem.data());
            g_aot_rt.lifted_vk_calls_executed++;
            return res;
        }
        return f->signaled ? 0 : 1 /* VK_NOT_READY */;
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
    static const uint32_t kModes[] = {
        2U /* VK_PRESENT_MODE_FIFO_KHR */,
        1U /* VK_PRESENT_MODE_MAILBOX_KHR */,
        0U /* VK_PRESENT_MODE_IMMEDIATE_KHR */
    };
    constexpr uint32_t kNumModes = 3U;
    if (!pPresentModes) {
        *pPresentModeCount = kNumModes;
        return 0;
    }
    uint32_t n = std::min<uint32_t>(*pPresentModeCount, kNumModes);
    std::memcpy(pPresentModes, kModes, n * sizeof(uint32_t));
    *pPresentModeCount = n;
    return (n < kNumModes) ? 5 : 0;
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
            auto& tctx = maliGetThreadGuestContext();
            GuestStackFrameGuard frame(tctx);
            auto& cpu = tctx.cpu;
            uint64_t va_pages = 0;
            // Exact r54p3 base_mem_import_padded (0x01c6a260) AAPCS64 signature:
            //   x0 = kctx, w1 = type (BASE_MEM_IMPORT_TYPE_UMM = 2), x2 = &dmabuf_fd (phandle),
            //   x3 = in_flags, w4 = padding (0), x5 = &va_pages -> returns (gpu_va | (out_flags & 0xFFF)) in x0
            cpu.x[0] = reinterpret_cast<uint64_t>(dev->kctx_mem.data());
            cpu.x[1] = 2ULL; // BASE_MEM_IMPORT_TYPE_UMM
            cpu.x[2] = reinterpret_cast<uint64_t>(&dmabuf_fd);
            cpu.x[3] = umm_flags;
            cpu.x[4] = 0ULL;
            cpu.x[5] = reinterpret_cast<uint64_t>(&va_pages);
            aot_kbase_mem_import(&cpu);
        }
        {
            int32_t mfd = (dev && dev->mali_fd >= 0) ? dev->mali_fd : 100;
            KbaseIoctlMemImport imp{};
            imp.in.flags = umm_flags;
            imp.in.phandle = reinterpret_cast<uint64_t>(&dmabuf_fd);
            imp.in.type = BASE_MEM_IMPORT_TYPE_UMM;
            bionic_mali_kbase_ioctl_stub(static_cast<uint64_t>(mfd), 0xc0188016U, reinterpret_cast<uint64_t>(&imp));
            uint64_t req_pages = std::max<uint64_t>(imp.out.va_pages, (byte_sz + 4095ULL) / 4096ULL);
            w_gpu_va = bionic_mali_mmap_stub(0, req_pages * 4096ULL, PROT_READ | PROT_WRITE, MAP_SHARED, static_cast<uint64_t>(mfd), imp.out.gpu_va);
        }

        auto* img = new MaliIcdImage();
        img->width        = sc->width;
        img->height       = sc->height;
        img->format       = sc->format;
        img->row_pitch    = sc->width * 4U;
        img->dmabuf_fd    = dmabuf_fd;
        img->byte_size    = byte_sz;
        img->bound_gpu_va = w_gpu_va;
        sc->images.push_back(img);
        {
            std::lock_guard<std::mutex> lk(g_icd_mutex);
            g_images[reinterpret_cast<uint64_t>(img)] = {img->bound_gpu_va, img->width, img->height};
        }
    }
    sc->image_acquired.assign(sc->images.size(), false);
    sc->image_fences.assign(sc->images.size(), 0ULL);
    if (sc->is_wayland) {
        sc->wl_pools.assign(sc->images.size(), nullptr);
        sc->wl_buffers.assign(sc->images.size(), nullptr);
        sc->wl_shm_fds.assign(sc->images.size(), -1);
        sc->wl_shm_maps.assign(sc->images.size(), nullptr);
        sc->wl_shm_szs.assign(sc->images.size(), 0);
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
    if (mali_kbase_emu_wait_csf_idle) mali_kbase_emu_wait_csf_idle(200000000ULL);
    if (sc->xcb_conn) {
        if (sc->xcb_shm_ok && sc->xcb_shm_seg && g_xcb_lib.shm_detach) {
            g_xcb_lib.shm_detach(sc->xcb_conn, sc->xcb_shm_seg);
        }
        if (sc->xcb_gc && g_xcb_lib.free_gc) {
            g_xcb_lib.free_gc(sc->xcb_conn, sc->xcb_gc);
        }
        if (g_xcb_lib.flush) g_xcb_lib.flush(sc->xcb_conn);
    }
    if (sc->xcb_shm_addr && sc->xcb_shm_addr != reinterpret_cast<uint8_t*>(-1)) {
        shmdt(sc->xcb_shm_addr);
        sc->xcb_shm_addr = nullptr;
    }
    if (sc->xcb_shmid >= 0) {
        shmctl(sc->xcb_shmid, IPC_RMID, nullptr);
        sc->xcb_shmid = -1;
    }
    for (size_t i = 0; i < sc->wl_buffers.size(); ++i) {
        if (sc->wl_buffers[i] && g_wayland_lib.proxy_destroy) g_wayland_lib.proxy_destroy(sc->wl_buffers[i]);
        if (i < sc->wl_pools.size() && sc->wl_pools[i] && g_wayland_lib.proxy_destroy) g_wayland_lib.proxy_destroy(sc->wl_pools[i]);
        if (i < sc->wl_shm_maps.size() && sc->wl_shm_maps[i] && i < sc->wl_shm_szs.size() && sc->wl_shm_szs[i] > 0) {
            munmap(sc->wl_shm_maps[i], sc->wl_shm_szs[i]);
        }
        if (i < sc->wl_shm_fds.size() && sc->wl_shm_fds[i] >= 0) close(sc->wl_shm_fds[i]);
    }
    if (sc->wl_buffer && g_wayland_lib.proxy_destroy) g_wayland_lib.proxy_destroy(sc->wl_buffer);
    if (sc->wl_pool && g_wayland_lib.proxy_destroy) g_wayland_lib.proxy_destroy(sc->wl_pool);
    if (sc->wl_shm_map && sc->wl_shm_sz > 0) munmap(sc->wl_shm_map, sc->wl_shm_sz);
    if (sc->wl_shm_fd >= 0) close(sc->wl_shm_fd);
    for (int fd : sc->dmabuf_fds) {
        if (fd >= 0) close(fd);
    }
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
int32_t vkAcquireNextImageKHR(void* /*device*/, uint64_t swapchain, uint64_t /*timeout*/, uint64_t semaphore, uint64_t fence, uint32_t* pImageIndex) {
    if (!swapchain || !pImageIndex) return -3;
    auto* sc = reinterpret_cast<MaliIcdSwapchain*>(swapchain);
    if (sc->images.empty()) return -3;
    uint32_t idx = sc->next_image_idx % static_cast<uint32_t>(sc->images.size());
    *pImageIndex = idx;
    sc->next_image_idx = (idx + 1U) % static_cast<uint32_t>(sc->images.size());
    if (idx < sc->image_acquired.size()) sc->image_acquired[idx] = true;
    if (idx < sc->image_fences.size()) sc->image_fences[idx] = fence;
    if (fence) {
        auto* f = reinterpret_cast<MaliIcdFence*>(fence);
        f->signaled = true;
    }
    if (semaphore) {
        auto* s = reinterpret_cast<MaliIcdSemaphore*>(semaphore);
        if (s->is_timeline) s->counter_value++;
    }
    if (!sc->is_wayland && sc->xcb_conn && sc->xcb_window) {
        uint32_t cur_w = sc->width, cur_h = sc->height;
        if (g_xcb_lib.queryWindowGeometry(sc->xcb_conn, sc->xcb_window, &cur_w, &cur_h, nullptr)) {
            if (cur_w != sc->width || cur_h != sc->height) {
                return 1000001003; // VK_SUBOPTIMAL_KHR
            }
        }
    }
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
    if (mali_kbase_emu_wait_csf_idle) mali_kbase_emu_wait_csf_idle(200000000ULL);
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
    int32_t overall_res = 0;
    for (uint32_t i = 0; i < pi->swapchainCount; ++i) {
        auto* sc = reinterpret_cast<MaliIcdSwapchain*>(pi->pSwapchains[i]);
        uint32_t idx = pi->pImageIndices ? pi->pImageIndices[i] : 0U;
        int32_t sc_res = 0;
        std::string target_ppm;
        if (dump_dir && dump_dir[0]) {
            target_ppm = std::string(dump_dir) + "/vkcube_frame_" + std::to_string(s_present_frame_idx++) + ".ppm";
        } else if (dump_ppm && dump_ppm[0]) {
            target_ppm = dump_ppm;
        }
        if (sc && idx < sc->images.size()) {
            if (idx < sc->image_acquired.size()) sc->image_acquired[idx] = false;
            const auto* img = sc->images[idx];
            if (img && img->bound_gpu_va) {
                uint32_t stride_px = (img->row_pitch >= img->width * 4U && (img->row_pitch % 4U) == 0)
                                         ? (img->row_pitch / 4U)
                                         : img->width;
                bool is_bgra = (img->format == 44U || img->format == 50U);

                // 1a. Live Wayland wl_shm window presentation (per-slot wl_buffer/wl_shm_pool)
                if (sc->is_wayland && sc->wl_display && sc->wl_surface && g_wayland_lib.ensureLoaded()) {
                    if (!sc->wl_shm) {
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
                    }
                    if (sc->wl_shm && idx < sc->wl_buffers.size() && !sc->wl_buffers[idx]) {
                        size_t sz = static_cast<size_t>(img->width) * img->height * 4U;
                        int fd = memfd_create("mali_aot_wl_shm", 0);
                        if (fd >= 0 && ftruncate(fd, static_cast<off_t>(sz)) == 0) {
                            void* mp = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                            if (mp != MAP_FAILED) {
                                sc->wl_shm_fds[idx] = fd;
                                sc->wl_shm_maps[idx] = static_cast<uint32_t*>(mp);
                                sc->wl_shm_szs[idx] = sz;
                                void* pool = g_wayland_lib.proxy_marshal_constructor(
                                    sc->wl_shm, 0U /* WL_SHM_CREATE_POOL */,
                                    g_wayland_lib.shm_pool_interface, nullptr,
                                    fd, static_cast<int32_t>(sz));
                                sc->wl_pools[idx] = pool;
                                if (pool) {
                                    sc->wl_buffers[idx] = g_wayland_lib.proxy_marshal_constructor(
                                        pool, 0U /* WL_SHM_POOL_CREATE_BUFFER */,
                                        g_wayland_lib.buffer_interface, nullptr,
                                        0, static_cast<int32_t>(img->width), static_cast<int32_t>(img->height),
                                        static_cast<int32_t>(img->width * 4U), 1U /* WL_SHM_FORMAT_XRGB8888 */);
                                }
                            } else {
                                close(fd);
                            }
                        } else if (fd >= 0) {
                            close(fd);
                        }
                    }
                    if (idx < sc->wl_buffers.size() && sc->wl_buffers[idx] && sc->wl_shm_maps[idx]) {
                        const uint32_t* src_px = reinterpret_cast<const uint32_t*>(img->bound_gpu_va);
                        uint32_t* dst_px = sc->wl_shm_maps[idx];
                        if (is_bgra && stride_px == img->width) {
                            std::memcpy(dst_px, src_px, static_cast<size_t>(img->width) * img->height * 4U);
                        } else {
                            for (uint32_t y = 0; y < img->height; ++y) {
                                const uint32_t* srow = src_px + y * stride_px;
                                uint32_t* drow = dst_px + y * img->width;
                                if (is_bgra) {
                                    std::memcpy(drow, srow, img->width * 4U);
                                } else {
                                    for (uint32_t x = 0; x < img->width; ++x) {
                                        uint32_t v = srow[x];
                                        uint32_t r = v & 0xFFU;
                                        uint32_t g = (v >> 8) & 0xFFU;
                                        uint32_t b = (v >> 16) & 0xFFU;
                                        drow[x] = (0xFFU << 24) | (r << 16) | (g << 8) | b;
                                    }
                                }
                            }
                        }
                        g_wayland_lib.proxy_marshal(sc->wl_surface, 1U /* WL_SURFACE_ATTACH */, sc->wl_buffers[idx], 0, 0);
                        g_wayland_lib.proxy_marshal(sc->wl_surface, 2U /* WL_SURFACE_DAMAGE */, 0, 0,
                                                    static_cast<int32_t>(img->width), static_cast<int32_t>(img->height));
                        g_wayland_lib.proxy_marshal(sc->wl_surface, 6U /* WL_SURFACE_COMMIT */);
                        g_wayland_lib.display_flush(sc->wl_display);
                    }
                }

                // 1b. Live XCB window presentation (MIT-SHM fast path with xcb_put_image fallback)
                if (sc->xcb_conn && sc->xcb_window && g_xcb_lib.ensureLoaded()) {
                    if (sc->xcb_gc == 0) {
                        sc->xcb_gc = g_xcb_lib.generate_id(sc->xcb_conn);
                        g_xcb_lib.create_gc(sc->xcb_conn, sc->xcb_gc, sc->xcb_window, 0, nullptr);
                    }
                    if (!sc->xcb_shm_checked) {
                        sc->xcb_shm_checked = true;
                        if (g_xcb_lib.ensureShmLoaded()) {
                            auto ck = g_xcb_lib.shm_query_version(sc->xcb_conn);
                            auto* rep = g_xcb_lib.shm_query_version_reply(sc->xcb_conn, ck, nullptr);
                            if (rep) {
                                std::free(rep);
                                size_t shm_sz = static_cast<size_t>(img->width) * img->height * 4U;
                                int shmid = shmget(IPC_PRIVATE, shm_sz, IPC_CREAT | 0600);
                                if (shmid >= 0) {
                                    void* addr = shmat(shmid, nullptr, 0);
                                    if (addr != reinterpret_cast<void*>(-1)) {
                                        sc->xcb_shmid = shmid;
                                        sc->xcb_shm_addr = static_cast<uint8_t*>(addr);
                                        sc->xcb_shm_sz = shm_sz;
                                        sc->xcb_shm_seg = g_xcb_lib.generate_id(sc->xcb_conn);
                                        g_xcb_lib.shm_attach(sc->xcb_conn, sc->xcb_shm_seg, static_cast<uint32_t>(shmid), 0);
                                        shmctl(shmid, IPC_RMID, nullptr);
                                        sc->xcb_shmid = -1;
                                        sc->xcb_shm_ok = true;
                                    } else {
                                        shmctl(shmid, IPC_RMID, nullptr);
                                    }
                                }
                            }
                        }
                    }

                    const uint32_t* src_px = reinterpret_cast<const uint32_t*>(img->bound_gpu_va);
                    auto convertToX11 = [&](uint32_t* dst_px) {
                        if (is_bgra && stride_px == img->width) {
                            std::memcpy(dst_px, src_px, static_cast<size_t>(img->width) * img->height * 4U);
                        } else {
                            for (uint32_t y = 0; y < img->height; ++y) {
                                const uint32_t* srow = src_px + y * stride_px;
                                uint32_t* drow = dst_px + y * img->width;
                                if (is_bgra) {
                                    std::memcpy(drow, srow, img->width * 4U);
                                } else {
                                    for (uint32_t x = 0; x < img->width; ++x) {
                                        uint32_t v = srow[x];
                                        uint32_t r = v & 0xFFU;
                                        uint32_t g = (v >> 8) & 0xFFU;
                                        uint32_t b = (v >> 16) & 0xFFU;
                                        uint32_t a = (v >> 24) & 0xFFU;
                                        drow[x] = (a << 24) | (r << 16) | (g << 8) | b;
                                    }
                                }
                            }
                        }
                    };

                    if (sc->xcb_shm_ok && sc->xcb_shm_addr &&
                        sc->xcb_shm_sz >= static_cast<size_t>(img->width) * img->height * 4U) {
                        convertToX11(reinterpret_cast<uint32_t*>(sc->xcb_shm_addr));
                        g_xcb_lib.shm_put_image(sc->xcb_conn, sc->xcb_window, sc->xcb_gc,
                                                static_cast<uint16_t>(img->width), static_cast<uint16_t>(img->height),
                                                0, 0,
                                                static_cast<uint16_t>(img->width), static_cast<uint16_t>(img->height),
                                                0, 0,
                                                sc->xcb_depth ? sc->xcb_depth : 24,
                                                2 /* XCB_IMAGE_FORMAT_Z_PIXMAP */, 0,
                                                sc->xcb_shm_seg, 0);
                    } else {
                        std::vector<uint32_t> bgra_px(static_cast<size_t>(img->width) * img->height);
                        convertToX11(bgra_px.data());
                        const uint32_t* x11_px = bgra_px.data();
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
                    }
                    g_xcb_lib.flush(sc->xcb_conn);

                    uint32_t cur_w = sc->width, cur_h = sc->height;
                    if (g_xcb_lib.queryWindowGeometry(sc->xcb_conn, sc->xcb_window, &cur_w, &cur_h, nullptr)) {
                        if (cur_w != sc->width || cur_h != sc->height) {
                            sc_res = 1000001003; // VK_SUBOPTIMAL_KHR
                        }
                    }
                }

                // 2. Optional PPM frame dump
                if (!target_ppm.empty()) {
                    std::ofstream out(target_ppm, std::ios::binary);
                    if (out) {
                        out << "P6\n" << img->width << " " << img->height << "\n255\n";
                        const uint32_t* px = reinterpret_cast<const uint32_t*>(img->bound_gpu_va);
                        for (uint32_t y = 0; y < img->height; ++y) {
                            const uint32_t* row = px + y * stride_px;
                            for (uint32_t x = 0; x < img->width; ++x) {
                                uint32_t v = row[x];
                                uint8_t rgb[3] = {
                                    static_cast<uint8_t>(is_bgra ? ((v >> 16) & 0xFFU) : (v & 0xFFU)),
                                    static_cast<uint8_t>((v >> 8) & 0xFFU),
                                    static_cast<uint8_t>(is_bgra ? (v & 0xFFU) : ((v >> 16) & 0xFFU))
                                };
                                out.write(reinterpret_cast<const char*>(rgb), 3);
                            }
                        }
                    }
                }
            }
        }
        if (pi->pResults) pi->pResults[i] = sc_res;
        if (sc_res != 0 && overall_res == 0) overall_res = sc_res;
    }
    static uint64_t s_total_presents = 0;
    ++s_total_presents;
    if (s_total_presents <= 5 || (s_total_presents % 100) == 0) {
        mali_icd_log("vkQueuePresentKHR frame=%lu swapchains=%u",
                     static_cast<unsigned long>(s_total_presents), pi->swapchainCount);
    }
    return overall_res;
}

__attribute__((visibility("default")))
int32_t vkQueueWaitIdle(void* /*queue*/) {
    if (mali_kbase_emu_wait_csf_idle) mali_kbase_emu_wait_csf_idle(200000000ULL);
    return 0;
}

__attribute__((visibility("default")))
int32_t vkDeviceWaitIdle(void* device) {
    if (mali_kbase_emu_wait_csf_idle) mali_kbase_emu_wait_csf_idle(200000000ULL);
    auto* dev = static_cast<MaliIcdDevice*>(device);
    if (dev && dev->aot_cctx_mem.size() > 0xc1 && dev->aot_cctx_mem[0xc1] != 0) {
        return -4; /* VK_ERROR_DEVICE_LOST */
    }
    return 0;
}


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
int32_t vkCreateEvent(void* device, const void* pCreateInfo, const void* pAllocator, uint64_t* pEvent) {
    if (!pEvent) return -3;
    g_host_vk_allocators.registerCallbacks(pAllocator);
    auto* dev = static_cast<MaliIcdDevice*>(device);
    struct DefEventCI { uint32_t sType; const void* pNext; uint32_t flags; } def_ci = { 10U, nullptr, 0U };
    const void* ci_ptr = pCreateInfo ? pCreateInfo : &def_ci;
    if (dev && aot_vkCreateEvent && !dev->aot_vk_device_mem.empty()) {
        uint64_t aot_ev = 0;
        int32_t res = 0;
        {
            auto& tctx = maliGetThreadGuestContext();
            GuestStackFrameGuard frame(tctx);
            const size_t frame_stack_sz = static_cast<size_t>(tctx.cpu.sp - reinterpret_cast<uintptr_t>(tctx.stack_mem));
            res = mali_aapcs64::invokeLiftedAapcs64<int32_t>(
                tctx.cpu, tctx.stack_mem, frame_stack_sz, aot_vkCreateEvent,
                dev->aot_vk_device_mem.data(), ci_ptr, pAllocator, &aot_ev);
            g_aot_rt.lifted_vk_calls_executed++;
        }
        if (res == 0 && aot_ev != 0) {
            *pEvent = aot_ev;
            return 0;
        }
        if (res != 0) return res;
    }
    *pEvent = reinterpret_cast<uint64_t>(new MaliIcdEvent());
    return 0;
}
__attribute__((visibility("default")))
void vkDestroyEvent(void* device, uint64_t event, const void* pAllocator) {
    if (!event || event == 0xE7000001ULL) return;
    g_host_vk_allocators.registerCallbacks(pAllocator);
    auto* dev = static_cast<MaliIcdDevice*>(device);
    if (dev && aot_vkDestroyEvent && !dev->aot_vk_device_mem.empty() &&
        (*reinterpret_cast<const uint8_t*>(event) == 0x0aU)) {
        auto& tctx = maliGetThreadGuestContext();
        GuestStackFrameGuard frame(tctx);
        const size_t frame_stack_sz = static_cast<size_t>(tctx.cpu.sp - reinterpret_cast<uintptr_t>(tctx.stack_mem));
        mali_aapcs64::invokeLiftedAapcs64<void>(
            tctx.cpu, tctx.stack_mem, frame_stack_sz, aot_vkDestroyEvent,
            dev->aot_vk_device_mem.data(), event, pAllocator);
        g_aot_rt.lifted_vk_calls_executed++;
        return;
    }
    delete reinterpret_cast<MaliIcdEvent*>(event);
}
__attribute__((visibility("default")))
int32_t vkGetEventStatus(void* device, uint64_t event) {
    if (!event || event == 0xE7000001ULL) return 0;
    auto* dev = static_cast<MaliIcdDevice*>(device);
    if (dev && aot_vkGetEventStatus && !dev->aot_vk_device_mem.empty() &&
        (*reinterpret_cast<const uint8_t*>(event) == 0x0aU)) {
        auto& tctx = maliGetThreadGuestContext();
        GuestStackFrameGuard frame(tctx);
        const size_t frame_stack_sz = static_cast<size_t>(tctx.cpu.sp - reinterpret_cast<uintptr_t>(tctx.stack_mem));
        int32_t res = mali_aapcs64::invokeLiftedAapcs64<int32_t>(
            tctx.cpu, tctx.stack_mem, frame_stack_sz, aot_vkGetEventStatus,
            dev->aot_vk_device_mem.data(), event);
        g_aot_rt.lifted_vk_calls_executed++;
        return res;
    }
    if (dev && dev->aot_cctx_mem.size() > 0xc1 && dev->aot_cctx_mem[0xc1] != 0) {
        return -4; /* VK_ERROR_DEVICE_LOST */
    }
    return reinterpret_cast<MaliIcdEvent*>(event)->signaled ? 3 /* VK_EVENT_SET */ : 4 /* VK_EVENT_RESET */;
}
__attribute__((visibility("default")))
int32_t vkSetEvent(void* device, uint64_t event) {
    if (!event || event == 0xE7000001ULL) return 0;
    auto* dev = static_cast<MaliIcdDevice*>(device);
    if (dev && aot_vkSetEvent && !dev->aot_vk_device_mem.empty() &&
        (*reinterpret_cast<const uint8_t*>(event) == 0x0aU)) {
        auto& tctx = maliGetThreadGuestContext();
        GuestStackFrameGuard frame(tctx);
        const size_t frame_stack_sz = static_cast<size_t>(tctx.cpu.sp - reinterpret_cast<uintptr_t>(tctx.stack_mem));
        int32_t res = mali_aapcs64::invokeLiftedAapcs64<int32_t>(
            tctx.cpu, tctx.stack_mem, frame_stack_sz, aot_vkSetEvent,
            dev->aot_vk_device_mem.data(), event);
        g_aot_rt.lifted_vk_calls_executed++;
        return res;
    }
    reinterpret_cast<MaliIcdEvent*>(event)->signaled = true;
    return 0;
}
__attribute__((visibility("default")))
int32_t vkResetEvent(void* device, uint64_t event) {
    if (!event || event == 0xE7000001ULL) return 0;
    auto* dev = static_cast<MaliIcdDevice*>(device);
    if (dev && aot_vkResetEvent && !dev->aot_vk_device_mem.empty() &&
        (*reinterpret_cast<const uint8_t*>(event) == 0x0aU)) {
        auto& tctx = maliGetThreadGuestContext();
        GuestStackFrameGuard frame(tctx);
        const size_t frame_stack_sz = static_cast<size_t>(tctx.cpu.sp - reinterpret_cast<uintptr_t>(tctx.stack_mem));
        int32_t res = mali_aapcs64::invokeLiftedAapcs64<int32_t>(
            tctx.cpu, tctx.stack_mem, frame_stack_sz, aot_vkResetEvent,
            dev->aot_vk_device_mem.data(), event);
        g_aot_rt.lifted_vk_calls_executed++;
        return res;
    }
    reinterpret_cast<MaliIcdEvent*>(event)->signaled = false;
    return 0;
}
__attribute__((visibility("default")))
void vkCmdSetEvent(void* commandBuffer, uint64_t event, uint32_t stageMask) {
    if (!commandBuffer) return;
    auto* cb = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    MaliIcdRecordedCmd cmd{};
    cmd.type = MaliIcdRecordedCmd::CMD_SET_EVENT;
    cmd.event_handle = event;
    cmd.src_stage_mask = stageMask;
    cb->cmds.push_back(cmd);
}
__attribute__((visibility("default")))
void vkCmdResetEvent(void* commandBuffer, uint64_t event, uint32_t stageMask) {
    if (!commandBuffer) return;
    auto* cb = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    MaliIcdRecordedCmd cmd{};
    cmd.type = MaliIcdRecordedCmd::CMD_RESET_EVENT;
    cmd.event_handle = event;
    cmd.src_stage_mask = stageMask;
    cb->cmds.push_back(cmd);
}
__attribute__((visibility("default")))
void vkCmdWaitEvents(void* commandBuffer, uint32_t eventCount, const uint64_t* pEvents, uint32_t srcStageMask, uint32_t dstStageMask, uint32_t /*memoryBarrierCount*/, const void* /*pMemoryBarriers*/, uint32_t /*bufferMemoryBarrierCount*/, const void* /*pBufferMemoryBarriers*/, uint32_t /*imageMemoryBarrierCount*/, const void* /*pImageMemoryBarriers*/) {
    if (!commandBuffer) return;
    auto* cb = static_cast<MaliIcdCommandBuffer*>(commandBuffer);
    MaliIcdRecordedCmd cmd{};
    cmd.type = MaliIcdRecordedCmd::CMD_WAIT_EVENTS;
    cmd.src_stage_mask = srcStageMask;
    cmd.dst_stage_mask = dstStageMask;
    if (pEvents && eventCount > 0) cmd.wait_events.assign(pEvents, pEvents + eventCount);
    cb->cmds.push_back(cmd);
}
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

__attribute__((visibility("default")))
int32_t vkGetImageDrmFormatModifierPropertiesEXT(void* /*device*/, uint64_t /*image*/, void* pProperties) {
    if (!pProperties) return -3;
    struct DrmModProps { uint32_t sType; void* pNext; uint64_t drmFormatModifier; };
    auto* props = static_cast<DrmModProps*>(pProperties);
    props->drmFormatModifier = 0ULL; // DRM_FORMAT_MOD_LINEAR
    return 0;
}

__attribute__((visibility("default")))
int32_t vkGetMemoryFdKHR(void* /*device*/, const void* pGetFdInfo, int* pFd) {
    if (!pGetFdInfo || !pFd) return -3;
    struct GetFdInfo { uint32_t sType; const void* pNext; uint64_t memory; uint32_t handleType; };
    const auto* info = static_cast<const GetFdInfo*>(pGetFdInfo);
    auto* mem = reinterpret_cast<MaliIcdDeviceMemory*>(info->memory);
    if (mem && mem->dmabuf_fd >= 0) {
        *pFd = dup(mem->dmabuf_fd);
        return (*pFd >= 0) ? 0 : -1;
    }
    int fd = memfd_create("mali_icd_ext_mem", 0);
    if (fd < 0) return -1;
    if (mem && mem->size_bytes > 0) {
        if (ftruncate(fd, static_cast<off_t>(mem->size_bytes)) != 0) {
            close(fd);
            return -1;
        }
    }
    *pFd = fd;
    return 0;
}

} // extern "C"
