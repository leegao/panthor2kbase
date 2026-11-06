/*
 * SPDX-License-Identifier: MIT
 *
 * Panthor2Kbase runtime logging.
 *
 * The minimum log level is controlled by the PANTHOR2KBASE_LOG_LEVEL
 * environment variable. Accepted values are the integer 0..4 or the
 * (case-insensitive) names error, warn, info, debug, trace. When the
 * variable is unset the default level is WARN.
 *
 * Log lines go to stderr by default, or to the file named by
 * PANTHOR2KBASE_LOG_PATH when that variable is set.
 */

#ifndef PANTHOR2KBASE_LOG_H
#define PANTHOR2KBASE_LOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

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

/* Current effective log level (initialized on first use). */
extern panthor2kbase_log_level_t panthor2kbase_log_level;

/* Core printf-style loggers. */
void panthor2kbase_logv(panthor2kbase_log_level_t level, const char *fmt, va_list ap);
void panthor2kbase_log(panthor2kbase_log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Convenience macros. Disabled levels still pay the cost of a level check
 * plus panthor2kbase_log_init() on first use; use panthor2kbase_log_level_enabled()
 * to guard expensive argument construction. */
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

/* Decode a drm/kbase ioctl request into a human readable name for logging. */
const char *panthor2kbase_ioctl_req_name(unsigned long request);

/*
 * Wrapper around the real ioctl that logs the call at trace level, including
 * the call text and a pretty rendering of the parameters. call_text is
 * typically a stringified expression like "KBASE_IOCTL_MEM_ALLOC(&ma)".
 *
 * The macro captures the call text via stringification so call sites stay
 * concise: adapter_ioctl(fd, KBASE_IOCTL_MEM_ALLOC, &ma)
 */
int panthor2kbase_adapter_ioctl(int fd, unsigned long request, void *arg, const char *call_text);

#define adapter_ioctl(fd, req, arg) \
    panthor2kbase_adapter_ioctl(fd, req, arg, #req "(" #arg ")")

#endif /* PANTHOR2KBASE_LOG_H */
