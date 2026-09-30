/* Host stand-in for <kos.h>, on top of bloom's KOS shim (rearmed_bloom's
 * plugins/gpu_pvr/kos): only what src/platform/dreamcast/pvr-gba.c uses. */
#ifndef PVRTEST_KOS_H
#define PVRTEST_KOS_H
#include <stdint.h>
#include <time.h>
#include <arch/timer.h>
#include <dc/pvr.h>
#include <dc/sq.h>

#ifndef PVR_PT_ALPHA_REF
#define PVR_PT_ALPHA_REF 0x011c
#endif

static inline uint64_t timer_us_gettime64(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
#endif
