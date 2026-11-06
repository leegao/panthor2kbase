/*
 * SPDX-License-Identifier: MIT
 *
 * Panthor2Kbase runtime logging implementation (C++17 with RAII scoped indentation).
 * Default log level: PANTHOR2KBASE_LOG_TRACE (4).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <time.h>

#include "xf86drm.h"
#include "panthor_drm.h"
#include "mali_kbase_uapi.h"

panthor2kbase_log_level_t panthor2kbase_log_level = PANTHOR2KBASE_LOG_TRACE;
static bool g_log_initialized = false;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_log_fp = nullptr;

/* Thread-local call-stack indentation depth for RAII ScopedLogIndent */
static thread_local int g_log_indent_depth = 0;

static const char *const g_level_names[PANTHOR2KBASE_LOG_LEVEL_COUNT] = {
    "ERROR",
    "WARN",
    "INFO",
    "DEBUG",
    "TRACE",
};

static panthor2kbase_log_level_t panthor2kbase_parse_level(const char *s)
{
    if (!s || !s[0])
        return PANTHOR2KBASE_LOG_TRACE;

    /* Numeric form: 0..4 */
    char *end = nullptr;
    long v = strtol(s, &end, 10);
    if (end != s && end && *end == '\0') {
        if (v < 0)
            v = 0;
        if (v > static_cast<long>(PANTHOR2KBASE_LOG_TRACE))
            v = static_cast<long>(PANTHOR2KBASE_LOG_TRACE);
        return static_cast<panthor2kbase_log_level_t>(v);
    }

    /* Named form: error/warn/info/debug/trace (case-insensitive) */
    for (int i = 0; i < PANTHOR2KBASE_LOG_LEVEL_COUNT; i++) {
        if (strcasecmp(s, g_level_names[i]) == 0)
            return static_cast<panthor2kbase_log_level_t>(i);
    }
    return PANTHOR2KBASE_LOG_TRACE;
}

extern "C" int panthor2kbase_log_get_indent(void)
{
    return g_log_indent_depth;
}

extern "C" void panthor2kbase_log_inc_indent(void)
{
    if (g_log_indent_depth < 64)
        ++g_log_indent_depth;
}

extern "C" void panthor2kbase_log_dec_indent(void)
{
    if (g_log_indent_depth > 0)
        --g_log_indent_depth;
}

extern "C" void panthor2kbase_log_init(void)
{
    if (g_log_initialized)
        return;
    g_log_initialized = true;

    const char *level_s = getenv("PANTHOR2KBASE_LOG_LEVEL");
    panthor2kbase_log_level = panthor2kbase_parse_level(level_s);

    const char *path = getenv("PANTHOR2KBASE_LOG_PATH");
    if (path && path[0])
        g_log_fp = fopen(path, "a");
}

extern "C" void panthor2kbase_logv(panthor2kbase_log_level_t level, const char *fmt, va_list ap)
{
    panthor2kbase_log_init();
    if (level > panthor2kbase_log_level)
        return;

    int indent = g_log_indent_depth;
    if (indent < 0)
        indent = 0;
    if (indent > 32)
        indent = 32;

    FILE *fp = g_log_fp ? g_log_fp : stderr;
    pthread_mutex_lock(&g_log_lock);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(fp, "[%ld.%06ld] %-5s: %*s",
            static_cast<long>(ts.tv_sec),
            static_cast<long>(ts.tv_nsec / 1000UL),
            g_level_names[level],
            indent * 2, "");
    vfprintf(fp, fmt, ap);
    fputc('\n', fp);
    fflush(fp);

    pthread_mutex_unlock(&g_log_lock);
}

extern "C" void panthor2kbase_log(panthor2kbase_log_level_t level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    panthor2kbase_logv(level, fmt, ap);
    va_end(ap);
}

extern "C" const char *panthor2kbase_ioctl_req_name(unsigned long request)
{
    unsigned int type = _IOC_TYPE(request);
    unsigned int nr = _IOC_NR(request);

    if (type == 'd') {
        switch (nr) {
        case 0x00: return "DRM_IOCTL_VERSION";
        case 0x01: return "DRM_IOCTL_GET_UNIQUE";
        case 0x02: return "DRM_IOCTL_GET_MAGIC";
        case 0x03: return "DRM_IOCTL_IRQ_BUSID";
        case 0x04: return "DRM_IOCTL_GET_MAP";
        case 0x05: return "DRM_IOCTL_GET_CLIENT";
        case 0x06: return "DRM_IOCTL_GET_STATS";
        case 0x07: return "DRM_IOCTL_SET_VERSION";
        case 0x08: return "DRM_IOCTL_MODESET_CTL";
        case 0x09: return "DRM_IOCTL_GEM_CLOSE";
        case 0x0a: return "DRM_IOCTL_GEM_FLINK";
        case 0x0b: return "DRM_IOCTL_GEM_OPEN";
        case 0x0c: return "DRM_IOCTL_GET_CAP";
        case 0x0d: return "DRM_IOCTL_SET_CLIENT_CAP";
        case 0x2d: return "DRM_IOCTL_PRIME_HANDLE_TO_FD";
        case 0x2e: return "DRM_IOCTL_PRIME_FD_TO_HANDLE";
        case 0xbf: return "DRM_IOCTL_SYNCOBJ_CREATE";
        case 0xc0: return "DRM_IOCTL_SYNCOBJ_DESTROY";
        case 0xc1: return "DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD";
        case 0xc2: return "DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE";
        case 0xc3: return "DRM_IOCTL_SYNCOBJ_WAIT";
        case 0xc4: return "DRM_IOCTL_SYNCOBJ_RESET";
        case 0xc5: return "DRM_IOCTL_SYNCOBJ_SIGNAL";
        case 0xca: return "DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT";
        case 0xcb: return "DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL";
        case 0xcc: return "DRM_IOCTL_SYNCOBJ_TRANSFER";
        case 0xcd: return "DRM_IOCTL_SYNCOBJ_QUERY";
        default: break;
        }
        if (nr >= DRM_COMMAND_BASE && nr < DRM_COMMAND_BASE + 0x10) {
            switch (nr - DRM_COMMAND_BASE) {
            case DRM_PANTHOR_DEV_QUERY:            return "DRM_IOCTL_PANTHOR_DEV_QUERY";
            case DRM_PANTHOR_VM_CREATE:            return "DRM_IOCTL_PANTHOR_VM_CREATE";
            case DRM_PANTHOR_VM_DESTROY:           return "DRM_IOCTL_PANTHOR_VM_DESTROY";
            case DRM_PANTHOR_VM_BIND:              return "DRM_IOCTL_PANTHOR_VM_BIND";
            case DRM_PANTHOR_VM_GET_STATE:         return "DRM_IOCTL_PANTHOR_VM_GET_STATE";
            case DRM_PANTHOR_BO_CREATE:            return "DRM_IOCTL_PANTHOR_BO_CREATE";
            case DRM_PANTHOR_BO_MMAP_OFFSET:       return "DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET";
            case DRM_PANTHOR_GROUP_CREATE:         return "DRM_IOCTL_PANTHOR_GROUP_CREATE";
            case DRM_PANTHOR_GROUP_DESTROY:        return "DRM_IOCTL_PANTHOR_GROUP_DESTROY";
            case DRM_PANTHOR_GROUP_SUBMIT:         return "DRM_IOCTL_PANTHOR_GROUP_SUBMIT";
            case DRM_PANTHOR_GROUP_GET_STATE:      return "DRM_IOCTL_PANTHOR_GROUP_GET_STATE";
            case DRM_PANTHOR_TILER_HEAP_CREATE:    return "DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE";
            case DRM_PANTHOR_TILER_HEAP_DESTROY:   return "DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY";
            case DRM_PANTHOR_BO_SYNC:              return "DRM_IOCTL_PANTHOR_BO_SYNC";
            case DRM_PANTHOR_SET_USER_MMIO_OFFSET: return "DRM_IOCTL_PANTHOR_SET_USER_MMIO_OFFSET";
            case DRM_PANTHOR_BO_SET_LABEL:         return "DRM_IOCTL_PANTHOR_BO_SET_LABEL";
            case DRM_PANTHOR_BO_QUERY_INFO:        return "DRM_IOCTL_PANTHOR_BO_QUERY_INFO";
            default: break;
            }
            static thread_local char panthor_name[40];
            snprintf(panthor_name, sizeof(panthor_name), "DRM_IOCTL_PANTHOR_0x%02x", nr - DRM_COMMAND_BASE);
            return panthor_name;
        }
        static thread_local char drm_name[32];
        snprintf(drm_name, sizeof(drm_name), "DRM_IOCTL_0x%02x", nr);
        return drm_name;
    }

    if (type == KBASE_IOCTL_TYPE) {
        switch (nr) {
        case 0:  return "KBASE_IOCTL_VERSION_CHECK_RESERVED";
        case 1:  return "KBASE_IOCTL_SET_FLAGS";
        case 2:  return "KBASE_IOCTL_JOB_SUBMIT";
        case 3:  return "KBASE_IOCTL_GET_GPUPROPS";
        case 5:  return "KBASE_IOCTL_MEM_ALLOC";
        case 6:  return "KBASE_IOCTL_MEM_QUERY";
        case 7:  return "KBASE_IOCTL_MEM_FREE";
        case 13: return "KBASE_IOCTL_GET_DDK_VERSION";
        case 14: return "KBASE_IOCTL_MEM_JIT_INIT";
        case 15: return "KBASE_IOCTL_MEM_SYNC";
        case 20: return "KBASE_IOCTL_MEM_COMMIT";
        case 21: return "KBASE_IOCTL_MEM_ALIAS";
        case 22: return "KBASE_IOCTL_MEM_IMPORT";
        case 24: return "KBASE_IOCTL_STREAM_CREATE";
        case 36: return "KBASE_IOCTL_CS_QUEUE_REGISTER";
        case 37: return "KBASE_IOCTL_CS_QUEUE_KICK";
        case 38: return "KBASE_IOCTL_MEM_EXEC_INIT";
        case 39: return "KBASE_IOCTL_CS_QUEUE_BIND";
        case 40: return "KBASE_IOCTL_CS_QUEUE_REGISTER_EX";
        case 41: return "KBASE_IOCTL_CS_QUEUE_TERMINATE";
        case 42: return "KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6";
        case 43: return "KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE";
        case 44: return "KBASE_IOCTL_CS_EVENT_SIGNAL";
        case 45: return "KBASE_IOCTL_KCPU_QUEUE_CREATE";
        case 46: return "KBASE_IOCTL_KCPU_QUEUE_DELETE";
        case 47: return "KBASE_IOCTL_KCPU_QUEUE_ENQUEUE";
        case 48: return "KBASE_IOCTL_CS_TILER_HEAP_INIT";
        case 49: return "KBASE_IOCTL_CS_TILER_HEAP_TERM";
        case 50: return "KBASE_IOCTL_GET_CPU_GPU_TIMEINFO";
        case 51: return "KBASE_IOCTL_CS_GET_GLB_IFACE";
        case 52: return "KBASE_IOCTL_VERSION_CHECK";
        case 54: return "KBASE_IOCTL_CONTEXT_PRIORITY_CHECK";
        case 58: return "KBASE_IOCTL_CS_QUEUE_GROUP_CREATE";
        case 59: return "KBASE_IOCTL_MEM_ALLOC_EX";
        default: break;
        }
        static thread_local char kbase_name[40];
        snprintf(kbase_name, sizeof(kbase_name), "KBASE_IOCTL_%u", nr);
        return kbase_name;
    }

    static thread_local char name[40];
    snprintf(name, sizeof(name), "ioctl type=0x%x nr=0x%x", type, nr);
    return name;
}

extern "C" int panthor2kbase_adapter_ioctl(int fd, unsigned long request, void *arg, const char *call_text)
{
    PANTHOR2KBASE_LOG_SCOPE("adapter_ioctl", "fd=%d, call=%s, req=%s(0x%lx)",
                            fd, call_text ? call_text : "?",
                            panthor2kbase_ioctl_req_name(request), request);

    typedef int (*ioctl_fn_t)(int, unsigned long, ...);
    static ioctl_fn_t real_ioctl_fn = nullptr;
    if (!real_ioctl_fn) {
        bool has_emu = (dlsym(RTLD_DEFAULT, "mali_kbase_emu_get_stats") != nullptr);
        void *handle = has_emu ? RTLD_DEFAULT : RTLD_NEXT;
        real_ioctl_fn = reinterpret_cast<ioctl_fn_t>(dlsym(handle, "ioctl"));
        if (!real_ioctl_fn)
            real_ioctl_fn = reinterpret_cast<ioctl_fn_t>(dlsym(RTLD_DEFAULT, "ioctl"));
    }

    /* Decode input fields for key kbase ioctls at TRACE level */
    if (_IOC_TYPE(request) == KBASE_IOCTL_TYPE && arg) {
        unsigned int nr = _IOC_NR(request);
        if (nr == 5) {
            const auto *ma = static_cast<const union kbase_ioctl_mem_alloc *>(arg);
            LOG_TRACE("in: va_pages=%llu commit_pages=%llu extension=%llu flags=0x%llx",
                      static_cast<unsigned long long>(ma->in.va_pages),
                      static_cast<unsigned long long>(ma->in.commit_pages),
                      static_cast<unsigned long long>(ma->in.extension),
                      static_cast<unsigned long long>(ma->in.flags));
        } else if (nr == 59) {
            const auto *ma = static_cast<const union kbase_ioctl_mem_alloc_ex *>(arg);
            LOG_TRACE("in: va_pages=%llu commit_pages=%llu extension=%llu flags=0x%llx fixed_va=0x%llx",
                      static_cast<unsigned long long>(ma->in.va_pages),
                      static_cast<unsigned long long>(ma->in.commit_pages),
                      static_cast<unsigned long long>(ma->in.extension),
                      static_cast<unsigned long long>(ma->in.flags),
                      static_cast<unsigned long long>(ma->in.fixed_address));
        }
    }

    errno = 0;
    int ret = real_ioctl_fn(fd, request, arg);
    int saved_errno = errno;

    if (ret < 0) {
        LOG_TRACE("IOCTL %s -> ret=%d errno=%d (%s)",
                  call_text, ret, saved_errno, strerror(saved_errno));
    } else {
        LOG_TRACE("IOCTL %s -> ret=%d (OK)", call_text, ret);
    }

    errno = saved_errno;
    return ret;
}
