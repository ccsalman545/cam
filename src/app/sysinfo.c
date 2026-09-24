#define _POSIX_C_SOURCE 200809L

/*
 * sysinfo.c
 *
 * /proc/self/stat fields 14 and 15 hold user and system CPU time in
 * clock ticks; /proc/self/statm field 2 holds resident pages.
 */
#include "sysinfo.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint64_t sysinfo_rss_kb(void)
{
    FILE *file = fopen("/proc/self/statm", "r");

    if (file == NULL) {
        return 0;
    }

    unsigned long size_pages = 0;
    unsigned long resident_pages = 0;

    int matched = fscanf(file, "%lu %lu", &size_pages, &resident_pages);

    fclose(file);

    if (matched != 2) {
        return 0;
    }

    long page_size = sysconf(_SC_PAGESIZE);

    if (page_size <= 0) {
        return 0;
    }

    return (uint64_t) resident_pages * (uint64_t) page_size / 1024ULL;
}

static uint64_t cpu_ticks(void)
{
    FILE *file = fopen("/proc/self/stat", "r");

    if (file == NULL) {
        return 0;
    }

    /*
     * The executable name can contain spaces and parentheses, so skip
     * past the last ')' before reading field 3 onwards.
     */
    char line[1024];

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return 0;
    }

    fclose(file);

    char *close_paren = strrchr(line, ')');

    if (close_paren == NULL) {
        return 0;
    }

    unsigned long user = 0;
    unsigned long system = 0;

    /* After ")" comes field 3 (state); utime/stime are fields 14/15. */
    if (sscanf(close_paren + 2,
               "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
               &user, &system) != 2) {
        return 0;
    }

    return (uint64_t) user + (uint64_t) system;
}

double sysinfo_cpu_percent(void)
{
    static uint64_t last_ticks;
    static uint64_t last_ms;
    static int have_baseline;

    uint64_t ticks = cpu_ticks();

    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    uint64_t now_ms = (uint64_t) ts.tv_sec * 1000ULL +
                      (uint64_t) ts.tv_nsec / 1000000ULL;

    if (!have_baseline) {
        last_ticks = ticks;
        last_ms = now_ms;
        have_baseline = 1;
        return 0.0;
    }

    if (ticks < last_ticks || now_ms <= last_ms) {
        last_ticks = ticks;
        last_ms = now_ms;
        return 0.0;
    }

    long hz = sysconf(_SC_CLK_TCK);

    if (hz <= 0) {
        hz = 100;
    }

    uint64_t ticks_delta = ticks - last_ticks;
    uint64_t ms_delta = now_ms - last_ms;

    last_ticks = ticks;
    last_ms = now_ms;

    return (double) ticks_delta * 1000.0 / (double) hz /
           (double) ms_delta * 100.0;
}
