/*
 * log.h
 *
 * Process wide logging with a bounded in-memory history.
 *
 * Two consumers:
 *   - the console, for the operator watching the terminal
 *   - GET /api/logs, which serves the recent history as JSON so a
 *     browser can show "recent errors" without shell access
 *
 * The history is a fixed size ring, so a burst of errors can never
 * grow the process. Every write is serialized by one mutex; callers may
 * log from any thread. Levels exist so the media hot path stays quiet:
 * a per frame or per packet event is DEBUG and off unless asked for.
 */
#ifndef APP_LOG_H
#define APP_LOG_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN = 1,
    LOG_LEVEL_INFO = 2,
    LOG_LEVEL_DEBUG = 3
} LogLevel;

#define LOG_RING_CAPACITY 256
#define LOG_MESSAGE_MAX 192
#define LOG_TAG_MAX 12

/*
 * Configure the console threshold. DEBUG is only ever printed when
 * explicitly selected (--verbose). Safe to call more than once.
 */
void log_init(LogLevel console_level);

void log_set_level(LogLevel console_level);

LogLevel log_get_level(void);

/*
 * Emit one message. 'subsystem' is a short fixed tag ("rtc", "v4l2",
 * "dtls", "http"). Messages above LOG_MESSAGE_MAX are truncated, never
 * dropped: a truncated error still names the subsystem and the level.
 */
void log_write(LogLevel level,
               const char *subsystem,
               const char *format,
               ...) __attribute__((format(printf, 3, 4)));

#define log_error(tag, ...) log_write(LOG_LEVEL_ERROR, tag, __VA_ARGS__)
#define log_warn(tag, ...)  log_write(LOG_LEVEL_WARN, tag, __VA_ARGS__)
#define log_info(tag, ...)  log_write(LOG_LEVEL_INFO, tag, __VA_ARGS__)
#define log_debug(tag, ...) log_write(LOG_LEVEL_DEBUG, tag, __VA_ARGS__)

/*
 * Write the newest entries with level >= min_level as a JSON array
 * (oldest first) into out. 'limit' 0 means "every entry in the ring".
 * Returns the number of bytes written, always NUL terminated.
 */
size_t log_recent_json(char *out,
                       size_t out_size,
                       unsigned limit,
                       LogLevel min_level);

/* Number of messages emitted at exactly this level since startup. */
uint64_t log_level_count(LogLevel level);

uint64_t log_total_count(void);

const char *log_level_name(LogLevel level);

#endif
