/*
 * SPDX-License-Identifier: MIT
 *
 * Panthor2Kbase runtime logging implementation (C++17 with RAII scoped indentation
 * and complete input/output ioctl struct deserialization).
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
        return PANTHOR2KBASE_LOG_INFO;

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
    fprintf(stderr, "[panthor2kbase_log_init]: panthor2kbase_parse_level returned %-5s\n",
            g_level_names[panthor2kbase_log_level]);

    const char *path = getenv("PANTHOR2KBASE_LOG_PATH");
    if (path && path[0]) {
        g_log_fp = fopen(path, "a");
        if (g_log_fp)
            setvbuf(g_log_fp, nullptr, _IOFBF, 65536);
    }
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
    static uint32_t s_log_line_cnt = 0;
    static int s_sync_flush = -1;
    if (s_sync_flush < 0) {
        const char *sf = getenv("PANTHOR2KBASE_LOG_SYNC_FLUSH");
        s_sync_flush = (sf && sf[0] && atoi(sf) != 0) ? 1 : 0;
    }
    ++s_log_line_cnt;
    if (s_sync_flush || level <= PANTHOR2KBASE_LOG_INFO || (s_log_line_cnt & 63U) == 0U)
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
        case 0xcb: return "DRM_IOCTL_SYNCOBJ_QUERY";
        case 0xcc: return "DRM_IOCTL_SYNCOBJ_TRANSFER";
        case 0xcd: return "DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL";
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

extern "C" void panthor2kbase_log_ioctl_struct_in(unsigned long request, const void *arg)
{
    if (!panthor2kbase_log_level_enabled(PANTHOR2KBASE_LOG_TRACE) || !arg)
        return;

    unsigned int type = _IOC_TYPE(request);
    unsigned int nr = _IOC_NR(request);
    unsigned int sz = _IOC_SIZE(request);

    if (type == KBASE_IOCTL_TYPE) {
        switch (nr) {
        case 0:
        case 52: {
            const auto *s = static_cast<const struct kbase_ioctl_version_check *>(arg);
            LOG_TRACE("in struct kbase_ioctl_version_check { major=%u, minor=%u }", s->major, s->minor);
            break;
        }
        case 1: {
            const auto *s = static_cast<const struct kbase_ioctl_set_flags *>(arg);
            LOG_TRACE("in struct kbase_ioctl_set_flags { create_flags=0x%x }", s->create_flags);
            break;
        }
        case 3: {
            const auto *s = static_cast<const struct kbase_ioctl_get_gpuprops *>(arg);
            LOG_TRACE("in struct kbase_ioctl_get_gpuprops { buffer=0x%llx, size=%u, flags=0x%x }",
                      static_cast<unsigned long long>(s->buffer), s->size, s->flags);
            break;
        }
        case 5: {
            const auto *ma = static_cast<const union kbase_ioctl_mem_alloc *>(arg);
            LOG_TRACE("in union kbase_ioctl_mem_alloc.in { va_pages=%llu, commit_pages=%llu, extension=%llu, flags=0x%llx }",
                      static_cast<unsigned long long>(ma->in.va_pages),
                      static_cast<unsigned long long>(ma->in.commit_pages),
                      static_cast<unsigned long long>(ma->in.extension),
                      static_cast<unsigned long long>(ma->in.flags));
            break;
        }
        case 6: {
            const auto *mq = static_cast<const union kbase_ioctl_mem_query *>(arg);
            LOG_TRACE("in union kbase_ioctl_mem_query.in { gpu_addr=0x%llx, query=%llu }",
                      static_cast<unsigned long long>(mq->in.gpu_addr),
                      static_cast<unsigned long long>(mq->in.query));
            break;
        }
        case 7: {
            const auto *mf = static_cast<const struct kbase_ioctl_mem_free *>(arg);
            LOG_TRACE("in struct kbase_ioctl_mem_free { gpu_addr=0x%llx }",
                      static_cast<unsigned long long>(mf->gpu_addr));
            break;
        }
        case 13: {
            const auto *dv = static_cast<const struct kbase_ioctl_get_ddk_version *>(arg);
            LOG_TRACE("in struct kbase_ioctl_get_ddk_version { version_buffer=0x%llx, size=%u, padding=%u }",
                      static_cast<unsigned long long>(dv->version_buffer), dv->size, dv->padding);
            break;
        }
        case 14: {
            if (sz == 8) {
                const uint64_t *va_pages_ptr = static_cast<const uint64_t *>(arg);
                LOG_TRACE("in struct kbase_ioctl_mem_jit_init_10_2 { va_pages=%llu }",
                          static_cast<unsigned long long>(*va_pages_ptr));
            } else {
                const auto *ji = static_cast<const struct kbase_ioctl_mem_jit_init *>(arg);
                LOG_TRACE("in struct kbase_ioctl_mem_jit_init { va_pages=%llu, max_allocations=%u, trim_level=%u, group_id=%u, phys_pages=%llu }",
                          static_cast<unsigned long long>(ji->va_pages), ji->max_allocations, ji->trim_level, ji->group_id,
                          static_cast<unsigned long long>(ji->phys_pages));
            }
            break;
        }
        case 15: {
            const auto *ms = static_cast<const struct kbase_ioctl_mem_sync *>(arg);
            LOG_TRACE("in struct kbase_ioctl_mem_sync { handle=0x%llx, user_addr=0x%llx, size=%llu, type=%u }",
                      static_cast<unsigned long long>(ms->handle),
                      static_cast<unsigned long long>(ms->user_addr),
                      static_cast<unsigned long long>(ms->size), ms->type);
            break;
        }
        case 20: {
            const auto *mc = static_cast<const struct kbase_ioctl_mem_commit *>(arg);
            LOG_TRACE("in struct kbase_ioctl_mem_commit { gpu_addr=0x%llx, pages=%llu }",
                      static_cast<unsigned long long>(mc->gpu_addr),
                      static_cast<unsigned long long>(mc->pages));
            break;
        }
        case 21: {
            const auto *al = static_cast<const union kbase_ioctl_mem_alias *>(arg);
            LOG_TRACE("in union kbase_ioctl_mem_alias.in { flags=0x%llx, stride=%llu, nents=%llu, aliasing_info=0x%llx }",
                      static_cast<unsigned long long>(al->in.flags),
                      static_cast<unsigned long long>(al->in.stride),
                      static_cast<unsigned long long>(al->in.nents),
                      static_cast<unsigned long long>(al->in.aliasing_info));
            if (al->in.aliasing_info && al->in.nents <= 8) {
                const auto *ai = reinterpret_cast<const struct base_mem_aliasing_info *>(
                    static_cast<uintptr_t>(al->in.aliasing_info));
                for (uint64_t i = 0; i < al->in.nents; i++) {
                    LOG_TRACE("  in aliasing_info[%llu] { handle=0x%llx, offset=%llu, length=%llu }",
                              static_cast<unsigned long long>(i),
                              static_cast<unsigned long long>(ai[i].handle),
                              static_cast<unsigned long long>(ai[i].offset),
                              static_cast<unsigned long long>(ai[i].length));
                }
            }
            break;
        }
        case 22: {
            const auto *im = static_cast<const union kbase_ioctl_mem_import *>(arg);
            int fd_val = im->in.phandle ? *reinterpret_cast<const int *>(static_cast<uintptr_t>(im->in.phandle)) : -1;
            LOG_TRACE("in union kbase_ioctl_mem_import.in { flags=0x%llx, phandle=0x%llx(fd=%d), type=%u }",
                      static_cast<unsigned long long>(im->in.flags),
                      static_cast<unsigned long long>(im->in.phandle), fd_val, im->in.type);
            break;
        }
        case 24: {
            const auto *sc = static_cast<const union kbase_ioctl_stream_create_u *>(arg);
            LOG_TRACE("in union kbase_ioctl_stream_create.in { name=\"%.31s\" }", sc->in.name);
            break;
        }
        case 36: {
            const auto *qr = static_cast<const struct kbase_ioctl_cs_queue_register *>(arg);
            LOG_TRACE("in struct kbase_ioctl_cs_queue_register { buffer_gpu_addr=0x%llx, buffer_size=%u, priority=%u }",
                      static_cast<unsigned long long>(qr->buffer_gpu_addr), qr->buffer_size, qr->priority);
            break;
        }
        case 37: {
            const auto *qk = static_cast<const struct kbase_ioctl_cs_queue_kick *>(arg);
            LOG_TRACE("in struct kbase_ioctl_cs_queue_kick { buffer_gpu_addr=0x%llx }",
                      static_cast<unsigned long long>(qk->buffer_gpu_addr));
            break;
        }
        case 38: {
            const auto *ei = static_cast<const struct kbase_ioctl_mem_exec_init *>(arg);
            LOG_TRACE("in struct kbase_ioctl_mem_exec_init { va_pages=%llu }",
                      static_cast<unsigned long long>(ei->va_pages));
            break;
        }
        case 39: {
            const auto *qb = static_cast<const union kbase_ioctl_cs_queue_bind *>(arg);
            LOG_TRACE("in union kbase_ioctl_cs_queue_bind.in { buffer_gpu_addr=0x%llx, group_handle=%u, csi_index=%u }",
                      static_cast<unsigned long long>(qb->in.buffer_gpu_addr),
                      qb->in.group_handle, qb->in.csi_index);
            break;
        }
        case 41: {
            const auto *qt = static_cast<const struct kbase_ioctl_cs_queue_terminate *>(arg);
            LOG_TRACE("in struct kbase_ioctl_cs_queue_terminate { buffer_gpu_addr=0x%llx }",
                      static_cast<unsigned long long>(qt->buffer_gpu_addr));
            break;
        }
        case 42: {
            const auto *cg = static_cast<const union kbase_ioctl_cs_queue_group_create_1_6 *>(arg);
            LOG_TRACE("in union kbase_ioctl_cs_queue_group_create_1_6.in { tiler_mask=0x%llx, fragment_mask=0x%llx, compute_mask=0x%llx, cs_min=%u, priority=%u, tiler_max=%u, fragment_max=%u, compute_max=%u }",
                      static_cast<unsigned long long>(cg->in.tiler_mask),
                      static_cast<unsigned long long>(cg->in.fragment_mask),
                      static_cast<unsigned long long>(cg->in.compute_mask),
                      cg->in.cs_min, cg->in.priority, cg->in.tiler_max,
                      cg->in.fragment_max, cg->in.compute_max);
            break;
        }
        case 43: {
            const auto *gt = static_cast<const struct kbase_ioctl_cs_queue_group_term *>(arg);
            LOG_TRACE("in struct kbase_ioctl_cs_queue_group_term { group_handle=%u }", gt->group_handle);
            break;
        }
        case 46: {
            const auto *kd = static_cast<const struct kbase_ioctl_kcpu_queue_delete *>(arg);
            LOG_TRACE("in struct kbase_ioctl_kcpu_queue_delete { id=%u }", kd->id);
            break;
        }
        case 47: {
            const auto *ke = static_cast<const struct kbase_ioctl_kcpu_queue_enqueue *>(arg);
            LOG_TRACE("in struct kbase_ioctl_kcpu_queue_enqueue { addr=0x%llx, nr_commands=%u, id=%u }",
                      static_cast<unsigned long long>(ke->addr), ke->nr_commands, ke->id);
            if (ke->addr && ke->nr_commands <= 8) {
                const auto *cmds = reinterpret_cast<const struct base_kcpu_command *>(
                    static_cast<uintptr_t>(ke->addr));
                for (uint32_t i = 0; i < ke->nr_commands; i++) {
                    LOG_TRACE("  in base_kcpu_command[%u] { type=%u }", i, cmds[i].type);
                }
            }
            break;
        }
        case 48: {
            if (sz == sizeof(union kbase_ioctl_cs_tiler_heap_init_1_13)) {
                const auto *hi = static_cast<const union kbase_ioctl_cs_tiler_heap_init_1_13 *>(arg);
                LOG_TRACE("in union kbase_ioctl_cs_tiler_heap_init_1_13.in { chunk_size=%u, initial_chunks=%u, max_chunks=%u, target_in_flight=%u, group_id=%u }",
                          hi->in.chunk_size, hi->in.initial_chunks, hi->in.max_chunks,
                          hi->in.target_in_flight, hi->in.group_id);
            } else {
                const auto *hi = static_cast<const union kbase_ioctl_cs_tiler_heap_init *>(arg);
                LOG_TRACE("in union kbase_ioctl_cs_tiler_heap_init.in { chunk_size=%u, initial_chunks=%u, max_chunks=%u, target_in_flight=%u, group_id=%u, buf_desc_va=0x%llx }",
                          hi->in.chunk_size, hi->in.initial_chunks, hi->in.max_chunks,
                          hi->in.target_in_flight, hi->in.group_id,
                          static_cast<unsigned long long>(hi->in.buf_desc_va));
            }
            break;
        }
        case 49: {
            const auto *ht = static_cast<const struct kbase_ioctl_cs_tiler_heap_term *>(arg);
            LOG_TRACE("in struct kbase_ioctl_cs_tiler_heap_term { gpu_heap_va=0x%llx }",
                      static_cast<unsigned long long>(ht->gpu_heap_va));
            break;
        }
        case 50: {
            const auto *ti = static_cast<const union kbase_ioctl_get_cpu_gpu_timeinfo *>(arg);
            LOG_TRACE("in union kbase_ioctl_get_cpu_gpu_timeinfo.in { request_flags=0x%x }", ti->in.request_flags);
            break;
        }
        case 51: {
            const auto *gi = static_cast<const union kbase_ioctl_cs_get_glb_iface *>(arg);
            LOG_TRACE("in union kbase_ioctl_cs_get_glb_iface.in { max_group_num=%u, max_total_stream_num=%u, groups_ptr=0x%llx, streams_ptr=0x%llx }",
                      gi->in.max_group_num, gi->in.max_total_stream_num,
                      static_cast<unsigned long long>(gi->in.groups_ptr),
                      static_cast<unsigned long long>(gi->in.streams_ptr));
            break;
        }
        case 54: {
            const auto *pc = static_cast<const struct kbase_ioctl_context_priority_check *>(arg);
            LOG_TRACE("in struct kbase_ioctl_context_priority_check { priority=%u }", pc->priority);
            break;
        }
        case 59: {
            const auto *ma = static_cast<const union kbase_ioctl_mem_alloc_ex *>(arg);
            LOG_TRACE("in union kbase_ioctl_mem_alloc_ex.in { va_pages=%llu, commit_pages=%llu, extension=%llu, flags=0x%llx, fixed_address=0x%llx }",
                      static_cast<unsigned long long>(ma->in.va_pages),
                      static_cast<unsigned long long>(ma->in.commit_pages),
                      static_cast<unsigned long long>(ma->in.extension),
                      static_cast<unsigned long long>(ma->in.flags),
                      static_cast<unsigned long long>(ma->in.fixed_address));
            break;
        }
        default:
            break;
        }
        return;
    }

    if (type == 'd') {
        if (nr >= DRM_COMMAND_BASE && nr < DRM_COMMAND_BASE + 0x10) {
            switch (nr - DRM_COMMAND_BASE) {
            case DRM_PANTHOR_DEV_QUERY: {
                const auto *q = static_cast<const struct drm_panthor_dev_query *>(arg);
                LOG_TRACE("in struct drm_panthor_dev_query { type=%u, size=%u, pointer=0x%llx }",
                          q->type, q->size, static_cast<unsigned long long>(q->pointer));
                break;
            }
            case DRM_PANTHOR_VM_CREATE: {
                const auto *vc = static_cast<const struct drm_panthor_vm_create *>(arg);
                LOG_TRACE("in struct drm_panthor_vm_create { flags=0x%x, user_va_range=0x%llx }",
                          vc->flags, static_cast<unsigned long long>(vc->user_va_range));
                break;
            }
            case DRM_PANTHOR_VM_DESTROY: {
                const auto *vd = static_cast<const struct drm_panthor_vm_destroy *>(arg);
                LOG_TRACE("in struct drm_panthor_vm_destroy { id=%u }", vd->id);
                break;
            }
            case DRM_PANTHOR_VM_BIND: {
                const auto *vb = static_cast<const struct drm_panthor_vm_bind *>(arg);
                LOG_TRACE("in struct drm_panthor_vm_bind { vm_id=%u, flags=0x%x, ops={ count=%u, array=0x%llx } }",
                          vb->vm_id, vb->flags, vb->ops.count,
                          static_cast<unsigned long long>(vb->ops.array));
                if (vb->ops.array && vb->ops.count <= 16) {
                    const auto *ops = reinterpret_cast<const struct drm_panthor_vm_bind_op *>(
                        static_cast<uintptr_t>(vb->ops.array));
                    for (uint32_t i = 0; i < vb->ops.count; i++) {
                        LOG_TRACE("  in drm_panthor_vm_bind_op[%u] { flags=0x%x, bo_handle=%u, bo_offset=0x%llx, va=0x%llx, size=0x%llx }",
                                  i, ops[i].flags, ops[i].bo_handle,
                                  static_cast<unsigned long long>(ops[i].bo_offset),
                                  static_cast<unsigned long long>(ops[i].va),
                                  static_cast<unsigned long long>(ops[i].size));
                    }
                }
                break;
            }
            case DRM_PANTHOR_VM_GET_STATE: {
                const auto *vs = static_cast<const struct drm_panthor_vm_get_state *>(arg);
                LOG_TRACE("in struct drm_panthor_vm_get_state { vm_id=%u }", vs->vm_id);
                break;
            }
            case DRM_PANTHOR_BO_CREATE: {
                const auto *bc = static_cast<const struct drm_panthor_bo_create *>(arg);
                LOG_TRACE("in struct drm_panthor_bo_create { size=%llu, flags=0x%x, exclusive_vm_id=%u }",
                          static_cast<unsigned long long>(bc->size), bc->flags, bc->exclusive_vm_id);
                break;
            }
            case DRM_PANTHOR_BO_MMAP_OFFSET: {
                const auto *mo = static_cast<const struct drm_panthor_bo_mmap_offset *>(arg);
                LOG_TRACE("in struct drm_panthor_bo_mmap_offset { handle=%u }", mo->handle);
                break;
            }
            case DRM_PANTHOR_GROUP_CREATE: {
                const auto *gc = static_cast<const struct drm_panthor_group_create *>(arg);
                LOG_TRACE("in struct drm_panthor_group_create { queues.count=%u, max_compute=%u, max_fragment=%u, max_tiler=%u, priority=%u, compute_mask=0x%llx, fragment_mask=0x%llx, tiler_mask=0x%llx, vm_id=%u }",
                          gc->queues.count, gc->max_compute_cores, gc->max_fragment_cores,
                          gc->max_tiler_cores, gc->priority,
                          static_cast<unsigned long long>(gc->compute_core_mask),
                          static_cast<unsigned long long>(gc->fragment_core_mask),
                          static_cast<unsigned long long>(gc->tiler_core_mask),
                          gc->vm_id);
                break;
            }
            case DRM_PANTHOR_GROUP_DESTROY: {
                const auto *gd = static_cast<const struct drm_panthor_group_destroy *>(arg);
                LOG_TRACE("in struct drm_panthor_group_destroy { group_handle=%u }", gd->group_handle);
                break;
            }
            case DRM_PANTHOR_GROUP_SUBMIT: {
                const auto *gs = static_cast<const struct drm_panthor_group_submit *>(arg);
                LOG_TRACE("in struct drm_panthor_group_submit { group_handle=%u, queue_submits={ count=%u, array=0x%llx } }",
                          gs->group_handle, gs->queue_submits.count,
                          static_cast<unsigned long long>(gs->queue_submits.array));
                if (gs->queue_submits.array && gs->queue_submits.count <= 8) {
                    const auto *qs = reinterpret_cast<const struct drm_panthor_queue_submit *>(
                        static_cast<uintptr_t>(gs->queue_submits.array));
                    for (uint32_t i = 0; i < gs->queue_submits.count; i++) {
                        LOG_TRACE("  in drm_panthor_queue_submit[%u] { queue_index=%u, stream_size=%u, stream_addr=0x%llx, latest_flush=%u, syncs.count=%u }",
                                  i, qs[i].queue_index, qs[i].stream_size,
                                  static_cast<unsigned long long>(qs[i].stream_addr),
                                  qs[i].latest_flush, qs[i].syncs.count);
                    }
                }
                break;
            }
            case DRM_PANTHOR_GROUP_GET_STATE: {
                const auto *gs = static_cast<const struct drm_panthor_group_get_state *>(arg);
                LOG_TRACE("in struct drm_panthor_group_get_state { group_handle=%u }", gs->group_handle);
                break;
            }
            case DRM_PANTHOR_TILER_HEAP_CREATE: {
                const auto *hc = static_cast<const struct drm_panthor_tiler_heap_create *>(arg);
                LOG_TRACE("in struct drm_panthor_tiler_heap_create { vm_id=%u, initial_chunk_count=%u, chunk_size=%u, max_chunks=%u, target_in_flight=%u }",
                          hc->vm_id, hc->initial_chunk_count, hc->chunk_size, hc->max_chunks, hc->target_in_flight);
                break;
            }
            case DRM_PANTHOR_TILER_HEAP_DESTROY: {
                const auto *hd = static_cast<const struct drm_panthor_tiler_heap_destroy *>(arg);
                LOG_TRACE("in struct drm_panthor_tiler_heap_destroy { handle=%u }", hd->handle);
                break;
            }
            case DRM_PANTHOR_BO_SYNC: {
                const auto *bs = static_cast<const struct drm_panthor_bo_sync *>(arg);
                LOG_TRACE("in struct drm_panthor_bo_sync { ops.count=%u, ops.array=0x%llx }",
                          bs->ops.count, static_cast<unsigned long long>(bs->ops.array));
                break;
            }
            case DRM_PANTHOR_SET_USER_MMIO_OFFSET: {
                const auto *mo = static_cast<const struct drm_panthor_set_user_mmio_offset *>(arg);
                LOG_TRACE("in struct drm_panthor_set_user_mmio_offset { offset=0x%llx }",
                          static_cast<unsigned long long>(mo->offset));
                break;
            }
            case DRM_PANTHOR_BO_SET_LABEL: {
                const auto *bl = static_cast<const struct drm_panthor_bo_set_label *>(arg);
                LOG_TRACE("in struct drm_panthor_bo_set_label { handle=%u, label=\"%s\" }",
                          bl->handle,
                          bl->label ? reinterpret_cast<const char *>(static_cast<uintptr_t>(bl->label)) : "(null)");
                break;
            }
            case DRM_PANTHOR_BO_QUERY_INFO: {
                const auto *qi = static_cast<const struct drm_panthor_bo_query_info *>(arg);
                LOG_TRACE("in struct drm_panthor_bo_query_info { handle=%u }", qi->handle);
                break;
            }
            default:
                break;
            }
            return;
        }

        switch (nr) {
        case 0x09: {
            const auto *cl = static_cast<const struct drm_gem_close *>(arg);
            LOG_TRACE("in struct drm_gem_close { handle=%u }", cl->handle);
            break;
        }
        case 0x0c: {
            const uint64_t *cap = static_cast<const uint64_t *>(arg);
            LOG_TRACE("in struct drm_get_cap { capability=0x%llx }",
                      static_cast<unsigned long long>(cap[0]));
            break;
        }
        case 0x2d:
        case 0x2e: {
            const auto *ph = static_cast<const struct drm_prime_handle *>(arg);
            LOG_TRACE("in struct drm_prime_handle { handle=%u, flags=0x%x, fd=%d }",
                      ph->handle, ph->flags, ph->fd);
            break;
        }
        case 0xbf: {
            const auto *sc = static_cast<const struct drm_syncobj_create *>(arg);
            LOG_TRACE("in struct drm_syncobj_create { flags=0x%x }", sc->flags);
            break;
        }
        case 0xc0: {
            const auto *sd = static_cast<const struct drm_syncobj_destroy *>(arg);
            LOG_TRACE("in struct drm_syncobj_destroy { handle=%u }", sd->handle);
            break;
        }
        default:
            break;
        }
    }
}

extern "C" void panthor2kbase_log_ioctl_struct_out(unsigned long request, const void *arg, int ret, int err)
{
    if (!panthor2kbase_log_level_enabled(PANTHOR2KBASE_LOG_TRACE) || !arg)
        return;
    if (ret < 0) {
        LOG_TRACE("out error { ret=%d, errno=%d (%s) }", ret, err, strerror(err));
        return;
    }

    unsigned int type = _IOC_TYPE(request);
    unsigned int nr = _IOC_NR(request);
    unsigned int sz = _IOC_SIZE(request);

    if (type == KBASE_IOCTL_TYPE) {
        switch (nr) {
        case 0:
        case 52: {
            const auto *s = static_cast<const struct kbase_ioctl_version_check *>(arg);
            LOG_TRACE("out struct kbase_ioctl_version_check { major=%u, minor=%u }", s->major, s->minor);
            break;
        }
        case 3: {
            const auto *s = static_cast<const struct kbase_ioctl_get_gpuprops *>(arg);
            LOG_TRACE("out struct kbase_ioctl_get_gpuprops { ret_bytes=%d, size=%u }", ret, s->size);
            break;
        }
        case 5: {
            const auto *ma = static_cast<const union kbase_ioctl_mem_alloc *>(arg);
            LOG_TRACE("out union kbase_ioctl_mem_alloc.out { flags=0x%llx, gpu_va=0x%llx }",
                      static_cast<unsigned long long>(ma->out.flags),
                      static_cast<unsigned long long>(ma->out.gpu_va));
            break;
        }
        case 6: {
            const auto *mq = static_cast<const union kbase_ioctl_mem_query *>(arg);
            LOG_TRACE("out union kbase_ioctl_mem_query.out { value=0x%llx }",
                      static_cast<unsigned long long>(mq->out.value));
            break;
        }
        case 21: {
            const auto *al = static_cast<const union kbase_ioctl_mem_alias *>(arg);
            LOG_TRACE("out union kbase_ioctl_mem_alias.out { flags=0x%llx, gpu_va=0x%llx, va_pages=%llu }",
                      static_cast<unsigned long long>(al->out.flags),
                      static_cast<unsigned long long>(al->out.gpu_va),
                      static_cast<unsigned long long>(al->out.va_pages));
            break;
        }
        case 22: {
            const auto *im = static_cast<const union kbase_ioctl_mem_import *>(arg);
            LOG_TRACE("out union kbase_ioctl_mem_import.out { flags=0x%llx, gpu_va=0x%llx, va_pages=%llu }",
                      static_cast<unsigned long long>(im->out.flags),
                      static_cast<unsigned long long>(im->out.gpu_va),
                      static_cast<unsigned long long>(im->out.va_pages));
            break;
        }
        case 24: {
            const auto *sc = static_cast<const union kbase_ioctl_stream_create_u *>(arg);
            LOG_TRACE("out union kbase_ioctl_stream_create.out { ret_fd=%d, out.fd=%d }", ret, sc->out.fd);
            break;
        }
        case 39: {
            const auto *qb = static_cast<const union kbase_ioctl_cs_queue_bind *>(arg);
            LOG_TRACE("out union kbase_ioctl_cs_queue_bind.out { mmap_handle=0x%llx }",
                      static_cast<unsigned long long>(qb->out.mmap_handle));
            break;
        }
        case 42: {
            const auto *cg = static_cast<const union kbase_ioctl_cs_queue_group_create_1_6 *>(arg);
            LOG_TRACE("out union kbase_ioctl_cs_queue_group_create_1_6.out { group_handle=%u, group_uid=%u }",
                      cg->out.group_handle, cg->out.group_uid);
            break;
        }
        case 45: {
            const auto *kc = static_cast<const struct kbase_ioctl_kcpu_queue_new *>(arg);
            LOG_TRACE("out struct kbase_ioctl_kcpu_queue_new { id=%u }", kc->id);
            break;
        }
        case 48: {
            if (sz == sizeof(union kbase_ioctl_cs_tiler_heap_init_1_13)) {
                const auto *hi = static_cast<const union kbase_ioctl_cs_tiler_heap_init_1_13 *>(arg);
                LOG_TRACE("out union kbase_ioctl_cs_tiler_heap_init_1_13.out { gpu_heap_va=0x%llx, first_chunk_va=0x%llx }",
                          static_cast<unsigned long long>(hi->out.gpu_heap_va),
                          static_cast<unsigned long long>(hi->out.first_chunk_va));
            } else {
                const auto *hi = static_cast<const union kbase_ioctl_cs_tiler_heap_init *>(arg);
                LOG_TRACE("out union kbase_ioctl_cs_tiler_heap_init.out { gpu_heap_va=0x%llx, first_chunk_va=0x%llx }",
                          static_cast<unsigned long long>(hi->out.gpu_heap_va),
                          static_cast<unsigned long long>(hi->out.first_chunk_va));
            }
            break;
        }
        case 50: {
            const auto *ti = static_cast<const union kbase_ioctl_get_cpu_gpu_timeinfo *>(arg);
            LOG_TRACE("out union kbase_ioctl_get_cpu_gpu_timeinfo.out { sec=%llu, nsec=%u, timestamp=%llu, cycle_counter=%llu }",
                      static_cast<unsigned long long>(ti->out.sec), ti->out.nsec,
                      static_cast<unsigned long long>(ti->out.timestamp),
                      static_cast<unsigned long long>(ti->out.cycle_counter));
            break;
        }
        case 51: {
            const auto *gi = static_cast<const union kbase_ioctl_cs_get_glb_iface *>(arg);
            LOG_TRACE("out union kbase_ioctl_cs_get_glb_iface.out { glb_version=0x%x, features=0x%x, group_num=%u, total_stream_num=%u, instr_features=0x%x }",
                      gi->out.glb_version, gi->out.features, gi->out.group_num,
                      gi->out.total_stream_num, gi->out.instr_features);
            break;
        }
        case 54: {
            const auto *pc = static_cast<const struct kbase_ioctl_context_priority_check *>(arg);
            LOG_TRACE("out struct kbase_ioctl_context_priority_check { priority=%u }", pc->priority);
            break;
        }
        case 59: {
            const auto *ma = static_cast<const union kbase_ioctl_mem_alloc_ex *>(arg);
            LOG_TRACE("out union kbase_ioctl_mem_alloc_ex.out { flags=0x%llx, gpu_va=0x%llx }",
                      static_cast<unsigned long long>(ma->out.flags),
                      static_cast<unsigned long long>(ma->out.gpu_va));
            break;
        }
        default:
            break;
        }
        return;
    }

    if (type == 'd') {
        if (nr >= DRM_COMMAND_BASE && nr < DRM_COMMAND_BASE + 0x10) {
            switch (nr - DRM_COMMAND_BASE) {
            case DRM_PANTHOR_DEV_QUERY: {
                const auto *q = static_cast<const struct drm_panthor_dev_query *>(arg);
                LOG_TRACE("out struct drm_panthor_dev_query { type=%u, size=%u, pointer=0x%llx }",
                          q->type, q->size, static_cast<unsigned long long>(q->pointer));
                if (q->pointer) {
                    if (q->type == DRM_PANTHOR_DEV_QUERY_GPU_INFO && q->size >= sizeof(struct drm_panthor_gpu_info)) {
                        const auto *gi = reinterpret_cast<const struct drm_panthor_gpu_info *>(static_cast<uintptr_t>(q->pointer));
                        LOG_TRACE("  out struct drm_panthor_gpu_info { gpu_id=0x%x, gpu_rev=0x%x, csf_id=0x%x, l2_features=0x%x, tiler_features=0x%x, mem_features=0x%x, mmu_features=0x%x, thread_features=0x%x, max_threads=%u, max_wg=%u, shader_present=0x%llx, core_features=0x%x }",
                                  gi->gpu_id, gi->gpu_rev, gi->csf_id, gi->l2_features, gi->tiler_features,
                                  gi->mem_features, gi->mmu_features, gi->thread_features, gi->max_threads,
                                  gi->thread_max_workgroup_size,
                                  static_cast<unsigned long long>(gi->shader_present), gi->core_features);
                    } else if (q->type == DRM_PANTHOR_DEV_QUERY_CSIF_INFO && q->size >= sizeof(struct drm_panthor_csif_info)) {
                        const auto *ci = reinterpret_cast<const struct drm_panthor_csif_info *>(static_cast<uintptr_t>(q->pointer));
                        LOG_TRACE("  out struct drm_panthor_csif_info { csg_slot_count=%u, cs_slot_count=%u, cs_reg_count=%u, scoreboard_slot_count=%u, unpreserved_cs_reg_count=%u }",
                                  ci->csg_slot_count, ci->cs_slot_count, ci->cs_reg_count,
                                  ci->scoreboard_slot_count, ci->unpreserved_cs_reg_count);
                    } else if (q->type == DRM_PANTHOR_DEV_QUERY_MMU_INFO && q->size >= sizeof(struct drm_panthor_mmu_info)) {
                        const auto *mi = reinterpret_cast<const struct drm_panthor_mmu_info *>(static_cast<uintptr_t>(q->pointer));
                        LOG_TRACE("  out struct drm_panthor_mmu_info { page_size_bitmap=0x%llx }",
                                  static_cast<unsigned long long>(mi->page_size_bitmap));
                    } else if (q->type == DRM_PANTHOR_DEV_QUERY_TIMESTAMP_INFO && q->size >= sizeof(struct drm_panthor_timestamp_info)) {
                        const auto *ti = reinterpret_cast<const struct drm_panthor_timestamp_info *>(static_cast<uintptr_t>(q->pointer));
                        LOG_TRACE("  out struct drm_panthor_timestamp_info { timestamp_frequency=%llu, current_timestamp=%llu, cycle_count=%llu }",
                                  static_cast<unsigned long long>(ti->timestamp_frequency),
                                  static_cast<unsigned long long>(ti->current_timestamp),
                                  static_cast<unsigned long long>(ti->cycle_count));
                    } else if (q->type == DRM_PANTHOR_DEV_QUERY_GROUP_PRIORITIES_INFO && q->size >= sizeof(struct drm_panthor_group_priorities_info)) {
                        const auto *pi = reinterpret_cast<const struct drm_panthor_group_priorities_info *>(static_cast<uintptr_t>(q->pointer));
                        LOG_TRACE("  out struct drm_panthor_group_priorities_info { allowed_mask=0x%x }", pi->allowed_mask);
                    }
                }
                break;
            }
            case DRM_PANTHOR_VM_CREATE: {
                const auto *vc = static_cast<const struct drm_panthor_vm_create *>(arg);
                LOG_TRACE("out struct drm_panthor_vm_create { id=%u, user_va_range=0x%llx }",
                          vc->id, static_cast<unsigned long long>(vc->user_va_range));
                break;
            }
            case DRM_PANTHOR_VM_BIND: {
                const auto *vb = static_cast<const struct drm_panthor_vm_bind *>(arg);
                if (vb->ops.array && vb->ops.count <= 16) {
                    const auto *ops = reinterpret_cast<const struct drm_panthor_vm_bind_op *>(
                        static_cast<uintptr_t>(vb->ops.array));
                    for (uint32_t i = 0; i < vb->ops.count; i++) {
                        LOG_TRACE("  out drm_panthor_vm_bind_op[%u] { flags=0x%x, bo_handle=%u, va=0x%llx, size=0x%llx }",
                                  i, ops[i].flags, ops[i].bo_handle,
                                  static_cast<unsigned long long>(ops[i].va),
                                  static_cast<unsigned long long>(ops[i].size));
                    }
                }
                break;
            }
            case DRM_PANTHOR_VM_GET_STATE: {
                const auto *vs = static_cast<const struct drm_panthor_vm_get_state *>(arg);
                LOG_TRACE("out struct drm_panthor_vm_get_state { vm_id=%u, state=%u }", vs->vm_id, vs->state);
                break;
            }
            case DRM_PANTHOR_BO_CREATE: {
                const auto *bc = static_cast<const struct drm_panthor_bo_create *>(arg);
                LOG_TRACE("out struct drm_panthor_bo_create { size=%llu, handle=%u }",
                          static_cast<unsigned long long>(bc->size), bc->handle);
                break;
            }
            case DRM_PANTHOR_BO_MMAP_OFFSET: {
                const auto *mo = static_cast<const struct drm_panthor_bo_mmap_offset *>(arg);
                LOG_TRACE("out struct drm_panthor_bo_mmap_offset { handle=%u, offset=0x%llx }",
                          mo->handle, static_cast<unsigned long long>(mo->offset));
                break;
            }
            case DRM_PANTHOR_GROUP_CREATE: {
                const auto *gc = static_cast<const struct drm_panthor_group_create *>(arg);
                LOG_TRACE("out struct drm_panthor_group_create { group_handle=%u }", gc->group_handle);
                break;
            }
            case DRM_PANTHOR_GROUP_GET_STATE: {
                const auto *gs = static_cast<const struct drm_panthor_group_get_state *>(arg);
                LOG_TRACE("out struct drm_panthor_group_get_state { group_handle=%u, state=0x%x, fatal_queues=0x%x }",
                          gs->group_handle, gs->state, gs->fatal_queues);
                break;
            }
            case DRM_PANTHOR_TILER_HEAP_CREATE: {
                const auto *hc = static_cast<const struct drm_panthor_tiler_heap_create *>(arg);
                LOG_TRACE("out struct drm_panthor_tiler_heap_create { handle=%u, tiler_heap_ctx_gpu_va=0x%llx, first_heap_chunk_gpu_va=0x%llx }",
                          hc->handle,
                          static_cast<unsigned long long>(hc->tiler_heap_ctx_gpu_va),
                          static_cast<unsigned long long>(hc->first_heap_chunk_gpu_va));
                break;
            }
            case DRM_PANTHOR_BO_QUERY_INFO: {
                const auto *qi = static_cast<const struct drm_panthor_bo_query_info *>(arg);
                LOG_TRACE("out struct drm_panthor_bo_query_info { handle=%u, extra_flags=0x%x, create_flags=0x%x }",
                          qi->handle, qi->extra_flags, qi->create_flags);
                break;
            }
            default:
                break;
            }
            return;
        }

        switch (nr) {
        case 0x0c: {
            const uint64_t *cap = static_cast<const uint64_t *>(arg);
            LOG_TRACE("out struct drm_get_cap { capability=0x%llx, value=0x%llx }",
                      static_cast<unsigned long long>(cap[0]),
                      static_cast<unsigned long long>(cap[1]));
            break;
        }
        case 0x2d:
        case 0x2e: {
            const auto *ph = static_cast<const struct drm_prime_handle *>(arg);
            LOG_TRACE("out struct drm_prime_handle { handle=%u, flags=0x%x, fd=%d }",
                      ph->handle, ph->flags, ph->fd);
            break;
        }
        case 0xbf: {
            const auto *sc = static_cast<const struct drm_syncobj_create *>(arg);
            LOG_TRACE("out struct drm_syncobj_create { handle=%u, flags=0x%x }", sc->handle, sc->flags);
            break;
        }
        default:
            break;
        }
    }
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

    panthor2kbase_log_ioctl_struct_in(request, arg);

    errno = 0;
    int ret = real_ioctl_fn(fd, request, arg);
    int saved_errno = errno;

    panthor2kbase_log_ioctl_struct_out(request, arg, ret, saved_errno);

    if (ret < 0) {
        LOG_TRACE("IOCTL %s -> ret=%d errno=%d (%s)",
                  call_text, ret, saved_errno, strerror(saved_errno));
    } else {
        LOG_TRACE("IOCTL %s -> ret=%d (OK)", call_text, ret);
    }

    errno = saved_errno;
    return ret;
}
