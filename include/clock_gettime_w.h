#ifndef _CLOCK_GETTIME_W_H_
#define _CLOCK_GETTIME_W_H_

#include <sys/time.h>
#include <time.h>

#ifdef __APPLE__
#include <mach/clock.h>
#include <mach/mach.h>

typedef int clockid_t;

#define CLOCK_REALTIME CALENDAR_CLOCK
#define CLOCK_MONOTONIC SYSTEM_CLOCK

#endif

#ifdef __cplusplus
extern "C" {
#endif

int clock_gettime_w(clockid_t clk_id, struct timespec *tp);

#ifdef __cplusplus
}
#endif

#endif
