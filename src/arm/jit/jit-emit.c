/* ARM/Thumb -> SH-4 translation.
 *
 * Register use in generated code:
 *   GBR  struct ARMCore* (cycles, cpsr, prefetch via @(disp,GBR))
 *   r14  struct ARMCore* too: gprs[0-15] are @(0-60,r14), reachable into any
 *        register in one instruction, and it is the first argument to every
 *        helper for the price of a mov
 *   r13  cycles - nextEvent: blocks add their cost, and a block's entry
 *        leaves for the event handler once it is >= 0. cpu->cycles is only
 *        written when C is about to look at it.
 *   r12  guest instructions executed since generated code was entered
 *   r0   GBR/immediate-logic operand
 *   r1-r7 scratch, clobbered by every call
 *   r11  gprs[PC] as the block's first instruction sees it; the memory stubs
 *        add the access's offset (index * length, r6) to get its own
 *   r8   jit->memStubs
 *   r9   N and Z: those of r9 taken as a result
 *   r10  C in bit 31, V in bit 30
 *
 * Guest registers live in the ARMCore between instructions, so anything
 * mGBA's code looks at is always current, except:
 *   - the flags: r9/r10, written to the cpsr on the way out to C (exits,
 *     handler calls) and read back when coming in. The memory slow paths
 *     don't look at them.
 *   - cycles: see r13. The static cost of the instructions since the last
 *     charge is held back in the emitter and added before anything that can
 *     observe it (a call, leaving the block).
 *   - flags an instruction sets that no later instruction in the block reads
 *     before they are overwritten are not computed (liveness pass).
 *   - gprs[PC] and the prefetch words are written by the helpers that can see
 *     them, and by the C side when generated code gives control back.
 *
 * Each instruction's cycles are exactly what mGBA's handler charges, so a
 * run leaves the cpu in the state the interpreter reaches after the same
 * number of instructions; tools/jittest checks that.
 *
 * Instructions not translated natively are calls to mGBA's own handler with
 * PC/prefetch/cycles set up as ARMStep/ThumbStep leave them. */
#include "jit-private.h"

#include <mgba/internal/arm/isa-arm.h>
#include <mgba/internal/arm/isa-inlines.h>
#include <mgba/internal/arm/isa-thumb.h>
#include <mgba/internal/arm/macros.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/memory.h>
#include <mgba-util/math.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __sh__
#include <kos/cache.h>
#endif

#define R_CPU 14
#define R_CYCLES 13
#define R_COUNT 12
#define R_BASE 11
#define R_STUBS 8
#define R_NZ 9
#define R_CV 10
#define R_ACC 7

#define F_V 1
#define F_C 2
#define F_Z 4
#define F_N 8
#define F_NZ (F_N | F_Z)
#define F_NZC (F_N | F_Z | F_C)
#define F_ALL (F_N | F_Z | F_C | F_V)

/* ---------------------------------------------------------------- */
/* Emitter: code buffer plus a PC-relative literal pool              */
/* ---------------------------------------------------------------- */

#define POOL_MAX 64
#define FIX_MAX 256
/* mov.l @(disp,PC),Rn reaches 255 longwords past (PC & ~3) + 4. */
#define POOL_REACH 1020
/* Upper bound on one guest instruction's code, for the reach check. */
#define INSN_MAX_BYTES 192

struct JITEmitter {
	sh4_codegen cg;
	struct ARMJIT* jit;
	uint32_t lits[POOL_MAX];
	/* Entries _lit mustn't share: their value changes after emission. */
	uint64_t unique;
	int nLits;
	uint8_t* fixAt[FIX_MAX];
	uint8_t fixLit[FIX_MAX];
	int nFix;

	/* Link sites whose stubs haven't been placed: pool index of the
	 * { entry, key } pair and the exit the stub takes. */
	struct {
		int lit;
		int type;
	} sites[4];
	int nSites;

	/* The instruction being translated. */
	bool thumb;
	uint32_t address;
	int index;
	/* Guest region the block is in (address >> 24). */
	uint32_t region;

	/* Memory accesses use the stall stubs (code in ROM, prefetch on). */
	bool stall;
	/* Something used r11 (the block sets it on entry). */
	bool usesBase;

	/* Cycles charged by instructions so far and not yet in r13. */
	int32_t pending;
	int32_t seq16;
	int32_t nonseq16;
	int32_t seq32;
	int32_t nonseq32;
};

/* Refer the PC-relative instruction about to be emitted to pool entry i. */
static void _fix(struct JITEmitter* e, int i) {
	e->fixAt[e->nFix] = e->cg.ptr;
	e->fixLit[e->nFix] = i;
	++e->nFix;
}

/* mov.l @(pool),reg. The displacement is patched when the pool is placed. */
static void _lit(struct JITEmitter* e, uint32_t value, int reg) {
	int i;
	for (i = 0; i < e->nLits && (e->lits[i] != value || (e->unique >> i) & 1); ++i) {
	}
	if (i == e->nLits) {
		e->lits[e->nLits++] = value;
	}
	_fix(e, i);
	sh4_emit_mov_l_load_pc(&e->cg, 0, reg);
}

/* Pool entries of their own, which later code or the C side rewrites. */
static int _litUnique(struct JITEmitter* e, uint32_t value) {
	e->lits[e->nLits] = value;
	e->unique |= (uint64_t) 1 << e->nLits;
	return e->nLits++;
}

static void _imm(struct JITEmitter* e, uint32_t value, int reg) {
	if ((int32_t) value >= -128 && (int32_t) value <= 127) {
		sh4_emit_mov_imm(&e->cg, (int) value, reg);
	} else {
		_lit(e, value, reg);
	}
}

/* Place the pending literals here. Mid-block the pool is jumped over. */
static void _flushPool(struct JITEmitter* e, bool jumpOver) {
	if (!e->nFix) {
		return;
	}
	uint8_t* bra = NULL;
	if (jumpOver) {
		bra = e->cg.ptr;
		sh4_emit_bra(&e->cg, 0);
		sh4_emit_nop(&e->cg);
	}
	if ((uintptr_t) e->cg.ptr & 2) {
		sh4_emit_nop(&e->cg);
	}
	uint8_t* pool = e->cg.ptr;
	int i;
	for (i = 0; i < e->nLits; ++i) {
		sh4_word(&e->cg, e->lits[i] & 0xFFFF);
		sh4_word(&e->cg, e->lits[i] >> 16);
	}
	if (e->cg.overflow) {
		return;
	}
	for (i = 0; i < e->nFix; ++i) {
		uintptr_t base = ((uintptr_t) e->fixAt[i] & ~(uintptr_t) 3) + 4;
		uintptr_t disp = ((uintptr_t) pool + 4 * e->fixLit[i] - base) >> 2;
		if (disp > 255) {
			e->cg.overflow = 1;
			return;
		}
		e->fixAt[i][0] = disp;
	}
	if (bra) {
		int32_t d = sh4_branch_disp12((uintptr_t) bra, (uintptr_t) e->cg.ptr);
		bra[0] = d & 0xFF;
		bra[1] = 0xA0 | ((d >> 8) & 0xF);
	}
	e->nLits = 0;
	e->unique = 0;
	e->nFix = 0;
}

/* Called before each guest instruction: flush now if the next one could push
 * the oldest pending load out of reach of its literal. */
static void _poolCheck(struct JITEmitter* e) {
	if (!e->nFix) {
		return;
	}
	ptrdiff_t span = e->cg.ptr - e->fixAt[0];
	span += INSN_MAX_BYTES + 4 + 2 + 4 * (e->nLits + 8);
	if (span > POOL_REACH - 16 || e->nLits > POOL_MAX - 8 || e->nFix > FIX_MAX - 16) {
		_flushPool(e, true);
	}
}

/* ---------------------------------------------------------------- */
/* State sync, calls, exits                                          */
/* ---------------------------------------------------------------- */

static uint32_t _insnLength(const struct JITEmitter* e) {
	return e->thumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM;
}

/* The value gprs[PC] holds while the current instruction executes. */
static uint32_t _pcValue(const struct JITEmitter* e) {
	return e->address + 2 * _insnLength(e);
}

/* r13 += value */
static void _addCycles(struct JITEmitter* e, int32_t value) {
	if (!value) {
		return;
	}
	if (value >= -128 && value <= 127) {
		sh4_emit_add_imm(&e->cg, value, R_CYCLES);
	} else {
		_imm(e, value, 1);
		sh4_emit_add_reg(&e->cg, 1, R_CYCLES);
	}
}

/* Charge what the instructions so far cost. */
static void _charge(struct JITEmitter* e) {
	_addCycles(e, e->pending);
	e->pending = 0;
}

/* r12 += executed */
static void _count(struct JITEmitter* e, int executed) {
	sh4_emit_add_imm(&e->cg, executed, R_COUNT);
}

/* Jump to one of the fixed routines (ARMJITEmitStubs). */
static void _jumpTo(struct JITEmitter* e, const void* target) {
	_lit(e, (uint32_t) (uintptr_t) target, 1);
	sh4_emit_jmp(&e->cg, 1);
}

/* Leave through a link site: a jump through a pool entry that starts out
 * pointing at a stub (placed at the end of the block) that exits to C, and
 * that C points at the target block once it is compiled. */
static void _site(struct JITEmitter* e, uint32_t key, int type) {
	int lit = _litUnique(e, 0);
	_litUnique(e, key);
	e->sites[e->nSites].lit = lit;
	e->sites[e->nSites].type = type;
	++e->nSites;
	_fix(e, lit);
	sh4_emit_mov_l_load_pc(&e->cg, 0, 1);
	sh4_emit_jmp(&e->cg, 1);
	sh4_emit_nop(&e->cg);
}

/* The stubs for the sites: r0 = the pair's address, then the exit. */
static void _placeSites(struct JITEmitter* e) {
	int i;
	for (i = 0; i < e->nSites; ++i) {
		e->lits[e->sites[i].lit] = (uint32_t) (uintptr_t) e->cg.ptr;
		_fix(e, e->sites[i].lit);
		sh4_emit_mova(&e->cg, 0);
		_jumpTo(e, e->jit->exits[e->sites[i].type]);
		sh4_emit_nop(&e->cg);
	}
	e->nSites = 0;
}

/* ---------------------------------------------------------------- */
/* Guest registers and flags                                         */
/* ---------------------------------------------------------------- */

static void _ld(struct JITEmitter* e, int guest, int host) {
	if (guest == ARM_PC) {
		_imm(e, _pcValue(e), host);
	} else {
		sh4_emit_mov_l_load_disp(&e->cg, R_CPU, host, guest);
	}
}

static void _st(struct JITEmitter* e, int host, int guest) {
	sh4_emit_mov_l_store_disp(&e->cg, host, R_CPU, guest);
}

/* T = bit 31 of r, r unchanged. */
static void _signToT(struct JITEmitter* e, int r) {
	sh4_emit_rotl(&e->cg, r);
	sh4_emit_rotr(&e->cg, r);
}

/* Flags live in r9/r10 while generated code runs (see the top): N and Z
 * are those of r9 as a result, C is bit 31 of r10 and V bit 30. */

/* N/Z from result r. */
static void _flagsNZ(struct JITEmitter* e, int r) {
	sh4_emit_mov_reg(&e->cg, r, R_NZ);
}

/* N/Z from r, C from bit 0 of rc (which is destroyed). */
static void _flagsNZC(struct JITEmitter* e, int r, int rc) {
	sh4_emit_mov_reg(&e->cg, r, R_NZ);
	sh4_emit_rotl(&e->cg, R_CV);
	sh4_emit_shlr(&e->cg, rc);
	sh4_emit_rotcr(&e->cg, R_CV);
}

/* T = C */
static void _carryToT(struct JITEmitter* e) {
	sh4_emit_rotl(&e->cg, R_CV);
	sh4_emit_rotr(&e->cg, R_CV);
}

/* ra + rb or ra - rb, into guest register rd (-1 for CMP/CMN). ra and rb are
 * r1/r2. */
static void _addSub(struct JITEmitter* e, bool sub, int rd, bool flags) {
	if (!flags) {
		if (rd < 0) {
			return;
		}
		if (sub) {
			sh4_emit_sub(&e->cg, 2, 1);
		} else {
			sh4_emit_add_reg(&e->cg, 2, 1);
		}
		_st(e, 1, rd);
		return;
	}
	sh4_emit_mov_reg(&e->cg, 1, R_NZ);
	if (sub) {
		sh4_emit_subv(&e->cg, 2, R_NZ);
		sh4_emit_rotcr(&e->cg, R_CV);
		sh4_emit_cmphs(&e->cg, 2, 1);
	} else {
		sh4_emit_addv(&e->cg, 2, R_NZ);
		sh4_emit_rotcr(&e->cg, R_CV);
		/* carry iff the sum wrapped below an operand */
		sh4_emit_cmphi(&e->cg, R_NZ, 1);
	}
	sh4_emit_rotcr(&e->cg, R_CV);
	if (rd >= 0) {
		_st(e, R_NZ, rd);
	}
}

/* ADC/SBC: r1 + r2 + C, with r2 inverted first for SBC. */
static void _addCarry(struct JITEmitter* e, bool sub, int rd, bool flags) {
	if (sub) {
		sh4_emit_not(&e->cg, 2, 2);
	}
	_carryToT(e);
	sh4_emit_mov_reg(&e->cg, 1, 3);
	sh4_emit_addc(&e->cg, 2, 3);
	_st(e, 3, rd);
	if (!flags) {
		return;
	}
	sh4_emit_movt(&e->cg, 5);
	/* V = ~(a ^ b) & (a ^ result), sign bit */
	sh4_emit_mov_reg(&e->cg, 1, 6);
	sh4_emit_xor(&e->cg, 2, 6);
	sh4_emit_not(&e->cg, 6, 6);
	sh4_emit_mov_reg(&e->cg, 1, 4);
	sh4_emit_xor(&e->cg, 3, 4);
	sh4_emit_and(&e->cg, 4, 6);
	sh4_emit_shll(&e->cg, 6);
	sh4_emit_rotcr(&e->cg, R_CV);
	sh4_emit_shlr(&e->cg, 5);
	sh4_emit_rotcr(&e->cg, R_CV);
	sh4_emit_mov_reg(&e->cg, 3, R_NZ);
}

/* Fixed code: the flags into the cpsr's (clearing bits 27-24, as
 * THUMB_ADDITION_S does); r0 and scratch clobbered. */
static void _emitFlagsOut(struct JITEmitter* e, int scratch) {
	sh4_emit_mov_reg(&e->cg, R_NZ, scratch);
	sh4_emit_rotl(&e->cg, scratch);
	sh4_emit_rotcl(&e->cg, 0);
	sh4_emit_tst(&e->cg, R_NZ, R_NZ);
	sh4_emit_rotcl(&e->cg, 0);
	sh4_emit_mov_reg(&e->cg, R_CV, scratch);
	sh4_emit_shll(&e->cg, scratch);
	sh4_emit_rotcl(&e->cg, 0);
	sh4_emit_shll(&e->cg, scratch);
	sh4_emit_rotcl(&e->cg, 0);
	sh4_emit_shll2(&e->cg, 0);
	sh4_emit_shll2(&e->cg, 0);
	sh4_emit_mov_b_store_gbr(&e->cg, JIT_GBR_FLAGS);
}

/* Fixed code: r9/r10 from the cpsr; r0 and r1 clobbered. N and Z both set
 * can't be held (ARMJITRun steps through it instead). */
static void _emitFlagsIn(struct JITEmitter* e) {
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_CPSR);
	sh4_emit_mov_reg(&e->cg, 0, R_CV);
	sh4_emit_shll2(&e->cg, R_CV);
	sh4_emit_shll(&e->cg, 0);
	sh4_emit_movt(&e->cg, 1);
	sh4_emit_cmppz(&e->cg, 0);
	sh4_emit_movt(&e->cg, R_NZ);
	sh4_emit_rotr(&e->cg, 1);
	sh4_emit_or(&e->cg, 1, R_NZ);
}

/* ---------------------------------------------------------------- */
/* Memory                                                            */
/* ---------------------------------------------------------------- */

/* What the interpreter has in gprs[PC] and the prefetch words during the
 * access: mGBA's open bus and ROM prefetch-buffer model read them. */
static void _setPC(struct ARMCore* cpu, uint32_t pc) {
	cpu->gprs[ARM_PC] = pc;
	if (cpu->executionMode == MODE_THUMB) {
		LOAD_16(cpu->prefetch[0], (pc - WORD_SIZE_THUMB) & cpu->memory.activeMask, cpu->memory.activeRegion);
		LOAD_16(cpu->prefetch[1], pc & cpu->memory.activeMask, cpu->memory.activeRegion);
	} else {
		LOAD_32(cpu->prefetch[0], (pc - WORD_SIZE_ARM) & cpu->memory.activeMask, cpu->memory.activeRegion);
		LOAD_32(cpu->prefetch[1], pc & cpu->memory.activeMask, cpu->memory.activeRegion);
	}
}

/* The access's own wait states go straight into cpu->cycles; the fixed part
 * of the instruction's cost is charged by the emitter. */
#ifndef __sh__
uint32_t jitLoadHist[2][16];
/* handler calls: [thumb][op >> 8 (Thumb), op >> 20 & 0xFF (ARM)] */
uint32_t jitHandlerHist[2][256];
#define HHIST(T, OP) ++jitHandlerHist[T][(T) ? (OP) >> 8 & 0xFF : (OP) >> 20 & 0xFF]
#define HIST(A) ++jitLoadHist[((struct GBA*) cpu->master)->memory.activeRegion >= REGION_CART0 && ((struct GBA*) cpu->master)->memory.prefetch][(A) >> 24 & 15]
#else
#define HIST(A)
#define HHIST(T, OP)
#endif
static uint32_t _load32(struct ARMCore* cpu, uint32_t address, uint32_t pc) {
	HIST(address);
	_setPC(cpu, pc);
	int cycles = 0;
	uint32_t value = cpu->memory.load32(cpu, address, &cycles);
	cpu->cycles += cycles;
	return value;
}

static uint32_t _load16(struct ARMCore* cpu, uint32_t address, uint32_t pc) {
	HIST(address);
	_setPC(cpu, pc);
	int cycles = 0;
	uint32_t value = cpu->memory.load16(cpu, address, &cycles);
	cpu->cycles += cycles;
	return value;
}

static uint32_t _load8(struct ARMCore* cpu, uint32_t address, uint32_t pc) {
	HIST(address);
	_setPC(cpu, pc);
	int cycles = 0;
	uint32_t value = cpu->memory.load8(cpu, address, &cycles);
	cpu->cycles += cycles;
	return value;
}

static uint32_t _loadS8(struct ARMCore* cpu, uint32_t address, uint32_t pc) {
	return ARM_SXT_8(_load8(cpu, address, pc));
}

/* An odd address loads a sign-extended byte, as LDRSH does on the ARM7. */
static uint32_t _loadS16(struct ARMCore* cpu, uint32_t address, uint32_t pc) {
	uint32_t value = _load16(cpu, address, pc);
	return address & 1 ? ARM_SXT_8(value) : ARM_SXT_16(value);
}

/* A store can pull the next event in (HALTCNT, IE/IME, DMA and timer
 * control) or change the wait states the block's fixed costs were compiled
 * with (WAITCNT). The block then stops after this instruction, as the
 * interpreter would see the change right after it, so the store charges the
 * instruction's fixed cost itself, the way the handler does: the prefetch
 * with the old wait states, the N/S adjustment with the new ones. PC and
 * prefetch already hold what they do after the instruction. Returns nonzero
 * if the block must stop. */
struct JITStoreState {
	int32_t nextEvent;
	int32_t seq;
	uint32_t timingKey;
};

static void _storeBegin(struct ARMCore* cpu, struct JITStoreState* state, uint32_t pc) {
	_setPC(cpu, pc);
	state->nextEvent = cpu->nextEvent;
	state->seq = cpu->executionMode == MODE_THUMB ? cpu->memory.activeSeqCycles16 : cpu->memory.activeSeqCycles32;
	state->timingKey = ARMJITTimingKey(cpu);
}

static uint32_t _storeEnd(struct ARMCore* cpu, const struct JITStoreState* state, int cycles) {
	cpu->cycles += cycles;
	bool retimed = ARMJITTimingKey(cpu) != state->timingKey;
	if (cpu->nextEvent >= state->nextEvent && !retimed) {
		return 0;
	}
	if (cpu->executionMode == MODE_THUMB) {
		cpu->cycles += 1 + state->seq + cpu->memory.activeNonseqCycles16 - cpu->memory.activeSeqCycles16;
	} else {
		cpu->cycles += 1 + state->seq + cpu->memory.activeNonseqCycles32 - cpu->memory.activeSeqCycles32;
	}
	if (retimed) {
		ARMJITTimingChanged(cpu->jit);
	}
	return 1;
}

static uint32_t _store32(struct ARMCore* cpu, uint32_t address, uint32_t value, uint32_t pc) {
	struct JITStoreState state;
	_storeBegin(cpu, &state, pc);
	int cycles = 0;
	cpu->memory.store32(cpu, address, value, &cycles);
	return _storeEnd(cpu, &state, cycles);
}

static uint32_t _store16(struct ARMCore* cpu, uint32_t address, uint32_t value, uint32_t pc) {
	struct JITStoreState state;
	_storeBegin(cpu, &state, pc);
	int cycles = 0;
	cpu->memory.store16(cpu, address, value, &cycles);
	return _storeEnd(cpu, &state, cycles);
}

static uint32_t _store8(struct ARMCore* cpu, uint32_t address, uint32_t value, uint32_t pc) {
	struct JITStoreState state;
	_storeBegin(cpu, &state, pc);
	int cycles = 0;
	cpu->memory.store8(cpu, address, value, &cycles);
	return _storeEnd(cpu, &state, cycles);
}

/* Address in r4; loads land in guest rd, stores take guest rd (or r5 if rd
 * is negative). */
static void _memory(struct JITEmitter* e, enum JITMemOp op, int rd) {
	bool store = op >= JIT_MEM_STORE32;
	_charge(e);
	if (store && rd >= 0) {
		_ld(e, rd, 5);
	}
	sh4_emit_mov_l_load_disp(&e->cg, R_STUBS, 1, e->stall * JIT_MEM_OPS + op);
	sh4_emit_jsr(&e->cg, 1);
	sh4_emit_mov_imm(&e->cg, e->index * _insnLength(e), 6);
	if (!store) {
		_st(e, 0, rd);
	}
	e->usesBase = true;
	/* The prefetch's 1S becomes 1N: THUMB/ARM_PREFETCH_CYCLES +
	 * *_LOAD/STORE_POST_BODY. */
	e->pending += 1 + (e->thumb ? e->nonseq16 : e->nonseq32);
}

/* ---------------------------------------------------------------- */
/* Fixed routines                                                    */
/* ---------------------------------------------------------------- */

static void _patchBranch(uint8_t* at, uint8_t* to) {
	int32_t d = sh4_branch_disp12((uintptr_t) at, (uintptr_t) to);
	if ((at[1] & 0xF0) == 0xA0) { /* bra */
		at[0] = d & 0xFF;
		at[1] = 0xA0 | ((d >> 8) & 0xF);
	} else { /* bt/bf */
		at[0] = d & 0xFF;
	}
}

/* Where a stub's short branch lands: its target once known. */
struct JITFixups {
	uint8_t* at[8];
	int n;
};

static void _branchHere(struct JITFixups* f, uint8_t* to) {
	int i;
	for (i = 0; i < f->n; ++i) {
		_patchBranch(f->at[i], to);
	}
	f->n = 0;
}

static void _bfTo(struct JITEmitter* e, struct JITFixups* f) {
	f->at[f->n++] = e->cg.ptr;
	sh4_emit_bf(&e->cg, 0);
}

static void _btTo(struct JITEmitter* e, struct JITFixups* f) {
	f->at[f->n++] = e->cg.ptr;
	sh4_emit_bt(&e->cg, 0);
}

/* The slow paths: mGBA's own access through the wrappers above, with
 * cpu->cycles and PC as they expect. r3 = wrapper, r4 = address, r5 = value,
 * r6 = offset. Loads return the value in r0. A store that says stop leaves
 * for dispatchSync with this instruction counted. */
static void _emitSlowLoad(struct JITEmitter* e) {
	sh4_emit_sts_pr_dec(&e->cg, 15);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_NEXT_EVENT);
	sh4_emit_add_reg(&e->cg, R_CYCLES, 0);
	sh4_emit_mov_l_store_gbr(&e->cg, JIT_GBR_CYCLES);
	sh4_emit_mov_reg(&e->cg, 4, 5);
	sh4_emit_add_reg(&e->cg, R_BASE, 6);
	sh4_emit_jsr(&e->cg, 3);
	sh4_emit_mov_reg(&e->cg, R_CPU, 4);
	sh4_emit_mov_reg(&e->cg, 0, 3);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_CYCLES);
	sh4_emit_mov_reg(&e->cg, 0, R_CYCLES);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_NEXT_EVENT);
	sh4_emit_sub(&e->cg, 0, R_CYCLES);
	sh4_emit_lds_pr_inc(&e->cg, 15);
	sh4_emit_rts(&e->cg);
	sh4_emit_mov_reg(&e->cg, 3, 0);
}

static void _emitSlowStore(struct JITEmitter* e) {
	sh4_emit_sts_pr_dec(&e->cg, 15);
	sh4_emit_mov_l_store_dec(&e->cg, 6, 15);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_NEXT_EVENT);
	sh4_emit_add_reg(&e->cg, R_CYCLES, 0);
	sh4_emit_mov_l_store_gbr(&e->cg, JIT_GBR_CYCLES);
	sh4_emit_mov_reg(&e->cg, R_BASE, 7);
	sh4_emit_add_reg(&e->cg, 6, 7);
	sh4_emit_mov_reg(&e->cg, 5, 6);
	sh4_emit_mov_reg(&e->cg, 4, 5);
	sh4_emit_jsr(&e->cg, 3);
	sh4_emit_mov_reg(&e->cg, R_CPU, 4);
	sh4_emit_mov_l_load_inc(&e->cg, 15, 6);
	sh4_emit_mov_reg(&e->cg, 0, 3);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_CYCLES);
	sh4_emit_mov_reg(&e->cg, 0, R_CYCLES);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_NEXT_EVENT);
	sh4_emit_sub(&e->cg, 0, R_CYCLES);
	sh4_emit_lds_pr_inc(&e->cg, 15);
	sh4_emit_tst(&e->cg, 3, 3);
	uint8_t* stop = e->cg.ptr;
	sh4_emit_bf(&e->cg, 0);
	sh4_emit_rts(&e->cg);
	sh4_emit_nop(&e->cg);
	_patchBranch(stop, e->cg.ptr);
	/* r12 += offset / length + 1; a store doesn't change the mode */
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_CPSR);
	sh4_emit_shlr(&e->cg, 6);
	sh4_emit_tst_imm(&e->cg, 0x20);
	uint8_t* thumb = e->cg.ptr;
	sh4_emit_bf(&e->cg, 0);
	sh4_emit_shlr(&e->cg, 6);
	_patchBranch(thumb, e->cg.ptr);
	sh4_emit_add_imm(&e->cg, 1, 6);
	sh4_emit_add_reg(&e->cg, 6, R_COUNT);
	_jumpTo(e, e->jit->dispatchSync);
	sh4_emit_nop(&e->cg);
}

/* GBAMemoryStall for code in ROM with the prefetch buffer on: r2 = the
 * access's wait, r6 = offset, r0 kept. Adds the result to r13 and returns.
 * The loop is mGBA's, counting loads down in r5 from maxLoads - 1, which
 * makes lastPrefetchedPc pc + 2 * (7 - r5) whatever previousLoads was. */
static void _emitStall(struct JITEmitter* e) {
	struct GBA* gba = (struct GBA*) e->jit->cpu->master;
	sh4_emit_mov_reg(&e->cg, R_BASE, 7);
	sh4_emit_add_reg(&e->cg, 6, 7);
	sh4_emit_mov_reg(&e->cg, 0, 6);
	_lit(e, (uint32_t) (uintptr_t) &gba->memory.lastPrefetchedPc, 3);
	sh4_emit_mov_l_load(&e->cg, 3, 1);
	sh4_emit_sub(&e->cg, 7, 1);
	sh4_emit_mov_imm(&e->cg, 7, 5);
	sh4_emit_mov_imm(&e->cg, 16, 0);
	sh4_emit_cmphs(&e->cg, 0, 1);
	uint8_t* far = e->cg.ptr;
	sh4_emit_bt(&e->cg, 0);
	sh4_emit_shlr(&e->cg, 1);
	sh4_emit_sub(&e->cg, 1, 5);
	_patchBranch(far, e->cg.ptr);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_SEQ16);
	sh4_emit_mov_reg(&e->cg, 0, 1);
	sh4_emit_mov_reg(&e->cg, 0, 4);
	sh4_emit_add_imm(&e->cg, 1, 4);
	uint8_t* loop = e->cg.ptr;
	sh4_emit_cmpge(&e->cg, 2, 4);
	uint8_t* done1 = e->cg.ptr;
	sh4_emit_bt(&e->cg, 0);
	sh4_emit_tst(&e->cg, 5, 5);
	uint8_t* done2 = e->cg.ptr;
	sh4_emit_bt(&e->cg, 0);
	sh4_emit_add_reg(&e->cg, 1, 4);
	uint8_t* back = e->cg.ptr;
	sh4_emit_bra(&e->cg, 0);
	sh4_emit_add_imm(&e->cg, -1, 5);
	_patchBranch(back, loop);
	_patchBranch(done1, e->cg.ptr);
	_patchBranch(done2, e->cg.ptr);
	sh4_emit_mov_imm(&e->cg, 7, 0);
	sh4_emit_sub(&e->cg, 5, 0);
	sh4_emit_add_reg(&e->cg, 0, 0);
	sh4_emit_add_reg(&e->cg, 7, 0);
	sh4_emit_mov_l_store(&e->cg, 0, 3);
	/* wait = max(wait, stall) - (n - s + 1) - (stall - 1) */
	sh4_emit_cmpgt(&e->cg, 2, 4);
	uint8_t* keep = e->cg.ptr;
	sh4_emit_bf(&e->cg, 0);
	sh4_emit_mov_reg(&e->cg, 4, 2);
	_patchBranch(keep, e->cg.ptr);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_NONSEQ16);
	sh4_emit_sub(&e->cg, 0, 2);
	sh4_emit_add_reg(&e->cg, 1, 2);
	sh4_emit_sub(&e->cg, 4, 2);
	sh4_emit_add_reg(&e->cg, 2, R_CYCLES);
	sh4_emit_clrt(&e->cg);
	sh4_emit_rts(&e->cg);
	sh4_emit_mov_reg(&e->cg, 6, 0);
}

/* The access itself, r0 = offset into the region's buffer at @(md,r3), value
 * in/out as the op says. */
static void _emitAccess(struct JITEmitter* e, enum JITMemOp op, int md) {
	sh4_emit_mov_l_load_disp(&e->cg, 3, 1, md);
	switch (op) {
	case JIT_MEM_LOAD32:
		sh4_emit_mov_l_load_r0(&e->cg, 1, 0);
		break;
	case JIT_MEM_LOAD16:
		sh4_emit_mov_w_load_r0(&e->cg, 1, 0);
		sh4_emit_extu_w(&e->cg, 0, 0);
		break;
	case JIT_MEM_LOADS16:
		sh4_emit_mov_w_load_r0(&e->cg, 1, 0);
		break;
	case JIT_MEM_LOAD8:
		sh4_emit_mov_b_load_r0(&e->cg, 1, 0);
		sh4_emit_extu_b(&e->cg, 0, 0);
		break;
	case JIT_MEM_LOADS8:
		sh4_emit_mov_b_load_r0(&e->cg, 1, 0);
		break;
	case JIT_MEM_STORE32:
		sh4_emit_mov_l_store_r0(&e->cg, 5, 1);
		break;
	case JIT_MEM_STORE16:
		sh4_emit_mov_w_store_r0(&e->cg, 5, 1);
		break;
	case JIT_MEM_STORE8:
		sh4_emit_mov_b_store_r0(&e->cg, 5, 1);
		break;
	default:
		break;
	}
}

/* r0 = r4 & mask */
static void _emitOffset(struct JITEmitter* e, uint32_t mask) {
	_lit(e, mask, 1);
	sh4_emit_mov_reg(&e->cg, 4, 0);
	sh4_emit_and(&e->cg, 1, 0);
}

/* A store into a chunk with compiled code in it goes the slow way, which
 * invalidates. r0 = offset, chunk list @(md,r3). */
static void _emitSmcCheck(struct JITEmitter* e, int md, struct JITFixups* slow) {
	sh4_emit_mov_reg(&e->cg, 0, 2);
	sh4_emit_shlr8(&e->cg, 2);
	sh4_emit_shll2(&e->cg, 2);
	sh4_emit_mov_l_load_disp(&e->cg, 3, 1, md);
	sh4_emit_add_reg(&e->cg, 1, 2);
	sh4_emit_mov_l_load(&e->cg, 2, 2);
	sh4_emit_tst(&e->cg, 2, 2);
	_bfTo(e, slow);
}

/* The access's wait is in r2: add it, or run the stall model on it. */
static void _emitWaitTail(struct JITEmitter* e, bool stall, uint8_t* stallCode) {
	if (stall) {
		uint8_t* bra = e->cg.ptr;
		sh4_emit_bra(&e->cg, 0);
		sh4_emit_nop(&e->cg);
		_patchBranch(bra, stallCode);
	} else {
		sh4_emit_rts(&e->cg);
		sh4_emit_add_reg(&e->cg, 2, R_CYCLES);
	}
}

static void _emitMemoryStub(struct JITEmitter* e, enum JITMemOp op, bool stall, uint8_t* stallCode,
                            uint8_t* slowLoad, uint8_t* slowStore) {
	static const void* const slow[JIT_MEM_OPS] = {
		_load32, _load16, _loadS16, _load8, _loadS8, _store32, _store16, _store8
	};
	bool store = op >= JIT_MEM_STORE32;
	int size = op == JIT_MEM_LOAD32 || op == JIT_MEM_STORE32 ? 4
	         : op == JIT_MEM_LOAD8 || op == JIT_MEM_LOADS8 || op == JIT_MEM_STORE8 ? 1 : 2;
	struct JITFixups toSlow = { .n = 0 };
	struct JITFixups toIwram = { .n = 0 };
	struct JITFixups toRom = { .n = 0 };

	e->jit->memStubs[stall][op] = e->cg.ptr;
	sh4_emit_mov_reg(&e->cg, 4, 0);
	if (size > 1) {
		sh4_emit_tst_imm(&e->cg, size - 1);
		_bfTo(e, &toSlow);
	}
	sh4_emit_shlr16(&e->cg, 0);
	sh4_emit_shlr8(&e->cg, 0);
	_lit(e, (uint32_t) (uintptr_t) e->jit->memData, 3);
	sh4_emit_cmpeq_imm(&e->cg, REGION_WORKING_IRAM);
	_btTo(e, &toIwram);
	if (!store) {
		sh4_emit_cmpeq_imm(&e->cg, REGION_CART0);
		_btTo(e, &toRom);
		sh4_emit_cmpeq_imm(&e->cg, REGION_CART0_EX);
		_btTo(e, &toRom);
	}
	sh4_emit_cmpeq_imm(&e->cg, REGION_WORKING_RAM);
	_bfTo(e, &toSlow);

	/* EWRAM */
	_emitOffset(e, SIZE_WORKING_RAM - 1);
	if (store) {
		_emitSmcCheck(e, JIT_MD_CHUNKS_EWRAM, &toSlow);
	}
	_emitAccess(e, op, JIT_MD_WRAM);
	sh4_emit_mov_l_load_disp(&e->cg, 3, 2,
	                         store ? (size == 4 ? JIT_MD_EWRAM_STORE32 : JIT_MD_EWRAM_STORE16)
	                               : (size == 4 ? JIT_MD_EWRAM_LOAD32 : JIT_MD_EWRAM_LOAD16));
	_emitWaitTail(e, stall, stallCode);

	/* IWRAM: no wait states of its own */
	_branchHere(&toIwram, e->cg.ptr);
	_emitOffset(e, SIZE_WORKING_IRAM - 1);
	if (store) {
		_emitSmcCheck(e, JIT_MD_CHUNKS_IWRAM, &toSlow);
	}
	_emitAccess(e, op, JIT_MD_IWRAM);
	sh4_emit_mov_imm(&e->cg, store ? 1 : 2, 2);
	_emitWaitTail(e, stall, stallCode);

	/* ROM (wait state 0 only), never stalls: above BASE_CART0 */
	if (!store) {
		_branchHere(&toRom, e->cg.ptr);
		_emitOffset(e, SIZE_CART0 - 1);
		sh4_emit_mov_l_load_disp(&e->cg, 3, 1, JIT_MD_ROM_SIZE);
		sh4_emit_cmphs(&e->cg, 1, 0);
		_btTo(e, &toSlow);
		_emitAccess(e, op, JIT_MD_ROM);
		sh4_emit_mov_l_load_disp(&e->cg, 3, 2, size == 4 ? JIT_MD_ROM_LOAD32 : JIT_MD_ROM_LOAD16);
		_emitWaitTail(e, false, NULL);
	}

	_branchHere(&toSlow, e->cg.ptr);
	ARMJITRegisterCall(e->jit, slow[op]);
	_lit(e, (uint32_t) (uintptr_t) slow[op], 3);
	uint8_t* bra = e->cg.ptr;
	sh4_emit_bra(&e->cg, 0);
	sh4_emit_nop(&e->cg);
	_patchBranch(bra, store ? slowStore : slowLoad);
	_flushPool(e, false);
}

/* One region's fast path for _emitMultipleStub. */
static void _emitMultipleRegion(struct JITEmitter* e, bool store, bool stall, uint8_t* stallCode, int md,
                                int chunks, uint32_t size, struct JITFixups* slow) {
	_emitOffset(e, size - 4);
	sh4_emit_mov_reg(&e->cg, 7, 2);
	sh4_emit_shll2(&e->cg, 2);
	sh4_emit_add_reg(&e->cg, 0, 2);
	_lit(e, size, 1);
	sh4_emit_cmphi(&e->cg, 1, 2);
	_btTo(e, slow);
	if (store) {
		/* 64 bytes at most: the chunks of the first and last words */
		sh4_emit_mov_reg(&e->cg, 2, 0);
		sh4_emit_add_imm(&e->cg, -1, 0);
		_emitSmcCheck(e, chunks, slow);
		_emitOffset(e, size - 4);
		_emitSmcCheck(e, chunks, slow);
	}
	sh4_emit_mov_l_load_disp(&e->cg, 3, 1, md);
	sh4_emit_add_reg(&e->cg, 1, 0);
	if (md == JIT_MD_WRAM) {
		sh4_emit_mov_l_load_disp(&e->cg, 3, 2, JIT_MD_EWRAM_WORD);
		sh4_emit_mul_l(&e->cg, 7, 2);
		sh4_emit_mov_l_load_disp(&e->cg, 3, 1, store ? JIT_MD_EWRAM_STM : JIT_MD_EWRAM_LDM);
		sh4_emit_sts_macl(&e->cg, 2);
		sh4_emit_add_reg(&e->cg, 1, 2);
	} else {
		sh4_emit_mov_reg(&e->cg, 7, 2);
		if (!store) {
			sh4_emit_add_imm(&e->cg, 1, 2);
		}
	}
	if (stall) {
		uint8_t* bra = e->cg.ptr;
		sh4_emit_bra(&e->cg, 0);
		sh4_emit_nop(&e->cg);
		_patchBranch(bra, stallCode);
	} else {
		sh4_emit_add_reg(&e->cg, 2, R_CYCLES);
		sh4_emit_rts(&e->cg);
		sh4_emit_clrt(&e->cg);
	}
}

/* LDM/STM-like: r4 = the lowest address, r5 = opcode, r6 = offset, r7 = the
 * number of words. In IWRAM or EWRAM without wrapping (and for a store, no
 * code in the way): charges GBALoad/StoreMultiple's wait and returns T clear
 * with r0 = the host address for the caller to copy through. Otherwise runs
 * mGBA's handler for the whole instruction and returns T set (or stops, as
 * jit->handlers). */
static void _emitMultipleStub(struct JITEmitter* e, bool store, bool stall, bool thumb, uint8_t* stallCode) {
	struct JITFixups toSlow = { .n = 0 };
	struct JITFixups toIwram = { .n = 0 };
	e->jit->multipleStubs[stall][store][thumb] = e->cg.ptr;
	sh4_emit_mov_reg(&e->cg, 4, 0);
	sh4_emit_shlr16(&e->cg, 0);
	sh4_emit_shlr8(&e->cg, 0);
	_lit(e, (uint32_t) (uintptr_t) e->jit->memData, 3);
	sh4_emit_cmpeq_imm(&e->cg, REGION_WORKING_IRAM);
	_btTo(e, &toIwram);
	sh4_emit_cmpeq_imm(&e->cg, REGION_WORKING_RAM);
	_bfTo(e, &toSlow);
	_emitMultipleRegion(e, store, stall, stallCode, JIT_MD_WRAM, JIT_MD_CHUNKS_EWRAM, SIZE_WORKING_RAM, &toSlow);
	_branchHere(&toIwram, e->cg.ptr);
	_emitMultipleRegion(e, store, stall, stallCode, JIT_MD_IWRAM, JIT_MD_CHUNKS_IWRAM, SIZE_WORKING_IRAM, &toSlow);
	_branchHere(&toSlow, e->cg.ptr);
	_jumpTo(e, e->jit->handlers[thumb]);
	sh4_emit_nop(&e->cg);
	_flushPool(e, false);
}

static void _emitMemoryStubs(struct JITEmitter* e) {
	uint8_t* slowLoad = e->cg.ptr;
	_emitSlowLoad(e);
	uint8_t* slowStore = e->cg.ptr;
	_emitSlowStore(e);
	_flushPool(e, false);
	uint8_t* stallCode = e->cg.ptr;
	_emitStall(e);
	_flushPool(e, false);
	int op;
	for (op = 0; op < JIT_MEM_OPS; ++op) {
		_emitMemoryStub(e, op, false, stallCode, slowLoad, slowStore);
		_emitMemoryStub(e, op, true, stallCode, slowLoad, slowStore);
	}
	int i;
	for (i = 0; i < 8; ++i) {
		_emitMultipleStub(e, i & 1, (i >> 1) & 1, i >> 2, stallCode);
	}
}

static uint32_t _thumbHandler(struct ARMCore* cpu, uint32_t opcode, uint32_t pc);
static uint32_t _armHandler(struct ARMCore* cpu, uint32_t opcode, uint32_t pc);

/* jit->handlers[thumb]: r5 = opcode, r6 = offset as for the memory stubs.
 * The flags go to the cpsr and back around mGBA's handler. Returns if the
 * block can carry on; otherwise counts the instruction and leaves for
 * dispatchSync, or for C (commonRaw) if the flags can't be held. */
static void _emitHandlerStub(struct JITEmitter* e, bool thumb, uint8_t* commonRaw) {
	const void* fn = thumb ? (const void*) _thumbHandler : (const void*) _armHandler;
	e->jit->handlers[thumb] = e->cg.ptr;
	sh4_emit_sts_pr_dec(&e->cg, 15);
	sh4_emit_mov_l_store_dec(&e->cg, 6, 15);
	_emitFlagsOut(e, 1);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_NEXT_EVENT);
	sh4_emit_add_reg(&e->cg, R_CYCLES, 0);
	sh4_emit_mov_l_store_gbr(&e->cg, JIT_GBR_CYCLES);
	sh4_emit_add_reg(&e->cg, R_BASE, 6);
	ARMJITRegisterCall(e->jit, fn);
	_lit(e, (uint32_t) (uintptr_t) fn, 1);
	sh4_emit_jsr(&e->cg, 1);
	sh4_emit_mov_reg(&e->cg, R_CPU, 4);
	sh4_emit_mov_reg(&e->cg, 0, 3);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_CYCLES);
	sh4_emit_mov_reg(&e->cg, 0, R_CYCLES);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_NEXT_EVENT);
	sh4_emit_sub(&e->cg, 0, R_CYCLES);
	_emitFlagsIn(e);
	sh4_emit_mov_l_load_inc(&e->cg, 15, 6);
	sh4_emit_lds_pr_inc(&e->cg, 15);
	sh4_emit_tst(&e->cg, 3, 3);
	uint8_t* stop = e->cg.ptr;
	sh4_emit_bf(&e->cg, 0);
	sh4_emit_rts(&e->cg);
	sh4_emit_nop(&e->cg);
	_patchBranch(stop, e->cg.ptr);
	/* r12 += offset / length + 1 */
	if (thumb) {
		sh4_emit_shlr(&e->cg, 6);
	} else {
		sh4_emit_shlr2(&e->cg, 6);
	}
	sh4_emit_add_imm(&e->cg, 1, 6);
	sh4_emit_add_reg(&e->cg, 6, R_COUNT);
	sh4_emit_mov_reg(&e->cg, 3, 0);
	sh4_emit_cmpeq_imm(&e->cg, 2);
	uint8_t* raw = e->cg.ptr;
	sh4_emit_bt(&e->cg, 0);
	_jumpTo(e, e->jit->dispatchSync);
	sh4_emit_nop(&e->cg);
	_patchBranch(raw, e->cg.ptr);
	uint8_t* bra = e->cg.ptr;
	sh4_emit_bra(&e->cg, 0);
	sh4_emit_mov_imm(&e->cg, JIT_EXIT_SYNC, 1);
	_patchBranch(bra, commonRaw);
	_flushPool(e, false);
}

void ARMJITUpdateMemory(struct ARMJIT* jit) {
	struct GBA* gba = (struct GBA*) jit->cpu->master;
	struct GBAMemory* memory = &gba->memory;
	uint32_t* md = jit->memData;
	md[JIT_MD_WRAM] = (uint32_t) (uintptr_t) memory->wram;
	md[JIT_MD_IWRAM] = (uint32_t) (uintptr_t) memory->iwram;
	md[JIT_MD_ROM] = (uint32_t) (uintptr_t) memory->rom;
	md[JIT_MD_ROM_SIZE] = memory->rom ? memory->romSize : 0;
	md[JIT_MD_CHUNKS_EWRAM] = (uint32_t) (uintptr_t) &jit->chunks[0];
	md[JIT_MD_CHUNKS_IWRAM] = (uint32_t) (uintptr_t) &jit->chunks[JIT_EWRAM_CHUNKS];
	md[JIT_MD_EWRAM_LOAD32] = 2 + memory->waitstatesNonseq32[REGION_WORKING_RAM];
	md[JIT_MD_EWRAM_LOAD16] = 2 + memory->waitstatesNonseq16[REGION_WORKING_RAM];
	md[JIT_MD_EWRAM_STORE32] = 1 + memory->waitstatesNonseq32[REGION_WORKING_RAM];
	md[JIT_MD_EWRAM_STORE16] = 1 + memory->waitstatesNonseq16[REGION_WORKING_RAM];
	md[JIT_MD_ROM_LOAD32] = 2 + memory->waitstatesNonseq32[REGION_CART0];
	md[JIT_MD_ROM_LOAD16] = 2 + memory->waitstatesNonseq16[REGION_CART0];
	md[JIT_MD_EWRAM_STM] = memory->waitstatesSeq32[REGION_WORKING_RAM] - memory->waitstatesNonseq32[REGION_WORKING_RAM];
	md[JIT_MD_EWRAM_LDM] = md[JIT_MD_EWRAM_STM] + 1;
	md[JIT_MD_EWRAM_WORD] = 1 + memory->waitstatesSeq32[REGION_WORKING_RAM];
}

/* Once, at the start of the code buffer:
 *
 * enter(cpu, entry): saves what the C ABI says a callee keeps (r8-r14, PR)
 *   plus GBR, sets up GBR, r12-r14 and jumps to a block entry. Returns the
 *   number of guest instructions run, with jit->exit saying why it stopped.
 * exits[type]: r0 = the exit's argument. Puts cpu->cycles back and returns
 *   from enter.
 * lookup: r4 = key, r5 = exit type if it isn't in the hash. Jumps to the
 *   block's entry.
 * dispatchSync: lookup for the instruction gprs[PC] and the cpsr say is
 *   next, after mGBA code has moved the cpu. */
void ARMJITEmitStubs(struct ARMJIT* jit) {
	struct JITEmitter e;
	memset(&e, 0, sizeof(e));
	e.jit = jit;
	e.cg.ptr = jit->code + jit->codeUsed;
	e.cg.end = jit->code + jit->codeSize;
	int r;

	jit->enter = e.cg.ptr;
	sh4_emit_sts_pr_dec(&e.cg, 15);
	for (r = 8; r <= 14; ++r) {
		sh4_emit_mov_l_store_dec(&e.cg, r, 15);
	}
	sh4_emit_stc_gbr(&e.cg, 1);
	sh4_emit_mov_l_store_dec(&e.cg, 1, 15);
	sh4_emit_ldc_gbr(&e.cg, 4);
	sh4_emit_mov_reg(&e.cg, 4, R_CPU);
	_lit(&e, (uint32_t) (uintptr_t) jit->memStubs, R_STUBS);
	sh4_emit_mov_l_load_gbr(&e.cg, JIT_GBR_CYCLES);
	sh4_emit_mov_reg(&e.cg, 0, R_CYCLES);
	sh4_emit_mov_l_load_gbr(&e.cg, JIT_GBR_NEXT_EVENT);
	sh4_emit_sub(&e.cg, 0, R_CYCLES);
	sh4_emit_mov_imm(&e.cg, 0, R_COUNT);
	_emitFlagsIn(&e);
	sh4_emit_jmp(&e.cg, 5);
	sh4_emit_nop(&e.cg);

	/* r0 = argument, r1 = type */
	uint8_t* common = e.cg.ptr;
	sh4_emit_mov_reg(&e.cg, 0, 3);
	_emitFlagsOut(&e, 2);
	sh4_emit_mov_reg(&e.cg, 3, 0);
	/* the same with the cpsr's flags already right */
	uint8_t* commonRaw = e.cg.ptr;
	_lit(&e, (uint32_t) (uintptr_t) &jit->exit, 2);
	sh4_emit_mov_l_store(&e.cg, 0, 2);
	sh4_emit_add_imm(&e.cg, 4, 2);
	sh4_emit_mov_l_store(&e.cg, 1, 2);
	sh4_emit_mov_l_load_gbr(&e.cg, JIT_GBR_NEXT_EVENT);
	sh4_emit_add_reg(&e.cg, R_CYCLES, 0);
	sh4_emit_mov_l_store_gbr(&e.cg, JIT_GBR_CYCLES);
	sh4_emit_mov_l_load_inc(&e.cg, 15, 1);
	sh4_emit_ldc_gbr(&e.cg, 1);
	sh4_emit_mov_reg(&e.cg, R_COUNT, 0);
	for (r = 14; r >= 8; --r) {
		sh4_emit_mov_l_load_inc(&e.cg, 15, r);
	}
	sh4_emit_lds_pr_inc(&e.cg, 15);
	sh4_emit_rts(&e.cg);
	sh4_emit_nop(&e.cg);

	int type;
	for (type = 0; type < JIT_EXIT_MAX; ++type) {
		jit->exits[type] = e.cg.ptr;
		uint8_t* bra = e.cg.ptr;
		sh4_emit_bra(&e.cg, 0);
		sh4_emit_mov_imm(&e.cg, type, 1);
		_patchBranch(bra, common);
	}

	/* r4 = key, r5 = exit type on a miss */
	jit->lookup = e.cg.ptr;
	sh4_emit_mov_reg(&e.cg, 4, 1);
	sh4_emit_shll2(&e.cg, 1);
	_imm(&e, (JIT_HASH_SIZE - 1) << 3, 2);
	sh4_emit_and(&e.cg, 2, 1);
	_lit(&e, (uint32_t) (uintptr_t) jit->hash, 2);
	sh4_emit_add_reg(&e.cg, 2, 1);
	sh4_emit_mov_l_load_inc(&e.cg, 1, 2);
	sh4_emit_cmpeq(&e.cg, 4, 2);
	uint8_t* miss = e.cg.ptr;
	sh4_emit_bf(&e.cg, 0);
	sh4_emit_mov_l_load(&e.cg, 1, 1);
	sh4_emit_jmp(&e.cg, 1);
	sh4_emit_nop(&e.cg);
	_patchBranch(miss, e.cg.ptr);
	sh4_emit_mov_reg(&e.cg, 4, 0);
	uint8_t* bra = e.cg.ptr;
	sh4_emit_bra(&e.cg, 0);
	sh4_emit_mov_reg(&e.cg, 5, 1);
	_patchBranch(bra, common);

	jit->dispatchSync = e.cg.ptr;
	sh4_emit_mov_l_load_disp(&e.cg, R_CPU, 4, ARM_PC);
	sh4_emit_mov_l_load_gbr(&e.cg, JIT_GBR_CPSR);
	sh4_emit_tst_imm(&e.cg, 0x20);
	uint8_t* arm = e.cg.ptr;
	sh4_emit_bt(&e.cg, 0);
	sh4_emit_add_imm(&e.cg, -WORD_SIZE_THUMB + 1, 4);
	bra = e.cg.ptr;
	sh4_emit_bra(&e.cg, 0);
	sh4_emit_mov_imm(&e.cg, JIT_EXIT_SYNC, 5);
	_patchBranch(bra, (uint8_t*) jit->lookup);
	_patchBranch(arm, e.cg.ptr);
	sh4_emit_add_imm(&e.cg, -WORD_SIZE_ARM, 4);
	bra = e.cg.ptr;
	sh4_emit_bra(&e.cg, 0);
	sh4_emit_mov_imm(&e.cg, JIT_EXIT_SYNC, 5);
	_patchBranch(bra, (uint8_t*) jit->lookup);
	_flushPool(&e, false);

	_emitHandlerStub(&e, true, commonRaw);
	_emitHandlerStub(&e, false, commonRaw);

	_emitMemoryStubs(&e);
	jit->codeUsed = ((e.cg.ptr - jit->code) + 31) & ~31;
#ifdef __sh__
	icache_sync_range((uintptr_t) jit->enter, e.cg.ptr - (const uint8_t*) jit->enter);
#endif
}

/* ---------------------------------------------------------------- */
/* Handler calls                                                     */
/* ---------------------------------------------------------------- */

static const uint16_t _conditionLut[16] = {
	0xF0F0, 0x0F0F, 0xCCCC, 0x3333, 0xFF00, 0x00FF, 0xAAAA, 0x5555,
	0x0C0C, 0xF3F3, 0xAA55, 0x55AA, 0x0A05, 0xF5FA, 0xFFFF, 0x0000
};

/* Run mGBA's handler for an instruction, with PC and prefetch as
 * ARMStep/ThumbStep leave them. Returns nonzero if the block must stop after
 * it: it moved the PC, pulled an event in (mode switch, halt, IRQ), or changed
 * the wait states the block was compiled with. */
static uint32_t _handlerEnd(struct ARMCore* cpu, uint32_t pc, int32_t nextEvent, uint32_t timingKey) {
	if (ARMJITTimingKey(cpu) != timingKey) {
		ARMJITTimingChanged(cpu->jit);
		return 1;
	}
	return (uint32_t) cpu->gprs[ARM_PC] != pc || cpu->nextEvent < nextEvent;
}

/* 2: the cpsr has N and Z both set, which r9 can't hold. */
static uint32_t _handlerResult(struct ARMCore* cpu, uint32_t stop) {
	if ((cpu->cpsr.packed >> 30) == 3) {
		return 2;
	}
	return stop;
}

static uint32_t _thumbHandler(struct ARMCore* cpu, uint32_t opcode, uint32_t pc) {
	HHIST(1, opcode);
	_setPC(cpu, pc);
	int32_t nextEvent = cpu->nextEvent;
	uint32_t timingKey = ARMJITTimingKey(cpu);
	_thumbTable[opcode >> 6](cpu, opcode);
	return _handlerResult(cpu, _handlerEnd(cpu, pc, nextEvent, timingKey));
}

static uint32_t _armHandler(struct ARMCore* cpu, uint32_t opcode, uint32_t pc) {
	HHIST(0, opcode);
	_setPC(cpu, pc);
	unsigned flags = cpu->cpsr.flags >> 4;
	if (!(_conditionLut[opcode >> 28] & (1 << flags))) {
		cpu->cycles += ARM_PREFETCH_CYCLES;
		return 0;
	}
	int32_t nextEvent = cpu->nextEvent;
	uint32_t timingKey = ARMJITTimingKey(cpu);
	_armTable[((opcode >> 16) & 0xFF0) | ((opcode >> 4) & 0x00F)](cpu, opcode);
	return _handlerResult(cpu, _handlerEnd(cpu, pc, nextEvent, timingKey));
}

/* Run mGBA's handler for the current instruction (jit->handlers). Returns
 * if the block can carry on. */
static void _handler(struct JITEmitter* e, uint32_t op) {
	_charge(e);
	_imm(e, op, 5);
	_lit(e, (uint32_t) (uintptr_t) e->jit->handlers[e->thumb], 1);
	sh4_emit_jsr(&e->cg, 1);
	sh4_emit_mov_imm(&e->cg, e->index * _insnLength(e), 6);
	e->usesBase = true;
}

/* r += value */
static void _addImmediate(struct JITEmitter* e, int32_t value, int r) {
	if (!value) {
		return;
	}
	if (value >= -128 && value <= 127) {
		sh4_emit_add_imm(&e->cg, value, r);
	} else {
		_imm(e, value, 1);
		sh4_emit_add_reg(&e->cg, 1, r);
	}
}

/* A 32-bit load from ROM at an address known now: the value can't change,
 * and the cycles don't depend on anything run-time (no stall above
 * BASE_CART0). Returns false if address isn't in ROM. */
static bool _romConstant(struct JITEmitter* e, uint32_t address, int rd) {
	struct GBA* gba = (struct GBA*) e->jit->cpu->master;
	uint32_t region = address >> BASE_OFFSET;
	if (region < REGION_CART0 || region > REGION_CART2_EX || (address & 3) ||
	    (address & (SIZE_CART0 - 1)) >= gba->memory.romSize) {
		return false;
	}
	uint32_t value;
	LOAD_32(value, address & (SIZE_CART0 - 4), gba->memory.rom);
	_imm(e, value, 1);
	_st(e, 1, rd);
	e->pending += 1 + e->nonseq16 + 2 + gba->memory.waitstatesNonseq32[region];
	return true;
}

/* ---------------------------------------------------------------- */
/* Thumb                                                             */
/* ---------------------------------------------------------------- */

/* Instructions after which straight-line code isn't worth compiling. */
static bool _thumbEndsBlock(uint32_t op) {
	if ((op & 0xF800) == 0xE000 || (op & 0xF000) == 0xD000 || (op & 0xF800) == 0xF800 ||
	    (op & 0xF800) == 0xE800 || (op & 0xFF00) == 0x4700 || (op & 0xFF00) == 0xBD00) {
		return true;
	}
	if ((op & 0xFF00) == 0x4400 || (op & 0xFF00) == 0x4600) {
		return ((op & 7) | ((op >> 4) & 8)) == ARM_PC;
	}
	return false;
}

/* Single stores, which end the block early if they pull an event in. */
static bool _thumbIsStore(uint32_t op) {
	switch (op >> 11) {
	case 0x0A:
	case 0x0B:
		return ((op >> 9) & 7) < 3;
	case 0x0C:
	case 0x0E:
	case 0x10:
	case 0x12:
		return true;
	default:
		return false;
	}
}

/* Whether op is translated natively, and the flags it writes and reads.
 * Handler calls count as reading every flag. */
static bool _thumbAnalyze(uint32_t op, unsigned* written, unsigned* read) {
	*written = 0;
	*read = 0;
	switch (op >> 11) {
	case 0x00:
		*written = (op & 0x07C0) ? F_NZC : F_NZ;
		return true;
	case 0x01:
	case 0x02:
		*written = F_NZC;
		return true;
	case 0x03:
	case 0x05:
	case 0x06:
	case 0x07:
		*written = F_ALL;
		return true;
	case 0x04:
		*written = F_NZ;
		return true;
	case 0x08:
		if (!(op & 0x0400)) {
			switch ((op >> 6) & 0xF) {
			case 0x0: // AND
			case 0x1: // EOR
			case 0x8: // TST
			case 0xC: // ORR
			case 0xE: // BIC
			case 0xF: // MVN
				*written = F_NZ;
				return true;
			case 0x5: // ADC
			case 0x6: // SBC
				*written = F_ALL;
				*read = F_C;
				return true;
			case 0x9: // NEG
			case 0xA: // CMP
			case 0xB: // CMN
				*written = F_ALL;
				return true;
			default: // register shifts, MUL
				*read = F_ALL;
				return false;
			}
		}
		switch ((op >> 8) & 3) {
		case 0: // ADD
		case 2: // MOV
			if (((op & 7) | ((op >> 4) & 8)) == ARM_PC) {
				*read = F_ALL;
				return false;
			}
			return true;
		case 1: // CMP
			*written = F_ALL;
			return true;
		default: // BX
			*read = F_ALL;
			return false;
		}
	case 0x09: // LDR PC-relative
	case 0x0A: // load/store register offset
	case 0x0B:
	case 0x0C: // load/store immediate offset
	case 0x0D:
	case 0x0E:
	case 0x0F:
	case 0x10: // LDRH/STRH immediate
	case 0x11:
	case 0x12: // load/store SP-relative
	case 0x13:
	case 0x14: // ADD PC
	case 0x15: // ADD SP
	case 0x1E: // BL prefix
		return true;
	case 0x16:
		if ((op & 0xFF00) == 0xB000) { // ADD/SUB SP
			return true;
		}
		/* fall through */
	case 0x17:
	case 0x18:
	case 0x19:
		/* PUSH/POP/STMIA/LDMIA with a list; may end the block */
		*read = F_ALL;
		return ((op & 0xF600) == 0xB400 || (op & 0xF000) == 0xC000) && (op & 0x1FF) &&
		       ((op & 0xF000) == 0xB000 || (op & 0xFF));
	default:
		*read = F_ALL;
		return false;
	}
}

static void _thumbShiftImmediate(struct JITEmitter* e, uint32_t op, bool flags) {
	int immediate = (op >> 6) & 0x1F;
	int rm = (op >> 3) & 7;
	int rd = op & 7;
	_ld(e, rm, 1);
	switch (op >> 11) {
	case 0x00: // LSL
		if (!immediate) {
			_st(e, 1, rd);
			if (flags) {
				_flagsNZ(e, 1);
			}
			return;
		}
		if (immediate > 1) {
			sh4_emit_mov_imm(&e->cg, immediate - 1, 2);
			sh4_emit_shld(&e->cg, 2, 1);
		}
		sh4_emit_shll(&e->cg, 1);
		break;
	case 0x01: // LSR
		if (!immediate) {
			sh4_emit_shll(&e->cg, 1);
			sh4_emit_mov_imm(&e->cg, 0, 1);
			break;
		}
		if (immediate > 1) {
			sh4_emit_mov_imm(&e->cg, 1 - immediate, 2);
			sh4_emit_shld(&e->cg, 2, 1);
		}
		sh4_emit_shlr(&e->cg, 1);
		break;
	default: // ASR
		if (!immediate) {
			/* T = sign; r1 - r1 - T is then 0 or -1 and leaves T alone */
			sh4_emit_shll(&e->cg, 1);
			sh4_emit_subc(&e->cg, 1, 1);
			break;
		}
		if (immediate > 1) {
			sh4_emit_mov_imm(&e->cg, 1 - immediate, 2);
			sh4_emit_shad(&e->cg, 2, 1);
		}
		sh4_emit_shar(&e->cg, 1);
		break;
	}
	_st(e, 1, rd);
	if (flags) {
		sh4_emit_movt(&e->cg, 3);
		_flagsNZC(e, 1, 3);
	}
}

static void _thumbAlu(struct JITEmitter* e, uint32_t op, bool flags) {
	int rd = op & 7;
	int rn = (op >> 3) & 7;
	int alu = (op >> 6) & 0xF;
	if (alu == 0x9) { // NEG
		sh4_emit_mov_imm(&e->cg, 0, 1);
		_ld(e, rn, 2);
		_addSub(e, true, rd, flags);
		return;
	}
	_ld(e, rd, 1);
	_ld(e, rn, 2);
	switch (alu) {
	case 0x0: // AND
		sh4_emit_and(&e->cg, 2, 1);
		break;
	case 0x1: // EOR
		sh4_emit_xor(&e->cg, 2, 1);
		break;
	case 0x5: // ADC
		_addCarry(e, false, rd, flags);
		return;
	case 0x6: // SBC
		_addCarry(e, true, rd, flags);
		return;
	case 0x8: // TST
		if (flags) {
			sh4_emit_and(&e->cg, 2, 1);
			_flagsNZ(e, 1);
		}
		return;
	case 0xA: // CMP
		_addSub(e, true, -1, flags);
		return;
	case 0xB: // CMN
		_addSub(e, false, -1, flags);
		return;
	case 0xC: // ORR
		sh4_emit_or(&e->cg, 2, 1);
		break;
	case 0xE: // BIC
		sh4_emit_not(&e->cg, 2, 2);
		sh4_emit_and(&e->cg, 2, 1);
		break;
	case 0xF: // MVN
		sh4_emit_not(&e->cg, 2, 1);
		break;
	}
	_st(e, 1, rd);
	if (flags) {
		_flagsNZ(e, 1);
	}
}

static void _thumbTranslate(struct JITEmitter* e, uint32_t op, bool flags) {
	int rd;
	switch (op >> 11) {
	case 0x00:
	case 0x01:
	case 0x02:
		_thumbShiftImmediate(e, op, flags);
		break;
	case 0x03: // ADD/SUB register or 3-bit immediate
		_ld(e, (op >> 3) & 7, 1);
		if (op & 0x0400) {
			sh4_emit_mov_imm(&e->cg, (op >> 6) & 7, 2);
		} else {
			_ld(e, (op >> 6) & 7, 2);
		}
		_addSub(e, op & 0x0200, op & 7, flags);
		break;
	case 0x04: // MOV immediate
		rd = (op >> 8) & 7;
		_imm(e, op & 0xFF, 1);
		_st(e, 1, rd);
		if (flags) {
			_flagsNZ(e, 1);
		}
		break;
	case 0x05: // CMP immediate
	case 0x06: // ADD immediate
	case 0x07: // SUB immediate
		rd = (op >> 8) & 7;
		_ld(e, rd, 1);
		_imm(e, op & 0xFF, 2);
		_addSub(e, (op >> 11) != 0x06, (op >> 11) == 0x05 ? -1 : rd, flags);
		break;
	case 0x08:
		if (!(op & 0x0400)) {
			_thumbAlu(e, op, flags);
			break;
		}
		rd = (op & 7) | ((op >> 4) & 8);
		switch ((op >> 8) & 3) {
		case 0: // ADD, no flags
			_ld(e, rd, 1);
			_ld(e, (op >> 3) & 0xF, 2);
			sh4_emit_add_reg(&e->cg, 2, 1);
			_st(e, 1, rd);
			break;
		case 1: // CMP
			_ld(e, rd, 1);
			_ld(e, (op >> 3) & 0xF, 2);
			_addSub(e, true, -1, flags);
			break;
		case 2: // MOV, no flags
			_ld(e, (op >> 3) & 0xF, 1);
			_st(e, 1, rd);
			break;
		}
		break;
	case 0x09: { // LDR Rd, [PC, #imm]
		uint32_t address = (_pcValue(e) & ~3) + ((op & 0xFF) << 2);
		if (!_romConstant(e, address, (op >> 8) & 7)) {
			_imm(e, address, 4);
			_memory(e, JIT_MEM_LOAD32, (op >> 8) & 7);
		}
		break;
	}
	case 0x0A:
	case 0x0B: { // load/store register offset
		static const enum JITMemOp ops[8] = {
			JIT_MEM_STORE32, JIT_MEM_STORE16, JIT_MEM_STORE8, JIT_MEM_LOADS8,
			JIT_MEM_LOAD32, JIT_MEM_LOAD16, JIT_MEM_LOAD8, JIT_MEM_LOADS16
		};
		_ld(e, (op >> 3) & 7, 4);
		_ld(e, (op >> 6) & 7, 1);
		sh4_emit_add_reg(&e->cg, 1, 4);
		_memory(e, ops[(op >> 9) & 7], op & 7);
		break;
	}
	case 0x0C: // STR/LDR immediate
	case 0x0D:
	case 0x0E: // STRB/LDRB immediate
	case 0x0F:
	case 0x10: // STRH/LDRH immediate
	case 0x11: {
		int immediate = (op >> 6) & 0x1F;
		enum JITMemOp mop;
		bool store = !(op & 0x0800);
		switch (op >> 12) {
		case 0x6:
			immediate <<= 2;
			mop = store ? JIT_MEM_STORE32 : JIT_MEM_LOAD32;
			break;
		case 0x7:
			mop = store ? JIT_MEM_STORE8 : JIT_MEM_LOAD8;
			break;
		default:
			immediate <<= 1;
			mop = store ? JIT_MEM_STORE16 : JIT_MEM_LOAD16;
			break;
		}
		_ld(e, (op >> 3) & 7, 4);
		if (immediate) {
			sh4_emit_add_imm(&e->cg, immediate, 4);
		}
		_memory(e, mop, op & 7);
		break;
	}
	case 0x12: // STR/LDR SP-relative
	case 0x13:
		_ld(e, ARM_SP, 4);
		_addImmediate(e, (op & 0xFF) << 2, 4);
		_memory(e, (op & 0x0800) ? JIT_MEM_LOAD32 : JIT_MEM_STORE32, (op >> 8) & 7);
		break;
	case 0x14: // ADD Rd, PC, #imm
		_imm(e, (_pcValue(e) & ~3) + ((op & 0xFF) << 2), 1);
		_st(e, 1, (op >> 8) & 7);
		break;
	case 0x15: // ADD Rd, SP, #imm
		_ld(e, ARM_SP, 1);
		_imm(e, (op & 0xFF) << 2, 2);
		sh4_emit_add_reg(&e->cg, 2, 1);
		_st(e, 1, (op >> 8) & 7);
		break;
	case 0x16: // ADD/SUB SP, #imm
		_ld(e, ARM_SP, 1);
		_imm(e, (op & 0x0080) ? -((op & 0x7F) << 2) : (op & 0x7F) << 2, 2);
		sh4_emit_add_reg(&e->cg, 2, 1);
		_st(e, 1, ARM_SP);
		break;
	case 0x1E: { // BL prefix: LR = PC + (offset << 12)
		int32_t offset = (int32_t) ((op & 0x07FF) << 21) >> 9;
		_imm(e, _pcValue(e) + offset, 1);
		_st(e, 1, ARM_LR);
		break;
	}
	}
	/* Everything above except the loads/stores (charged in _memory) is
	 * THUMB_PREFETCH_CYCLES. */
	if (!((op >> 11) >= 0x09 && (op >> 11) <= 0x13)) {
		e->pending += 1 + e->seq16;
	}
}

/* ---------------------------------------------------------------- */
/* Block compile                                                     */
/* ---------------------------------------------------------------- */

static bool _armEndsBlock(uint32_t op) {
	if ((op & 0x0E000000) == 0x0A000000 || (op & 0x0F000000) == 0x0F000000 ||
	    (op & 0x0FFFFFF0) == 0x012FFF10 || (op & 0x0DB0F000) == 0x0120F000) {
		return true;
	}
	if ((op & 0x0E000000) == 0x0C000000 || (op & 0x0F000000) == 0x0E000000 ||
	    (op & 0x0E000010) == 0x06000010) {
		return true;
	}
	if ((op & 0x0E108000) == 0x08108000) {
		return true;
	}
	if ((op & 0x0C000000) == 0 || (op & 0x0C100000) == 0x04100000) {
		return ((op >> 12) & 0xF) == ARM_PC;
	}
	return false;
}

static inline uint32_t _read(const uint8_t* p, bool thumb) {
	if (thumb) {
		return p[0] | (p[1] << 8);
	}
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24);
}

/* What the wait states the blocks were compiled with depend on. */
uint32_t ARMJITTimingKey(const struct ARMCore* cpu) {
	const struct GBA* gba = (const struct GBA*) cpu->master;
	return gba->memory.io[REG_WAITCNT >> 1] | ((uint32_t) gba->memory.waitstatesNonseq16[REGION_WORKING_RAM] << 16) |
	       ((uint32_t) gba->memory.waitstatesSeq16[REGION_WORKING_RAM] << 24);
}

/* ---------------------------------------------------------------- */
/* Branches                                                          */
/* ---------------------------------------------------------------- */

/* What ThumbWritePC/ARMWritePC charge for a branch within the region. */
static uint32_t _writePCCycles(const struct JITEmitter* e) {
	return e->thumb ? 2 + e->nonseq16 + e->seq16 : 2 + e->nonseq32 + e->seq32;
}

/* The prefetch buffer model forgets what it had fetched on a branch, when
 * the code runs from ROM with the buffer on. */
static void _branchForgetsPrefetch(struct JITEmitter* e) {
	struct GBA* gba = (struct GBA*) e->jit->cpu->master;
	if (e->region < REGION_CART0 || !gba->memory.prefetch) {
		return;
	}
	_lit(e, (uint32_t) (uintptr_t) &gba->memory.lastPrefetchedPc, 1);
	sh4_emit_mov_imm(&e->cg, 0, 0);
	sh4_emit_mov_l_store(&e->cg, 0, 1);
}

/* Whether generated code may branch straight to target itself: the region
 * doesn't change, so all mGBA's setActiveRegion would do is bookkeeping,
 * and it isn't the idle loop, which mGBA has to see being entered. */
static bool _linkable(const struct JITEmitter* e, uint32_t target) {
	const struct GBA* gba = (const struct GBA*) e->jit->cpu->master;
	if ((target >> BASE_OFFSET) != e->region) {
		return false;
	}
	if (gba->idleOptimization >= IDLE_LOOP_REMOVE && target == gba->idleLoop) {
		return false;
	}
	if (e->region >= REGION_CART0 && (target & (SIZE_CART0 - 1)) >= gba->memory.romSize) {
		return false;
	}
	return true;
}

/* The block ends with a branch to target in the same mode; the branch
 * instruction's own cost is already pending. */
static void _branch(struct JITEmitter* e, uint32_t target, int executed) {
	uint32_t key = JIT_KEY(target, e->thumb);
	if (_linkable(e, target)) {
		e->pending += _writePCCycles(e);
		_charge(e);
		_branchForgetsPrefetch(e);
		_count(e, executed);
		_site(e, key, JIT_EXIT_SITE_BRANCH);
	} else {
		_charge(e);
		_count(e, executed);
		_imm(e, key, 0);
		_jumpTo(e, e->jit->exits[JIT_EXIT_BRANCH]);
		sh4_emit_nop(&e->cg);
	}
}

/* Running off the end of the block into the next instruction. */
static void _fallThrough(struct JITEmitter* e, uint32_t next, int executed) {
	_charge(e);
	_count(e, executed);
	_site(e, JIT_KEY(next, e->thumb), JIT_EXIT_SITE_FALL);
}

/* T from the flags for Thumb/ARM condition cond (0-13). Returns whether the
 * condition holds when T is set (otherwise when it is clear). */
static bool _condition(struct JITEmitter* e, int cond) {
	switch (cond) {
	case 0x0: // EQ
	case 0x1: // NE
		sh4_emit_tst(&e->cg, R_NZ, R_NZ);
		return !(cond & 1);
	case 0x2: // CS
	case 0x3: // CC
		sh4_emit_cmppz(&e->cg, R_CV);
		return cond & 1;
	case 0x4: // MI
	case 0x5: // PL
		sh4_emit_cmppz(&e->cg, R_NZ);
		return cond & 1;
	case 0x6: // VS
	case 0x7: // VC
		sh4_emit_mov_reg(&e->cg, R_CV, 0);
		sh4_emit_shll(&e->cg, 0);
		sh4_emit_cmppz(&e->cg, 0);
		return cond & 1;
	case 0x8: // HI
	case 0x9: // LS
		/* T = !C || Z */
		sh4_emit_cmppz(&e->cg, R_CV);
		sh4_emit_bt(&e->cg, 0);
		sh4_emit_tst(&e->cg, R_NZ, R_NZ);
		return cond & 1;
	default: // GE, LT, GT, LE
		/* T = N != V, or Z for GT/LE */
		if (cond >= 0xC) {
			sh4_emit_tst(&e->cg, R_NZ, R_NZ);
			sh4_emit_bt(&e->cg, 3);
		}
		sh4_emit_mov_reg(&e->cg, R_CV, 0);
		sh4_emit_shll(&e->cg, 0);
		sh4_emit_xor(&e->cg, R_NZ, 0);
		sh4_emit_shll(&e->cg, 0);
		return cond & 1;
	}
}

static void _thumbB(struct JITEmitter* e, uint32_t op, int executed) {
	int32_t offset = (int32_t) ((op & 0x07FF) << 21) >> 20;
	e->pending += 1 + e->seq16;
	_branch(e, _pcValue(e) + offset, executed);
}

static void _thumbBcc(struct JITEmitter* e, uint32_t op, int executed) {
	int32_t offset = (int8_t) op * 2;
	e->pending += 1 + e->seq16;
	_charge(e);
	bool takenIfT = _condition(e, (op >> 8) & 0xF);
	uint8_t* skip = e->cg.ptr;
	if (takenIfT) {
		sh4_emit_bt(&e->cg, 0);
	} else {
		sh4_emit_bf(&e->cg, 0);
	}
	_fallThrough(e, e->address + WORD_SIZE_THUMB, executed);
	_patchBranch(skip, e->cg.ptr);
	_branch(e, _pcValue(e) + offset, executed);
}

/* BL as the pair it nearly always is: LR = the instruction after the
 * second half, | 1. */
static void _thumbBL(struct JITEmitter* e, uint32_t op1, uint32_t op2, int executed) {
	int32_t high = (int32_t) ((op1 & 0x07FF) << 21) >> 9;
	uint32_t target = _pcValue(e) + high + ((op2 & 0x07FF) << 1);
	_imm(e, (e->address + 2 * WORD_SIZE_THUMB) | 1, 1);
	_st(e, 1, ARM_LR);
	e->pending += 2 * (1 + e->seq16);
	e->address += WORD_SIZE_THUMB;
	_branch(e, target, executed);
}

/* BX Rm: within the region and staying in Thumb, look the target up here;
 * anything else is mGBA's. */
static void _thumbBX(struct JITEmitter* e, uint32_t op, int executed) {
	_charge(e);
	_ld(e, (op >> 3) & 0xF, 4);
	_imm(e, 0xFF000001, 1);
	sh4_emit_mov_reg(&e->cg, 4, 2);
	sh4_emit_and(&e->cg, 1, 2);
	_imm(e, (e->region << BASE_OFFSET) | 1, 1);
	sh4_emit_cmpeq(&e->cg, 1, 2);
	uint8_t* slow = e->cg.ptr;
	sh4_emit_bf(&e->cg, 0);
	e->pending += 1 + e->seq16 + _writePCCycles(e);
	_charge(e);
	_branchForgetsPrefetch(e);
	_count(e, executed);
	_jumpTo(e, e->jit->lookup);
	sh4_emit_mov_imm(&e->cg, JIT_EXIT_LOOKUP, 5);
	_patchBranch(slow, e->cg.ptr);
	_handler(e, op);
	_count(e, executed);
	_jumpTo(e, e->jit->dispatchSync);
	sh4_emit_nop(&e->cg);
}

/* ---------------------------------------------------------------- */
/* Block compile                                                     */
/* ---------------------------------------------------------------- */

/* ---------------------------------------------------------------- */
/* Load/store multiple                                               */
/* ---------------------------------------------------------------- */

/* LDM/STM, PUSH/POP: rn's value is the base, the words run up (IA/IB) or
 * down (DA/DB) from it, before the first (IB/DB) or not. mask may include
 * PC for a load, which ends the block. */
static void _multiple(struct JITEmitter* e, uint32_t op, int rn, unsigned mask, bool store, bool up, bool before,
                      bool writeback, int executed) {
	int n = popcount32(mask);
	int32_t lowest = up ? (before ? 4 : 0) : (before ? -4 * n : 4 - 4 * n);
	int r;
	_charge(e);
	_ld(e, rn, 4);
	_addImmediate(e, lowest, 4);
	_imm(e, op, 5);
	sh4_emit_mov_imm(&e->cg, n, 7);
	_lit(e, (uint32_t) (uintptr_t) e->jit->multipleStubs[e->stall][store][e->thumb], 1);
	sh4_emit_jsr(&e->cg, 1);
	sh4_emit_mov_imm(&e->cg, e->index * _insnLength(e), 6);
	e->usesBase = true;
	uint8_t* slow = e->cg.ptr;
	sh4_emit_bt(&e->cg, 0);
	if (store) {
		sh4_emit_add_imm(&e->cg, 4 * n, 0);
		for (r = 15; r >= 0; --r) {
			if (mask & (1 << r)) {
				_ld(e, r, 1);
				sh4_emit_mov_l_store_dec(&e->cg, 1, 0);
			}
		}
	} else {
		for (r = 0; r < 16; ++r) {
			if (mask & (1 << r)) {
				sh4_emit_mov_l_load_inc(&e->cg, 0, r == ARM_PC ? 4 : 1);
				if (r != ARM_PC) {
					_st(e, 1, r);
				}
			}
		}
	}
	if (writeback && (store || !(mask & (1 << rn)))) {
		_ld(e, rn, 1);
		_addImmediate(e, up ? 4 * n : -4 * n, 1);
		_st(e, 1, rn);
	}
	/* the prefetch's 1S becomes 1N, as _memory */
	int32_t cost = 1 + (e->thumb ? e->nonseq16 : e->nonseq32);
	if (!(mask & (1 << ARM_PC))) {
		_addCycles(e, cost);
	} else {
		/* ThumbWritePC/ARMWritePC: Thumb stays Thumb (no interworking on
		 * ARMv4T), and the low bits go */
		if (e->thumb) {
			sh4_emit_mov_reg(&e->cg, 4, 0);
			sh4_emit_or_imm(&e->cg, 1);
			sh4_emit_mov_reg(&e->cg, 0, 4);
		} else {
			sh4_emit_shlr2(&e->cg, 4);
			sh4_emit_shll2(&e->cg, 4);
		}
		sh4_emit_mov_reg(&e->cg, 4, 0);
		sh4_emit_shlr16(&e->cg, 0);
		sh4_emit_shlr8(&e->cg, 0);
		sh4_emit_cmpeq_imm(&e->cg, e->region);
		uint8_t* other = e->cg.ptr;
		sh4_emit_bf(&e->cg, 0);
		e->pending = cost + _writePCCycles(e);
		_charge(e);
		_branchForgetsPrefetch(e);
		_count(e, executed);
		_jumpTo(e, e->jit->lookup);
		sh4_emit_mov_imm(&e->cg, JIT_EXIT_LOOKUP, 5);
		_patchBranch(other, e->cg.ptr);
		e->pending = cost;
		_charge(e);
		_count(e, executed);
		sh4_emit_mov_reg(&e->cg, 4, 0);
		_jumpTo(e, e->jit->exits[JIT_EXIT_BRANCH]);
		sh4_emit_nop(&e->cg);
	}
	if (!sh4_disp8_fits(sh4_branch_disp8((uintptr_t) slow, (uintptr_t) e->cg.ptr))) {
		e->cg.overflow = 1;
	}
	_patchBranch(slow, e->cg.ptr);
}

/* Thumb PUSH/POP/LDMIA/STMIA with a non-empty list, else false. */
static bool _thumbMultiple(struct JITEmitter* e, uint32_t op, int executed) {
	unsigned mask = op & 0xFF;
	switch (op >> 8) {
	case 0xB4: // PUSH
	case 0xB5:
		mask |= (op & 0x100) ? 1 << ARM_LR : 0;
		if (!mask) {
			return false;
		}
		_multiple(e, op, ARM_SP, mask, true, false, true, true, executed);
		return true;
	case 0xBC: // POP
	case 0xBD:
		mask |= (op & 0x100) ? 1 << ARM_PC : 0;
		if (!mask) {
			return false;
		}
		_multiple(e, op, ARM_SP, mask, false, true, false, true, executed);
		return true;
	default:
		if ((op & 0xF000) != 0xC000 || !mask) {
			return false;
		}
		_multiple(e, op, (op >> 8) & 7, mask, !(op & 0x0800), true, false, true, executed);
		return true;
	}
}

/* ARM LDM/STM translated natively: no S bit, a list, rn not PC, and PC only
 * in a load's list. */
static bool _armMultipleNative(uint32_t op) {
	if ((op & 0x0E000000) != 0x08000000 || (op & 0x00400000) || !(op & 0xFFFF)) {
		return false;
	}
	if (((op >> 16) & 0xF) == ARM_PC) {
		return false;
	}
	return (op & 0x00100000) || !(op & 0x8000);
}

static void _armMultiple(struct JITEmitter* e, uint32_t op, int executed) {
	_multiple(e, op, (op >> 16) & 0xF, op & 0xFFFF, !(op & 0x00100000), op & 0x00800000, op & 0x01000000,
	          op & 0x00200000, executed);
}

/* ---------------------------------------------------------------- */
/* ARM                                                               */
/* ---------------------------------------------------------------- */

enum {
	CARRY_KEEP,
	CARRY_T,
	CARRY_0,
	CARRY_1
};

/* r <<= n, r >>= n (logical or arithmetic) for 1 <= n <= 31; r0 is scratch. */
static void _shiftConst(struct JITEmitter* e, int r, int n, int kind) {
	switch (kind) {
	case 0:
		switch (n) {
		case 1: sh4_emit_shll(&e->cg, r); return;
		case 2: sh4_emit_shll2(&e->cg, r); return;
		case 8: sh4_emit_shll8(&e->cg, r); return;
		case 16: sh4_emit_shll16(&e->cg, r); return;
		}
		sh4_emit_mov_imm(&e->cg, n, 0);
		sh4_emit_shld(&e->cg, 0, r);
		return;
	case 1:
		switch (n) {
		case 1: sh4_emit_shlr(&e->cg, r); return;
		case 2: sh4_emit_shlr2(&e->cg, r); return;
		case 8: sh4_emit_shlr8(&e->cg, r); return;
		case 16: sh4_emit_shlr16(&e->cg, r); return;
		}
		sh4_emit_mov_imm(&e->cg, -n, 0);
		sh4_emit_shld(&e->cg, 0, r);
		return;
	default:
		if (n == 1) {
			sh4_emit_shar(&e->cg, r);
			return;
		}
		sh4_emit_mov_imm(&e->cg, -n, 0);
		sh4_emit_shad(&e->cg, 0, r);
		return;
	}
}

/* The shift-by-immediate forms (addressing modes 1 and 2) of guest rm into
 * host dst, as mGBA's _shift* and ADDR_MODE_2_* compute them. With carry,
 * says where the shifter carry-out is (CARRY_T: in T). r0 and r3 are
 * scratch. */
static int _armShiftImmediate(struct JITEmitter* e, uint32_t op, int dst, bool carry) {
	int shift = (op >> 7) & 0x1F;
	_ld(e, op & 0xF, dst);
	switch ((op >> 5) & 3) {
	case 0: // LSL
		if (!shift) {
			return CARRY_KEEP;
		}
		if (!carry) {
			_shiftConst(e, dst, shift, 0);
			return CARRY_KEEP;
		}
		if (shift > 1) {
			_shiftConst(e, dst, shift - 1, 0);
		}
		sh4_emit_shll(&e->cg, dst);
		return CARRY_T;
	case 1: // LSR; #0 means #32
		if (!shift) {
			sh4_emit_shll(&e->cg, dst);
			sh4_emit_mov_imm(&e->cg, 0, dst);
			return CARRY_T;
		}
		if (!carry) {
			_shiftConst(e, dst, shift, 1);
			return CARRY_KEEP;
		}
		if (shift > 1) {
			_shiftConst(e, dst, shift - 1, 1);
		}
		sh4_emit_shlr(&e->cg, dst);
		return CARRY_T;
	case 2: // ASR; #0 means #32
		if (!shift) {
			sh4_emit_shll(&e->cg, dst);
			sh4_emit_subc(&e->cg, dst, dst);
			return CARRY_T;
		}
		if (!carry) {
			_shiftConst(e, dst, shift, 2);
			return CARRY_KEEP;
		}
		if (shift > 1) {
			_shiftConst(e, dst, shift - 1, 2);
		}
		sh4_emit_shar(&e->cg, dst);
		return CARRY_T;
	default: // ROR; #0 is RRX
		if (!shift) {
			_carryToT(e);
			sh4_emit_rotcr(&e->cg, dst);
			return CARRY_T;
		}
		sh4_emit_mov_reg(&e->cg, dst, 3);
		_shiftConst(e, dst, shift, 1);
		_shiftConst(e, 3, 32 - shift, 0);
		sh4_emit_or(&e->cg, 3, dst);
		if (!carry) {
			return CARRY_KEEP;
		}
		_signToT(e, dst);
		return CARRY_T;
	}
}

static bool _armIsLogical(int alu) {
	switch (alu) {
	case 0x0: // AND
	case 0x1: // EOR
	case 0x8: // TST
	case 0x9: // TEQ
	case 0xC: // ORR
	case 0xD: // MOV
	case 0xE: // BIC
	case 0xF: // MVN
		return true;
	default:
		return false;
	}
}

static void _armDataProcessing(struct JITEmitter* e, uint32_t op, bool flags) {
	int alu = (op >> 21) & 0xF;
	int rd = (op >> 12) & 0xF;
	int rn = (op >> 16) & 0xF;
	bool logical = _armIsLogical(alu);
	bool reverse = alu == 0x3 || alu == 0x7; // RSB, RSC
	int dst = reverse ? 1 : 2;
	int src = reverse ? 2 : 1;
	flags = flags && (op & 0x00100000);
	int carry;
	if (op & 0x02000000) {
		int rotate = (op >> 7) & 0x1E;
		uint32_t value = ROR(op & 0xFF, rotate);
		_imm(e, value, dst);
		carry = !rotate ? CARRY_KEEP : value >> 31 ? CARRY_1 : CARRY_0;
	} else {
		carry = _armShiftImmediate(e, op, dst, flags && logical);
	}
	if (flags && logical) {
		if (carry == CARRY_T) {
			sh4_emit_movt(&e->cg, 3);
		} else if (carry != CARRY_KEEP) {
			sh4_emit_mov_imm(&e->cg, carry == CARRY_1, 3);
		}
	}
	if (alu != 0xD && alu != 0xF) {
		_ld(e, rn, src);
	}
	switch (alu) {
	case 0x0: // AND
	case 0x8: // TST
		sh4_emit_and(&e->cg, 2, 1);
		break;
	case 0x1: // EOR
	case 0x9: // TEQ
		sh4_emit_xor(&e->cg, 2, 1);
		break;
	case 0x2: // SUB
	case 0x3: // RSB
		_addSub(e, true, rd, flags);
		break;
	case 0x4: // ADD
		_addSub(e, false, rd, flags);
		break;
	case 0x5: // ADC
		_addCarry(e, false, rd, flags);
		break;
	case 0x6: // SBC
	case 0x7: // RSC
		_addCarry(e, true, rd, flags);
		break;
	case 0xA: // CMP
		_addSub(e, true, -1, flags);
		break;
	case 0xB: // CMN
		_addSub(e, false, -1, flags);
		break;
	case 0xC: // ORR
		sh4_emit_or(&e->cg, 2, 1);
		break;
	case 0xD: // MOV
		sh4_emit_mov_reg(&e->cg, 2, 1);
		break;
	case 0xE: // BIC
		sh4_emit_not(&e->cg, 2, 2);
		sh4_emit_and(&e->cg, 2, 1);
		break;
	case 0xF: // MVN
		sh4_emit_not(&e->cg, 2, 1);
		break;
	}
	if (logical) {
		if (alu < 0x8 || alu > 0xB) {
			_st(e, 1, rd);
		}
		if (flags) {
			if (carry == CARRY_KEEP) {
				_flagsNZ(e, 1);
			} else {
				_flagsNZC(e, 1, 3);
			}
		}
	}
	e->pending += 1 + e->seq32;
}

/* r1 = the offset of a single load/store: immediate, or rm shifted. */
static void _armOffset(struct JITEmitter* e, uint32_t op, bool mode3) {
	if (mode3) {
		if (op & 0x00400000) {
			_imm(e, ((op >> 4) & 0xF0) | (op & 0xF), 1);
		} else {
			_ld(e, op & 0xF, 1);
		}
	} else if (!(op & 0x02000000)) {
		_imm(e, op & 0xFFF, 1);
	} else {
		_armShiftImmediate(e, op, 1, false);
	}
}

/* LDR/STR/LDRB/STRB and the halfword/signed forms. Writeback happens before
 * the access either way: a load overwrites it if rd == rn, as in mGBA, and
 * nothing a store can reach looks at rn. */
static void _armLoadStore(struct JITEmitter* e, uint32_t op, bool mode3) {
	int rn = (op >> 16) & 0xF;
	int rd = (op >> 12) & 0xF;
	bool pre = op & 0x01000000;
	bool up = op & 0x00800000;
	bool writeback = !pre || (op & 0x00200000);
	bool load = op & 0x00100000;
	enum JITMemOp mop;
	if (mode3) {
		switch ((op >> 5) & 3) {
		case 1:
			mop = load ? JIT_MEM_LOAD16 : JIT_MEM_STORE16;
			break;
		case 2:
			mop = JIT_MEM_LOADS8;
			break;
		default:
			mop = JIT_MEM_LOADS16;
			break;
		}
	} else if (op & 0x00400000) {
		mop = load ? JIT_MEM_LOAD8 : JIT_MEM_STORE8;
	} else {
		mop = load ? JIT_MEM_LOAD32 : JIT_MEM_STORE32;
	}
	if (!load) {
		_ld(e, rd, 5);
	}
	_armOffset(e, op, mode3);
	_ld(e, rn, 4);
	if (pre) {
		if (up) {
			sh4_emit_add_reg(&e->cg, 1, 4);
		} else {
			sh4_emit_sub(&e->cg, 1, 4);
		}
		if (writeback) {
			_st(e, 4, rn);
		}
	} else {
		sh4_emit_mov_reg(&e->cg, 4, 2);
		if (up) {
			sh4_emit_add_reg(&e->cg, 1, 2);
		} else {
			sh4_emit_sub(&e->cg, 1, 2);
		}
		_st(e, 2, rn);
	}
	_memory(e, mop, load ? rd : -1);
}

/* MUL/MLA without S, outside ROM-with-prefetch code (where the wait goes
 * through the stall model). */
static void _armMultiply(struct JITEmitter* e, uint32_t op) {
	int rd = (op >> 16) & 0xF;
	bool accumulate = op & 0x00200000;
	_ld(e, op & 0xF, 1);
	_ld(e, (op >> 8) & 0xF, 2);
	sh4_emit_mul_l(&e->cg, 2, 1);
	sh4_emit_sts_macl(&e->cg, 1);
	if (accumulate) {
		_ld(e, (op >> 12) & 0xF, 3);
		sh4_emit_add_reg(&e->cg, 3, 1);
	}
	_st(e, 1, rd);
	/* ARM_WAIT_SMUL: 1-4 by how many top bytes of rs are all sign */
	_charge(e);
	sh4_emit_mov_reg(&e->cg, 2, 0);
	sh4_emit_shll(&e->cg, 0);
	sh4_emit_subc(&e->cg, 3, 3);
	sh4_emit_xor(&e->cg, 3, 2);
	sh4_emit_add_imm(&e->cg, accumulate ? 2 : 1, R_CYCLES);
	uint8_t* done[3];
	int i;
	for (i = 0; i < 3; ++i) {
		sh4_emit_shlr8(&e->cg, 2);
		sh4_emit_tst(&e->cg, 2, 2);
		done[i] = e->cg.ptr;
		sh4_emit_bt(&e->cg, 0);
		sh4_emit_add_imm(&e->cg, 1, R_CYCLES);
	}
	for (i = 0; i < 3; ++i) {
		_patchBranch(done[i], e->cg.ptr);
	}
	e->pending += 1 + e->nonseq32;
}

/* Whether op is translated natively (B/BL/BX are handled by the block
 * loop), and the flags it writes and reads. */
static bool _armAnalyze(uint32_t op, bool stall, unsigned* written, unsigned* read) {
	unsigned cond = op >> 28;
	*written = 0;
	*read = cond != 0xE ? F_ALL : 0;
	if (cond == 0xF) {
		*read = F_ALL;
		return false;
	}
	int rd = (op >> 12) & 0xF;
	int rn = (op >> 16) & 0xF;
	int rm = op & 0xF;
	if ((op & 0x0FC000F0) == 0x00000090) { // MUL/MLA
		if ((op & 0x00100000) || stall || rn == ARM_PC || ((op & 0x00200000) && rd == ARM_PC)) {
			*read = F_ALL;
			return false;
		}
		return true;
	}
	if ((op & 0x0E000090) == 0x00000090) {
		if (!(op & 0x60) || (!(op & 0x00100000) && ((op >> 5) & 3) != 1)) { // SWP, MULL, LDRD/STRD
			*read = F_ALL;
			return false;
		}
		bool pre = op & 0x01000000;
		bool writeback = !pre || (op & 0x00200000);
		if (rd == ARM_PC || (!pre && (op & 0x00200000)) || (writeback && rn == ARM_PC) ||
		    (!(op & 0x00400000) && rm == ARM_PC)) {
			*read = F_ALL;
			return false;
		}
		return true;
	}
	if ((op & 0x0C000000) == 0) {
		int alu = (op >> 21) & 0xF;
		bool s = op & 0x00100000;
		if ((alu >= 0x8 && alu <= 0xB && !s) || rd == ARM_PC || (op & 0x02000010) == 0x00000010) {
			*read = F_ALL;
			return false;
		}
		if (alu == 0x5 || alu == 0x6 || alu == 0x7) {
			*read |= F_C;
		}
		if (!(op & 0x02000000) && (op & 0x00000FE0) == 0x00000060) { // RRX
			*read |= F_C;
		}
		if (s) {
			if (!_armIsLogical(alu)) {
				*written = F_ALL;
			} else if ((op & 0x02000000) ? (op & 0xF00) != 0 : ((op >> 5) & 3) != 0 || (op & 0xF80)) {
				*written = F_NZC;
			} else {
				*written = F_NZ;
			}
		}
		return true;
	}
	if ((op & 0x0C000000) == 0x04000000) {
		bool pre = op & 0x01000000;
		bool writeback = !pre || (op & 0x00200000);
		if ((op & 0x02000010) == 0x02000010 || rd == ARM_PC || (!pre && (op & 0x00200000)) ||
		    (writeback && rn == ARM_PC) || ((op & 0x02000000) && rm == ARM_PC)) {
			*read = F_ALL;
			return false;
		}
		if ((op & 0x02000000) && (op & 0x00000FE0) == 0x00000060) { // RRX offset
			*read = F_ALL;
			return false;
		}
		return true;
	}
	*read = F_ALL;
	return _armMultipleNative(op);
}

static bool _armIsMemory(uint32_t op) {
	return (op & 0x0C000000) == 0x04000000 || (op & 0x0E000000) == 0x08000000 ||
	       ((op & 0x0E000090) == 0x00000090 && (op & 0x60));
}

static void _armTranslate(struct JITEmitter* e, uint32_t op, bool flags) {
	if ((op & 0x0E000000) == 0x08000000) {
		_armMultiple(e, op, e->index + 1);
	} else if ((op & 0x0FC000F0) == 0x00000090) {
		_armMultiply(e, op);
	} else if ((op & 0x0E000090) == 0x00000090) {
		_armLoadStore(e, op, true);
	} else if ((op & 0x0C000000) == 0) {
		_armDataProcessing(e, op, flags);
	} else {
		_armLoadStore(e, op, false);
	}
}

/* A conditional instruction: the condition-failed cost (ARM_PREFETCH_CYCLES)
 * is charged up front and the body charges the difference, so both ways
 * fall through to the same place. */
static void _armConditional(struct JITEmitter* e, uint32_t op, bool native, bool flags) {
	int cond = op >> 28;
	if (cond >= 0xE) {
		if (native) {
			_armTranslate(e, op, flags);
		} else {
			_handler(e, op);
		}
		return;
	}
	e->pending += 1 + e->seq32;
	_charge(e);
	bool passIfT = _condition(e, cond);
	uint8_t* skip = e->cg.ptr;
	if (passIfT) {
		sh4_emit_bf(&e->cg, 0);
	} else {
		sh4_emit_bt(&e->cg, 0);
	}
	e->pending = -(1 + e->seq32);
	if (native) {
		_armTranslate(e, op, flags);
	} else {
		_handler(e, op);
	}
	_charge(e);
	if (!sh4_disp8_fits(sh4_branch_disp8((uintptr_t) skip, (uintptr_t) e->cg.ptr))) {
		e->cg.overflow = 1;
	}
	_patchBranch(skip, e->cg.ptr);
}

static void _armB(struct JITEmitter* e, uint32_t op, int executed) {
	int32_t offset = (int32_t) (op << 8) >> 6;
	uint32_t target = _pcValue(e) + offset;
	bool link = op & 0x01000000;
	int cond = op >> 28;
	e->pending += 1 + e->seq32;
	if (cond != 0xE) {
		_charge(e);
		bool takenIfT = _condition(e, cond);
		uint8_t* skip = e->cg.ptr;
		if (takenIfT) {
			sh4_emit_bt(&e->cg, 0);
		} else {
			sh4_emit_bf(&e->cg, 0);
		}
		_fallThrough(e, e->address + WORD_SIZE_ARM, executed);
		_patchBranch(skip, e->cg.ptr);
	}
	if (link) {
		_imm(e, e->address + WORD_SIZE_ARM, 1);
		_st(e, 1, ARM_LR);
	}
	_branch(e, target, executed);
}

/* BX Rm staying in ARM within the region: look the target up here. */
static void _armBX(struct JITEmitter* e, uint32_t op, int executed) {
	_charge(e);
	_ld(e, op & 0xF, 4);
	_imm(e, 0xFF000001, 1);
	sh4_emit_mov_reg(&e->cg, 4, 2);
	sh4_emit_and(&e->cg, 1, 2);
	_imm(e, e->region << BASE_OFFSET, 1);
	sh4_emit_cmpeq(&e->cg, 1, 2);
	uint8_t* slow = e->cg.ptr;
	sh4_emit_bf(&e->cg, 0);
	e->pending += 1 + e->seq32 + _writePCCycles(e);
	_charge(e);
	_branchForgetsPrefetch(e);
	_count(e, executed);
	_jumpTo(e, e->jit->lookup);
	sh4_emit_mov_imm(&e->cg, JIT_EXIT_LOOKUP, 5);
	_patchBranch(slow, e->cg.ptr);
	_handler(e, op);
	_count(e, executed);
	_jumpTo(e, e->jit->dispatchSync);
	sh4_emit_nop(&e->cg);
}

struct JITBlock* ARMJITCompile(struct ARMJIT* jit, struct JITBlock* block, uint32_t pc, bool thumb,
                               const uint8_t* src, uint32_t srcBytes) {
	uint32_t len = thumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM;
	uint32_t avail = srcBytes / len;
	/* Each instruction also needs the two prefetch words after it. */
	if (avail < 3) {
		return NULL;
	}
	avail -= 2;
	if (avail > JIT_MAX_BLOCK_INSNS) {
		avail = JIT_MAX_BLOCK_INSNS;
	}
#ifndef __sh__
	/* Debugging aid: shorter blocks narrow a divergence down. */
	static int maxInsns = -1;
	if (maxInsns < 0) {
		const char* env = getenv("JIT_MAX_INSNS");
		maxInsns = env ? atoi(env) : 0;
	}
	if (maxInsns > 0 && avail > (uint32_t) maxInsns) {
		avail = maxInsns;
	}
#endif

	struct GBA* gba = (struct GBA*) jit->cpu->master;
	bool stall = (pc >> BASE_OFFSET) >= REGION_CART0 && gba->memory.prefetch;

	/* Pass 1: extent, and which instructions' flags anything reads. */
	uint32_t ops[JIT_MAX_BLOCK_INSNS];
	bool native[JIT_MAX_BLOCK_INSNS];
	unsigned written[JIT_MAX_BLOCK_INSNS];
	unsigned read[JIT_MAX_BLOCK_INSNS];
	bool flagsLive[JIT_MAX_BLOCK_INSNS];
	uint32_t n;
	for (n = 0; n < avail; ++n) {
		ops[n] = _read(&src[n * len], thumb);
		if (thumb) {
			native[n] = _thumbAnalyze(ops[n], &written[n], &read[n]);
			if (_thumbIsStore(ops[n])) {
				/* It may end the block, and then its flags are what the
				 * next block sees. */
				read[n] = F_ALL;
			}
		} else {
			native[n] = _armAnalyze(ops[n], stall, &written[n], &read[n]);
			if (native[n] && (ops[n] & 0x0C100000) == 0x04000000) {
				read[n] = F_ALL; // a store may end the block
			} else if (native[n] && (ops[n] & 0x0E100090) == 0x00000090 && (ops[n] & 0x60)) {
				read[n] = F_ALL;
			}
		}
		if (thumb ? _thumbEndsBlock(ops[n]) : _armEndsBlock(ops[n])) {
			++n;
			break;
		}
	}
	bool usesBase = false;
	int i;
	for (i = 0; i < (int) n; ++i) {
		uint32_t op = ops[i];
		if (native[i] && (thumb ? (op >> 11) >= 0x09 && (op >> 11) <= 0x19 : _armIsMemory(op))) {
			usesBase = true;
		}
		/* handler calls, except for the branches translated whole */
		if (!native[i]) {
			if (thumb) {
				if ((op & 0xF800) == 0xE000 || ((op & 0xF000) == 0xD000 && ((op >> 8) & 0xF) < 0xE)) {
					continue;
				}
				if ((op & 0xF800) == 0xF000 && i + 1 < (int) n && (ops[i + 1] & 0xF800) == 0xF800) {
					break;
				}
			} else if ((op & 0x0E000000) == 0x0A000000 && (op >> 28) != 0xF) {
				continue;
			}
			usesBase = true;
		}
	}
	unsigned needed = F_ALL;
	for (i = n - 1; i >= 0; --i) {
		flagsLive[i] = (written[i] & needed) != 0;
		needed = (needed & ~written[i]) | read[i];
	}

	struct JITEmitter e;
	memset(&e, 0, sizeof(e));
	e.jit = jit;
	e.thumb = thumb;
	e.region = pc >> BASE_OFFSET;
	e.cg.ptr = jit->code + ((jit->codeUsed + 3) & ~3);
	e.cg.end = jit->code + jit->codeSize;
	e.seq16 = gba->memory.waitstatesSeq16[e.region];
	e.nonseq16 = gba->memory.waitstatesNonseq16[e.region];
	e.seq32 = gba->memory.waitstatesSeq32[e.region];
	e.nonseq32 = gba->memory.waitstatesNonseq32[e.region];
	e.stall = stall;

	/* Prologue: see JIT_PRE_BYTES. */
	uint8_t* start = e.cg.ptr;
	sh4_emit_mov_l_load_pc(&e.cg, 1, 0);
	sh4_emit_mov_l_load_pc(&e.cg, 2, 1);
	sh4_emit_jmp(&e.cg, 1);
	sh4_emit_nop(&e.cg);
	uint32_t key = JIT_KEY(pc, thumb);
	uint32_t event = (uint32_t) (uintptr_t) jit->exits[JIT_EXIT_EVENT];
	sh4_word(&e.cg, key & 0xFFFF);
	sh4_word(&e.cg, key >> 16);
	sh4_word(&e.cg, event & 0xFFFF);
	sh4_word(&e.cg, event >> 16);
	uint8_t* entry = e.cg.ptr;
	sh4_emit_cmppz(&e.cg, R_CYCLES);
	sh4_emit_bt(&e.cg, sh4_branch_disp8((uintptr_t) e.cg.ptr, (uintptr_t) start));
	if (usesBase) {
		_imm(&e, pc + 2 * len, R_BASE);
	}

	bool ended = false;
	for (i = 0; i < (int) n; ++i) {
		uint32_t op = ops[i];
		e.index = i;
		e.address = pc + i * len;
		_poolCheck(&e);
		if (thumb) {
			if ((op & 0xF800) == 0xF000 && i + 1 < (int) n && (ops[i + 1] & 0xF800) == 0xF800) {
				_thumbBL(&e, op, ops[i + 1], n);
				ended = true;
				break;
			}
			if ((op & 0xF800) == 0xE000) {
				_thumbB(&e, op, n);
				ended = true;
				break;
			}
			if ((op & 0xF000) == 0xD000 && ((op >> 8) & 0xF) < 0xE) {
				_thumbBcc(&e, op, n);
				ended = true;
				break;
			}
			if ((op & 0xFF87) == 0x4700 && ((op >> 3) & 0xF) != ARM_PC) {
				_thumbBX(&e, op, n);
				ended = true;
				break;
			}
		}
		if (!thumb) {
			if ((op & 0x0E000000) == 0x0A000000 && (op >> 28) != 0xF) {
				_armB(&e, op, n);
				ended = true;
				break;
			}
			if ((op & 0x0FFFFFF0) == 0x012FFF10 && (op >> 28) == 0xE && (op & 0xF) != ARM_PC) {
				_armBX(&e, op, n);
				ended = true;
				break;
			}
		}
		if (!thumb) {
			_armConditional(&e, op, native[i], flagsLive[i]);
		} else if (native[i]) {
			if (!_thumbMultiple(&e, op, i + 1)) {
				_thumbTranslate(&e, op, flagsLive[i]);
			}
		} else {
			_handler(&e, op);
		}
	}
	if (!ended) {
		_fallThrough(&e, pc + n * len, n);
	}
	_placeSites(&e);
	_flushPool(&e, false);
	if (e.cg.overflow) {
		return NULL;
	}
	if (e.usesBase && !usesBase) {
		abort();
	}

	block->pc = pc;
	block->end = pc + n * len;
	block->nInsns = n;
	block->thumb = thumb;
	block->code = entry;
	block->codeSize = e.cg.ptr - start;
#ifndef __sh__
	/* Debugging aid: JIT_DUMP=<hex guest PC> prints that block's code. */
	const char* dump = getenv("JIT_DUMP");
	if (dump && strtoul(dump, NULL, 16) == pc) {
		const uint8_t* p;
		fprintf(stderr, "block %08X, %u insns:", pc, n);
		for (p = start; p < e.cg.ptr; p += 2) {
			fprintf(stderr, "%s%04X", (p - start) % 32 ? " " : "\n  ", p[0] | (p[1] << 8));
		}
		fputc('\n', stderr);
	}
#endif
	/* codeUsed was rounded up to start */
	jit->codeUsed = start - jit->code;
	return block;
}
