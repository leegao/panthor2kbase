#define _GNU_SOURCE
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <time.h>

#include "xf86drm.h"

panthor2kbase_log_level_t panthor2kbase_log_level = PANTHOR2KBASE_LOG_DEBUG;
static bool g_log_initialized = false;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_log_fp = NULL;

static const char *const g_level_names[PANTHOR2KBASE_LOG_LEVEL_COUNT] = {
    [PANTHOR2KBASE_LOG_ERROR] = "ERROR",
    [PANTHOR2KBASE_LOG_WARN]  = "WARN",
    [PANTHOR2KBASE_LOG_INFO]  = "INFO",
    [PANTHOR2KBASE_LOG_DEBUG] = "DEBUG",
    [PANTHOR2KBASE_LOG_TRACE] = "TRACE",
};

static panthor2kbase_log_level_t panthor2kbase_parse_level(const char *s)
{
    if (!s || !s[0])
        return PANTHOR2KBASE_LOG_TRACE;

    /* Numeric form: 0..4 */
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end != s && *end == '\0') {
        if (v < 0)
            v = 0;
        if (v > (long)PANTHOR2KBASE_LOG_TRACE)
            v = (long)PANTHOR2KBASE_LOG_TRACE;
        return (panthor2kbase_log_level_t)v;
    }

    /* Named form: error/warn/info/debug/trace (case-insensitive) */
    for (int i = 0; i < PANTHOR2KBASE_LOG_LEVEL_COUNT; i++) {
        if (strcasecmp(s, g_level_names[i]) == 0)
            return (panthor2kbase_log_level_t)i;
    }
    return PANTHOR2KBASE_LOG_WARN;
}

void panthor2kbase_log_init(void)
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

void panthor2kbase_logv(panthor2kbase_log_level_t level, const char *fmt, va_list ap)
{
    panthor2kbase_log_init();
    if (level > panthor2kbase_log_level)
        return;

    FILE *fp = g_log_fp ? g_log_fp : stderr;
    pthread_mutex_lock(&g_log_lock);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(fp, "[%ld.%06ld] %s: ", ts.tv_sec, ts.tv_nsec / 1000UL,
            g_level_names[level]);
    vfprintf(fp, fmt, ap);
    fputc('\n', fp);
    fflush(fp);

    pthread_mutex_unlock(&g_log_lock);
}

void panthor2kbase_log(panthor2kbase_log_level_t level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    panthor2kbase_logv(level, fmt, ap);
    va_end(ap);
}

const char *panthor2kbase_ioctl_req_name(unsigned long request)
{
    unsigned int type = _IOC_TYPE(request);
    unsigned int nr = _IOC_NR(request);

    if (type == 'd') {
        switch (nr) {
        case 0x00: return "DRM_IOCTL_VERSION";
        case 0x01: return "DRM_IOCTL_GET_UNIQUE";
        case 0x02: return "DRM_IOCTL_GET_PARAM";
        case 0x03: return "DRM_IOCTL_SET_UNIQUE";
        case 0x04: return "DRM_IOCTL_GET_MAGIC";
        case 0x05: return "DRM_IOCTL_IRQ_BUSID";
        case 0x06: return "DRM_IOCTL_GET_IRQ";
        case 0x07: return "DRM_IOCTL_SET_MASTER";
        case 0x08: return "DRM_IOCTL_DROP_MASTER";
        case 0x09: return "DRM_IOCTL_GEM_CLOSE";
        case 0x0a: return "DRM_IOCTL_GET_UNIQUE";
        case 0x0b: return "DRM_IOCTL_GET_PARAM";
        case 0x0c: return "DRM_IOCTL_GET_CAP";
        case 0x0d: return "DRM_IOCTL_SET_CLIENT_CAP";
        case 0x0e: return "DRM_IOCTL_GET_STATS";
        case 0x2d: return "DRM_IOCTL_SYNCOBJ_WAIT";
        case 0x2e: return "DRM_IOCTL_PRIME_FD_TO_HANDLE";
        case 0xbf: return "DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD";
        case 0xc0: return "DRM_IOCTL_SYNCOBJ_DESTROY";
        case 0xc1: return "DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD";
        case 0xc2: return "DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE";
        case 0xc3: return "DRM_IOCTL_SYNCOBJ_RESET";
        case 0xc4: return "DRM_IOCTL_SYNCOBJ_SIGNAL";
        case 0xc5: return "DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL";
        case 0xca: return "DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT";
        case 0xcb: return "DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL";
        case 0xcc: return "DRM_IOCTL_SYNCOBJ_TRANSFER";
        case 0xcd: return "DRM_IOCTL_SYNCOBJ_QUERY";
        default: break;
        }
        if (nr >= DRM_COMMAND_BASE && nr < DRM_COMMAND_BASE + 0x10) {
            static char name[32];
            snprintf(name, sizeof(name), "DRM_IOCTL_PANTHOR_0x%02x", nr - DRM_COMMAND_BASE);
            return name;
        }
        static char name[32];
        snprintf(name, sizeof(name), "DRM_IOCTL_0x%02x", nr);
        return name;
    }
    static char name[32];
    snprintf(name, sizeof(name), "ioctl type=0x%x nr=0x%x", type, nr);
    return name;
}

int panthor2kbase_adapter_ioctl(int fd, unsigned long request, void *arg, const char *call_text)
{
    static int (*real_ioctl_fn)(int, unsigned long, ...) = NULL;
    if (!real_ioctl_fn) {
        bool has_emu = (dlsym(RTLD_DEFAULT, "mali_kbase_emu_get_stats") != NULL);
        void *handle = has_emu ? RTLD_DEFAULT : RTLD_NEXT;
        real_ioctl_fn = dlsym(handle, "ioctl");
        if (!real_ioctl_fn)
            real_ioctl_fn = dlsym(RTLD_DEFAULT, "ioctl");
    }

    panthor2kbase_log(PANTHOR2KBASE_LOG_TRACE,
                      "IOCTL %s fd=%d req=%s(0x%lx) arg=%p",
                      call_text, fd, panthor2kbase_ioctl_req_name(request), request, arg);

    int ret = real_ioctl_fn(fd, request, arg);

    panthor2kbase_log(PANTHOR2KBASE_LOG_TRACE,
                      "IOCTL %s -> ret=%d errno=%d (%s)",
                      call_text, ret, errno, strerror(errno));

    return ret;
}
