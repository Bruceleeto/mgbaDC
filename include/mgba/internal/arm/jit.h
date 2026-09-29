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
 * many guest instructions it executed. */
uint32_t ARMJITRun(struct ARMCore* cpu);
/* One instruction on the interpreter, events or not (arm.c). */
void ARMRunInstruction(struct ARMCore* cpu);
/* Drop every compiled block: reset, savestate load, anything that replaces
 * guest code behind the CPU's back. */
void ARMJITFlush(struct ARMCore* cpu);
void ARMJITGetStats(struct ARMCore* cpu, struct ARMJITStats* stats);

CXX_GUARD_END

#endif
