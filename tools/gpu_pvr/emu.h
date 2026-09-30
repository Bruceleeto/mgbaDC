/* SPDX-License-Identifier: GPL-2.0-only */
/* Host stand-in for bloom's emu.h: what pvr.c uses of it. */
#ifndef __BLOOM_EMU_H
#define __BLOOM_EMU_H
#include <sys/cdefs.h>
#include <stdint.h>
__BEGIN_DECLS
#include "bloom-config.h"
#undef likely
#undef unlikely
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define SCREEN_WIDTH	((WITH_480P ? 640 : 320) << WITH_FSAA)
#define SCREEN_HEIGHT	(WITH_480P ? 480 : 240)
extern _Bool started;
extern unsigned int screen_bpp;
/* Copy 32 bytes from src to dst. Both must be aligned to 32 bytes. */
void copy32(void *dst, const void *src);
/* bloom's gpulib gpu.h declares this; pcsx_rearmed's does not. */
int do_cmd_list(uint32_t *list, int list_len, int *cycles_sum_out,
		int *cycles_last, int *last_cmd);
__END_DECLS
#endif
