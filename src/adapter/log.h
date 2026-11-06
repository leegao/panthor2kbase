/*
 * SPDX-License-Identifier: MIT
 *
 * Panthor2Kbase runtime logging with C++ RAII scoped indentation.
 *
 * The minimum log level is controlled by the PANTHOR2KBASE_LOG_LEVEL
 * environment variable. Accepted values are the integer 0..4 or the
 * (case-insensitive) names error, warn, info, debug, trace.
 * When the variable is unset, the default level is TRACE (4).
 *
 * Every time an interceptor or adapter function is entered, constructing
 * a `panthor2kbase::ScopedLogIndent` (via `PANTHOR2KBASE_LOG_SCOPE(...)`)
 * increments the thread-local indentation depth by 1 on entry and
 * automatically decrements it by 1 on scope exit via RAII destruction.
 */

#ifndef PANTHOR2KBASE_LOG_H
#define PANTHOR2KBASE_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PANTHOR2KBASE_LOG_ERROR = 0,
    PANTHOR2KBASE_LOG_WARN  = 1,
    PANTHOR2KBASE_LOG_INFO  = 2,
    PANTHOR2KBASE_LOG_DEBUG = 3,
    PANTHOR2KBASE_LOG_TRACE = 4,
    PANTHOR2KBASE_LOG_LEVEL_COUNT = 5,
} panthor2kbase_log_level_t;

/* Lazily initialize the log level/sink from the environment (thread-safe). */
void panthor2kbase_log_init(void);

/* Current effective log level (initialized on first use; default TRACE). */
extern panthor2kbase_log_level_t panthor2kbase_log_level;

/* Thread-local indentation depth accessors (used by C++ RAII ScopedLogIndent). */
int panthor2kbase_log_get_indent(void);
void panthor2kbase_log_inc_indent(void);
void panthor2kbase_log_dec_indent(void);

/* Core printf-style loggers. */
void panthor2kbase_logv(panthor2kbase_log_level_t level, const char *fmt, va_list ap);
void panthor2kbase_log(panthor2kbase_log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define LOG_ERROR(...) panthor2kbase_log(PANTHOR2KBASE_LOG_ERROR, __VA_ARGS__)
#define LOG_WARN(...)  panthor2kbase_log(PANTHOR2KBASE_LOG_WARN,  __VA_ARGS__)
#define LOG_INFO(...)  panthor2kbase_log(PANTHOR2KBASE_LOG_INFO,  __VA_ARGS__)
#define LOG_DEBUG(...) panthor2kbase_log(PANTHOR2KBASE_LOG_DEBUG, __VA_ARGS__)
#define LOG_TRACE(...) panthor2kbase_log(PANTHOR2KBASE_LOG_TRACE, __VA_ARGS__)

static inline bool panthor2kbase_log_level_enabled(panthor2kbase_log_level_t level)
{
    panthor2kbase_log_init();
    return level <= panthor2kbase_log_level;
}

/* Decode a drm/kbase ioctl request into a human-readable name for logging. */
const char *panthor2kbase_ioctl_req_name(unsigned long request);

/*
 * Wrapper around the real ioctl that logs the call at trace level, including
 * the call text and a detailed rendering of the parameters and result.
 */
int panthor2kbase_adapter_ioctl(int fd, unsigned long request, void *arg, const char *call_text);

#define adapter_ioctl(fd, req, arg) \
    panthor2kbase_adapter_ioctl(fd, req, arg, #req "(" #arg ")")

#ifdef __cplusplus
} /* extern "C" */

namespace panthor2kbase {

/*
 * RAII Scoped Indentation Guard:
 *  - Logs entry at TRACE level at current indentation depth, then increments
 *    thread-local indentation depth by +1 for all inner logs/calls.
 *  - Automatically decrements indentation depth by -1 on scope exit (~ScopedLogIndent).
 */
class ScopedLogIndent {
public:
    explicit ScopedLogIndent(const char *func_name) noexcept
        : func_name_(func_name)
    {
        if (panthor2kbase_log_level_enabled(PANTHOR2KBASE_LOG_TRACE)) {
            panthor2kbase_log(PANTHOR2KBASE_LOG_TRACE, "-> %s {", func_name_);
        }
        panthor2kbase_log_inc_indent();
    }

    ScopedLogIndent(const char *func_name, const char *fmt, ...) noexcept
        __attribute__((format(printf, 3, 4)))
        : func_name_(func_name)
    {
        if (panthor2kbase_log_level_enabled(PANTHOR2KBASE_LOG_TRACE)) {
            char detail[256];
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(detail, sizeof(detail), fmt, ap);
            va_end(ap);
            panthor2kbase_log(PANTHOR2KBASE_LOG_TRACE, "-> %s(%s) {", func_name_, detail);
        }
        panthor2kbase_log_inc_indent();
    }

    ~ScopedLogIndent() noexcept
    {
        panthor2kbase_log_dec_indent();
        if (panthor2kbase_log_level_enabled(PANTHOR2KBASE_LOG_TRACE)) {
            panthor2kbase_log(PANTHOR2KBASE_LOG_TRACE, "} // %s", func_name_);
        }
    }

    ScopedLogIndent(const ScopedLogIndent &) = delete;
    ScopedLogIndent &operator=(const ScopedLogIndent &) = delete;

private:
    const char *func_name_;
};

} /* namespace panthor2kbase */

#define PANTHOR2KBASE_CONCAT_INNER(a, b) a##b
#define PANTHOR2KBASE_CONCAT(a, b) PANTHOR2KBASE_CONCAT_INNER(a, b)

#define PANTHOR2KBASE_LOG_SCOPE(func_name, ...) \
    ::panthor2kbase::ScopedLogIndent PANTHOR2KBASE_CONCAT(_p2k_log_scope_, __LINE__)(func_name, ##__VA_ARGS__)

#endif /* __cplusplus */

#endif /* PANTHOR2KBASE_LOG_H */
