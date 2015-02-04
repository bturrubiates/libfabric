#ifndef _MACH_CLOCK_GETTIME_H_
#define _MACH_CLOCK_GETTIME_H_

#include <sys/time.h>
#include <time.h>

#include <mach/clock.h>
#include <mach/mach.h>

typedef int clockid_t;

#define CLOCK_REALTIME CALENDAR_CLOCK
#define CLOCK_MONOTONIC SYSTEM_CLOCK

#ifdef __cplusplus
extern "C" {
#endif

int mach_clock_gettime(clockid_t clk_id, struct timespec *tp);

#ifdef __cplusplus
}
#endif

#endif
