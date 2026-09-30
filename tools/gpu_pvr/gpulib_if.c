/*
 * Host-side shim for bloom's PVR renderer.
 *
 * pvr.c and platform.c are bloom's files, verbatim.  Everything bloom expects
 * from the hardware and from KOS lives in kos_pvr.cpp + libpvr; this file is
 * only what neither side supplies on Linux:
 *
 *  - bringing libpvr up before gpulib can reach the TA (renderer_init)
 *  - keeping bloom's renderer_init off gpu.vram (the aliased -D allocator)
 *  - scanout: on the DC the PVR scans the framebuffer out itself, here the
 *    front buffer has to be handed to pcsx's plugin_lib for the SDL window.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../gpulib/gpu.h"
#include "../../frontend/plugin_lib.h"
#include "pvr.h"
#include "kos_pvr.h"

/* bloom's emu.h: set by its main() on the DC, nothing sets it here. */
_Bool started;
_Bool bloom_pvr_bound;          /* platform.c's, PVR-bound frame; never here */

/* platform.c's, for the trace line below. */
extern unsigned int screen_bpp;
extern float screen_fw, screen_fh;

/* pvr.c's renderer_init/finish are compiled under these names; the real ones
 * below bring libpvr up first, at GPUinit, before anything reaches the TA. */
int pvr_real_renderer_init(void);
void pvr_real_renderer_finish(void);

/* gpu.vram is mapped by gpu.c (map_vram, so the mirror past 1 MB exists).
 * pvr.c's renderer_init/finish are compiled with aligned_alloc and free
 * pointed here so they leave that alone. */
void *pvr_glue_vram_alloc(size_t align, size_t size)
{
	(void)align; (void)size;
	return NULL;
}

void pvr_glue_vram_free(void *p)
{
	(void)p;
}

/* --- scanout ------------------------------------------------------------- */

#define OUT_W 640
#define OUT_H 480
#define OUT_STRIDE 2048          /* plugin_lib assumes 2048-byte lines in 24bpp */

static uint8_t *out_rgb;
static int inited;
static int sdl_open;
static unsigned long frame_no;

/* Called from kos_pvr.cpp once libpvr has finished a render, which is where
 * the DC's scanout would pick the new front buffer up. */
void kos_pvr_present(void)
{
	int got;

	if (!inited)
		return;

	if (!sdl_open) {
		pl_rearmed_cbs.pl_vout_open();
		pl_rearmed_cbs.pl_vout_set_mode(OUT_W, OUT_H, OUT_W, OUT_H, 24);
		sdl_open = 1;
	}

	got = out_rgb && kos_pvr_read_front(out_rgb, OUT_STRIDE, OUT_W, OUT_H);

	fprintf(stderr, "pvr frame %lu: %dx%d %ubpp scale %.2fx%.2f front=%s\n",
		frame_no++, gpu.screen.hres, gpu.screen.vres, screen_bpp,
		screen_fw, screen_fh, got ? "ok" : "EMPTY");

	if (got)
		pl_rearmed_cbs.pl_vout_flip(out_rgb, 0, 1, 0, 0, OUT_W, OUT_H, 0);
}

int renderer_init(void)
{
	if (!inited) {
		if (kos_pvr_init()) {
			fprintf(stderr, "gpu_pvr: libpvr init failed\n");
			return -1;
		}
		out_rgb = calloc(OUT_STRIDE, OUT_H);
		pvr_real_renderer_init();
		pvr_renderer_init();
		inited = 1;
		started = 1;
	}
	return 0;
}

void renderer_finish(void)
{
	if (inited) {
		started = 0;
		inited = 0;
		pvr_renderer_shutdown();
		pvr_real_renderer_finish();
		kos_pvr_shutdown();
	}
	if (sdl_open) {
		pl_rearmed_cbs.pl_vout_close();
		sdl_open = 0;
	}
	free(out_rgb);
	out_rgb = NULL;
}

/* bloom's pvr.c does not define this; its gpulib never reaches the call. */
void renderer_set_interlace(int enable, int is_odd)
{
	(void)enable; (void)is_odd;
}
