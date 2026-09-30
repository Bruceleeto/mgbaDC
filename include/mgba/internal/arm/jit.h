/* ARM7 -> SH-4 recompiler.
 *
 * Replaces mGBA's interpreter as the thing that runs guest code; everything
 * else (memory, I/O, timing, IRQs) is reached through the same ARMCore hooks
 * the interpreter uses. On the Dreamcast the generated code runs natively. On
 * a 32-bit Linux build it runs under src/arm/jit/sh4-interp.h, which is how
 * the recompiler is developed and tested without the hardware.
 *
 * Plan and design notes: docs/sh4jit/GAMEPLAN.md. */
#ifndef ARM_JIT_H
#define ARM_JIT_H

#include <mgba-util/common.h>

CXX_GUARD_START

struct ARMCore;

struct ARMJITStats {
	uint32_t blocksCompiled;
	uint32_t guestInsnsCompiled;
	uint32_t armInsnsCompiled;
	uint32_t codeBytes;
	uint32_t flushes;
	uint32_t invalidations;
	uint32_t fallbackSteps;
	uint64_t blockRuns;
	/* Why generated code returned to C, by jit-private.h's JITExitType. */
	uint32_t exits[8];
};

/* Call after the core is created and its memory hooks are installed. */
bool ARMJITInit(struct ARMCore* cpu);
void ARMJITDeinit(struct ARMCore* cpu);
void ARMJITRunLoop(struct ARMCore* cpu);
/* Run generated code from the PC until it needs C: an event is due, a
 * target has to be compiled, and so on. Doesn't process events. Returns how
 * many guest instructions it executed; generated code only counts them off
 * the Dreamcast (tools/jittest), so there it's 0 for a block run. */
uint32_t ARMJITRun(struct ARMCore* cpu);
/* One instruction on the interpreter, events or not (arm.c). */
void ARMRunInstruction(struct ARMCore* cpu);
/* Drop every compiled block: reset, savestate load, anything that replaces
 * guest code behind the CPU's back. */
void ARMJITFlush(struct ARMCore* cpu);
void ARMJITGetStats(struct ARMCore* cpu, struct ARMJITStats* stats);
/* The cpsr flag bits generated code may have left stale where it stopped:
 * nothing from the next instruction on reads them before writing them. */
uint32_t ARMJITStaleFlags(struct ARMCore* cpu);

#if defined(__DREAMCAST__) && defined(M_ARM_JIT_FASTMEM)
struct VFile;
/* ROM paging (fastmem.c) for a ROM that doesn't fit: read from vf 64 KiB at a
 * time into up to budget bytes of frames, and seen through the MMU at
 * BASE_CART0, which it returns (NULL if it can't). *page0 is page 0's frame,
 * for C to write the GPIO registers through. */
void* ARMJITRomPagingOpen(struct VFile* vf, size_t size, size_t budget, void** page0);
void ARMJITRomPagingClose(void);
uint32_t ARMJITRomPagingFaults(void);
#endif

CXX_GUARD_END

#endif
