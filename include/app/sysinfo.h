/*
 * sysinfo.h
 *
 * Process resource sampling for the diagnostics API: resident memory
 * and CPU time. Linux only, no libprocps dependency, no per-call
 * allocation.
 *
 * Sampling state is kept in one static struct, so call
 * sysinfo_cpu_percent() from a single thread (the main loop). It
 * returns the average CPU usage since the previous call.
 */
#ifndef APP_SYSINFO_H
#define APP_SYSINFO_H

#include <stddef.h>
#include <stdint.h>

/* Resident set size in kibibytes, 0 when /proc is unavailable. */
uint64_t sysinfo_rss_kb(void);

/*
 * Process CPU usage (all threads, all cores) as a percentage of one
 * core, averaged over the interval since the previous call. The first
 * call returns 0.0 because it only establishes the baseline.
 */
double sysinfo_cpu_percent(void);

#endif
