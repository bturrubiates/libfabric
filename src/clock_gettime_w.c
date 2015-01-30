#include "clock_gettime_w.h"

int clock_gettime_w(clockid_t clk_id, struct timespec *tp) {
	int retval;

	clock_serv_t cclock;
	mach_timespec_t mts;

	host_get_clock_service(mach_host_self(), clk_id, &cclock);
	retval = clock_get_time(cclock, &mts);
	mach_port_deallocate(mach_task_self(), cclock);

	tp->tv_sec = mts.tv_sec;
	tp->tv_nsec = mts.tv_nsec;

	return retval;
}

