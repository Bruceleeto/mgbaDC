/* WHERE A FRAME GOES, IN A COUPLE OF PRINTFS.
 *
 * Not a profiler.  A profiler says which function; this says which STAGE, and
 * that is the question a recompiler rewrite actually needs answered -- whether
 * a change moved time out of emulated code, or moved it nowhere and only moved
 * it around inside the same bucket.
 *
 * Wall clock, microseconds, accumulated per bucket and reported over the same
 * one-second window `dc_vout_flip` already keeps for the fps counter, so the
 * buckets and the frame rate are measured over exactly the same interval and
 * belong in the same sentence.
 *
 * THE BUCKETS NEST, AND THE REPORT SAYS SO.  Only three are siblings:
 *
 *      frame  =  cpu  +  evt  +  flip  +  other
 *
 * Everything else is a CHILD of one of those and is reported indented, never
 * added in.  Inside `cpu` are the four doors out of generated code -- `hw`,
 * `rw`, `cop`, `svc` -- and what `cpu` has left after them is emitted
 * instructions and nothing else.  `gpu` is `do_cmd_list`, reached from a DMA
 * register write, so it is inside `hw` and not beside it.  Inside `evt` are
 * the fifteen interrupt sources, and inside the one of those that crosses
 * vblank are `emuupd`, `lace`, `spu` and `vbl`.  `jit` is the odd one: real
 * work, but on the recompiler's own thread, so it overlaps everything and is
 * a child of nothing.
 *
 * An early version of this printed all of them as siblings and they summed to
 * 104%, which is how the nesting was noticed.  Do not add a bucket without
 * saying which parent it is inside.
 *
 * IT COSTS SOMETHING.  Two TMU reads and a 64-bit subtract per bracket.  The
 * frequent ones are the event sources (tens per frame) and `gpu` (hundreds),
 * so the overhead is tens of microseconds a frame -- fine for reading a
 * spread, not fine for quoting a frame time from.  A number to compare against
 * a build without this is a number FROM a build without this.
 */

#ifndef BLOOM_PERF_H
#define BLOOM_PERF_H

#include <stdint.h>

enum {
	/* THE SIBLINGS.  These three and `other` are the frame, and nothing
	 * else is ever added to a total. */
	PERF_CPU,       /* inside generated code: lightrec_execute         */
	PERF_EVT,       /* gen_interupt: the event tick between dispatches */
	PERF_FLIP,      /* dc_vout_flip: scene submit and present          */

	/* INSIDE `cpu` -- the C that generated code calls without leaving the
	 * dispatch.  `hw` is every MMIO access the optimiser could classify,
	 * and the DMA engines hang off it, so `gpu` is inside `hw` in turn. */
	PERF_HW,        /* lightrec_hw_lb .. lightrec_hw_sw: known MMIO     */
	PERF_RW,        /* fgl_rw: an access whose region was not proved    */
	PERF_COP,       /* fgl_mtc / fgl_mfc / fgl_rfe                      */
	PERF_SVC,       /* the dispatcher's own doors: interpret, memset,
			 * ds_check -- a block giving up on emitted code    */
	PERF_GPU,       /* do_cmd_list: GPU list -> PVR polygons            */

	/* INSIDE `evt`, and specifically inside the ONE `rcnt` tick a frame
	 * that crosses vblank.  pcsx hangs the whole frame boundary off that
	 * counter, so these are not counter work at all. */
	PERF_EMUUPDATE, /* EmuUpdate: input, memcard, frame limiter         */
	PERF_LACE,      /* GPU_updateLace: sync with the render thread      */
	PERF_SPU,       /* SPU_async                                        */
	PERF_VBLANK,    /* GPU_vBlank, both calls                           */

	/* Inside nothing: the recompiler has its own thread. */
	PERF_JIT,

	PERF_N
};

/* Which parent a bucket is reported under.  The table that pairs these with
 * the printed names lives in platform.c, next to the report that reads it. */
enum { PERF_IN_FRAME, PERF_IN_CPU, PERF_IN_HW, PERF_IN_EVT, PERF_IN_NONE };

#define PERF_EVT_N 16

extern uint64_t bloom_perf_us[PERF_N];
extern uint32_t bloom_perf_cnt[PERF_N];

/* WALL TIME IN A BUCKET IS NOT THE SAME AS COST, and `lace` is the row where
 * the difference decides what to do next.  `GPU_updateLace` blocks until the
 * render thread is done, so its 15 ms a frame is either the emulator standing
 * still with work left to do -- the largest single item outside generated
 * code -- or the SH-4 asleep on a vblank it was going to miss anyway, which
 * is slack and cannot be reclaimed by making anything faster.
 *
 * The two are indistinguishable from a timer, so this samples the IDLE thread
 * either side of the bracket as well.  Idle time that accrues while the
 * bracket is open is time the scheduler had nothing to run: subtract it and
 * what is left is what the bucket actually costs. */
extern uint64_t bloom_perf_idle_us[PERF_N];
extern uint64_t bloom_perf_evt_us[PERF_EVT_N];
extern uint32_t bloom_perf_evt_cnt[PERF_EVT_N];

/* Microseconds.  Defined in platform.c so that lightrec and pcsx_rearmed do
 * not have to see a KOS header to be bracketed. */
uint64_t bloom_perf_now(void);

/* Microseconds the idle thread has accumulated, from the same place. */
uint64_t bloom_perf_idle_now(void);

/* OFF BY DEFAULT, AND THE MACROS VANISH WHEN IT IS.
 *
 * The brackets are not free and the cost is not small.  The MMIO shims fire
 * ~1270 times a frame and `gpu` ~1900, so a frame pays on the order of 3300
 * bracket pairs and each is two TMU reads -- ~6600 on-chip I/O accesses a
 * frame, against the 400 a frame an uninstrumented build makes in total.
 * Measured on Spyro: ~10 ms a frame, which is a sixth of the frame.
 *
 * The report itself is four lines a second down a 115200 serial port on top of
 * that.  Both are worth having while a question is open and neither belongs in
 * a build whose frame time is being quoted.  A number to compare against a
 * build without this is a number FROM a build without this.
 *
 * -DWITH_PERF=ON puts it all back. */
#ifndef BLOOM_PERF
#define BLOOM_PERF 0
#endif

#if BLOOM_PERF

#define PERF_BEGIN(b)   uint64_t perf_t0_##b = bloom_perf_now()
#define PERF_END(b)     do {                                             \
		bloom_perf_us[b] += bloom_perf_now() - perf_t0_##b;      \
		bloom_perf_cnt[b]++;                                     \
	} while (0)

/* The same, for a bucket chosen at run time. */
/* The same pair, plus the idle delta.  Two extra reads, so it goes only on a
 * bucket where the distinction is the question being asked. */
#define PERF_BEGIN_I(b)         uint64_t perf_t0_##b = bloom_perf_now();     \
				uint64_t perf_i0_##b = bloom_perf_idle_now()
#define PERF_END_I(b)   do {                                             \
		bloom_perf_us[b] += bloom_perf_now() - perf_t0_##b;      \
		bloom_perf_idle_us[b] += bloom_perf_idle_now()           \
				       - perf_i0_##b;                    \
		bloom_perf_cnt[b]++;                                     \
	} while (0)

#define PERF_BEGIN_AT(v)        uint64_t v = bloom_perf_now()
#define PERF_END_EVT(v, i)      do {                                     \
		bloom_perf_evt_us[i] += bloom_perf_now() - (v);          \
		bloom_perf_evt_cnt[i]++;                                 \
	} while (0)

#else

#define PERF_BEGIN(b)           do { } while (0)
#define PERF_END(b)             do { } while (0)
#define PERF_BEGIN_I(b)         do { } while (0)
#define PERF_END_I(b)           do { } while (0)
#define PERF_BEGIN_AT(v)        do { } while (0)
#define PERF_END_EVT(v, i)      do { } while (0)

#endif /* BLOOM_PERF */

#endif /* BLOOM_PERF_H */
