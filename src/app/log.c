#define _POSIX_C_SOURCE 200809L

/*
 * log.c
 *
 * See log.h. The ring buffer is the only state: no allocation, no
 * file I/O, no timestamps beyond two clock reads per message.
 */
#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint64_t seq;
    uint64_t uptime_ms;
    uint64_t wall_ms;
    LogLevel level;
    char tag[LOG_TAG_MAX];
    char message[LOG_MESSAGE_MAX];
} LogEntry;

static LogEntry g_ring[LOG_RING_CAPACITY];
static size_t g_head;                 /* next slot to write */
static size_t g_count;                /* valid entries, up to capacity */
static uint64_t g_seq;
static uint64_t g_level_counts[4];
static uint64_t g_total;
static LogLevel g_console_level = LOG_LEVEL_INFO;
static uint64_t g_start_ms;
static int g_started;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t monotonic_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

static uint64_t wall_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);

    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

const char *log_level_name(LogLevel level)
{
    switch (level) {
    case LOG_LEVEL_ERROR: return "error";
    case LOG_LEVEL_WARN:  return "warn";
    case LOG_LEVEL_INFO:  return "info";
    default:              return "debug";
    }
}

static const char *level_tag(LogLevel level)
{
    switch (level) {
    case LOG_LEVEL_ERROR: return "ERROR";
    case LOG_LEVEL_WARN:  return "WARN ";
    case LOG_LEVEL_INFO:  return "INFO ";
    default:              return "DEBUG";
    }
}

void log_init(LogLevel console_level)
{
    pthread_mutex_lock(&g_lock);

    if (!g_started) {
        g_start_ms = monotonic_ms();
        g_started = 1;
    }

    g_console_level = console_level;

    pthread_mutex_unlock(&g_lock);
}

void log_set_level(LogLevel console_level)
{
    pthread_mutex_lock(&g_lock);
    g_console_level = console_level;
    pthread_mutex_unlock(&g_lock);
}

LogLevel log_get_level(void)
{
    LogLevel level;

    pthread_mutex_lock(&g_lock);
    level = g_console_level;
    pthread_mutex_unlock(&g_lock);

    return level;
}

void log_write(LogLevel level,
               const char *subsystem,
               const char *format,
               ...)
{
    char message[LOG_MESSAGE_MAX];
    va_list args;

    if (level > LOG_LEVEL_DEBUG) {
        level = LOG_LEVEL_DEBUG;
    }

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    uint64_t now_uptime = monotonic_ms();

    pthread_mutex_lock(&g_lock);

    uint64_t elapsed = g_started && now_uptime >= g_start_ms ?
        now_uptime - g_start_ms : 0;

    LogEntry *entry = &g_ring[g_head];

    entry->seq = ++g_seq;
    entry->uptime_ms = elapsed;
    entry->wall_ms = wall_ms();
    entry->level = level;

    snprintf(entry->tag, sizeof(entry->tag), "%s",
             subsystem != NULL ? subsystem : "-");
    memcpy(entry->message, message, sizeof(entry->message));
    entry->message[sizeof(entry->message) - 1] = 0;

    g_head = (g_head + 1) % LOG_RING_CAPACITY;

    if (g_count < LOG_RING_CAPACITY) {
        g_count++;
    }

    g_level_counts[level]++;
    g_total++;

    int console = level <= g_console_level;

    pthread_mutex_unlock(&g_lock);

    if (console) {
        FILE *stream = level <= LOG_LEVEL_WARN ? stderr : stdout;

        fprintf(stream, "[%5llu.%03llu] %s %s: %s\n",
                (unsigned long long) (elapsed / 1000),
                (unsigned long long) (elapsed % 1000),
                level_tag(level),
                subsystem != NULL ? subsystem : "-",
                message);
        fflush(stream);
    }
}

size_t log_recent_json(char *out,
                       size_t out_size,
                       unsigned limit,
                       LogLevel min_level)
{
    if (out == NULL || out_size < 3) {
        return 0;
    }

    pthread_mutex_lock(&g_lock);

    size_t written = 0;
    size_t start = 0;

    /*
     * Walk oldest to newest. When the ring wrapped, the oldest entry
     * sits at g_head; otherwise the history starts at slot 0.
     */
    if (g_count == LOG_RING_CAPACITY) {
        start = g_head;
    }

    size_t first = 0;

    if (limit > 0 && g_count > limit) {
        first = g_count - limit;
    }

    out_size -= 1;      /* reserve room for the terminating NUL */
    out[written++] = '[';

    size_t emitted = 0;

    for (size_t i = first; i < g_count; i++) {
        const LogEntry *entry = &g_ring[(start + i) % LOG_RING_CAPACITY];

        if (entry->level > min_level) {
            continue;
        }

        char escaped[LOG_MESSAGE_MAX];
        size_t e = 0;

        for (const char *p = entry->message; *p != 0 && e + 2 < sizeof(escaped); p++) {
            unsigned char c = (unsigned char) *p;

            if (c == '"' || c == '\\') {
                escaped[e++] = '\\';
                escaped[e++] = (char) c;
            } else if (c == '\n') {
                escaped[e++] = '\\';
                escaped[e++] = 'n';
            } else if (c == '\r') {
                escaped[e++] = '\\';
                escaped[e++] = 'r';
            } else if (c == '\t') {
                escaped[e++] = '\\';
                escaped[e++] = 't';
            } else if (c < 0x20) {
                escaped[e++] = ' ';
            } else {
                escaped[e++] = (char) c;
            }
        }
        escaped[e] = 0;

        char line[512];

        int n = snprintf(line, sizeof(line),
                         "%s{\"seq\":%llu,\"uptime_ms\":%llu,\"wall_ms\":%llu,"
                         "\"level\":\"%s\",\"tag\":\"%s\",\"msg\":\"%s\"}",
                         emitted ? "," : "",
                         (unsigned long long) entry->seq,
                         (unsigned long long) entry->uptime_ms,
                         (unsigned long long) entry->wall_ms,
                         log_level_name(entry->level),
                         entry->tag,
                         escaped);

        if (n < 0 || written + (size_t) n > out_size) {
            break;
        }

        memcpy(out + written, line, (size_t) n);
        written += (size_t) n;
        emitted++;
    }

    out[written++] = ']';
    out[written] = 0;

    pthread_mutex_unlock(&g_lock);

    return written;
}

uint64_t log_level_count(LogLevel level)
{
    uint64_t count;

    if (level > LOG_LEVEL_DEBUG) {
        return 0;
    }

    pthread_mutex_lock(&g_lock);
    count = g_level_counts[level];
    pthread_mutex_unlock(&g_lock);

    return count;
}

uint64_t log_total_count(void)
{
    uint64_t count;

    pthread_mutex_lock(&g_lock);
    count = g_total;
    pthread_mutex_unlock(&g_lock);

    return count;
}
