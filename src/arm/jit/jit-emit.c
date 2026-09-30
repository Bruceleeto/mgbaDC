/* ARM/Thumb -> SH-4 translation.
 *
 * Register use in generated code:
 *   GBR  struct ARMCore*: gprs, cycles, cpsr, prefetch and the recompiler's
 *        slots via @(disp,GBR) through r0; stc gbr gives helpers their first
 *        argument
 *   r13  cycles - nextEvent: blocks add their cost, and a block's entry
 *        leaves for the event handler once it is >= 0. cpu->cycles is only
 *        written when C is about to look at it.
 *   r0   GBR/immediate-logic operand; every guest register access goes
 *        through it
 *   r1-r5 scratch, clobbered by every call
 *   r8, r11, r12, r14, r6, r7  guest r0, r1, r2, sp, r4, r5 (_pinGuest)
 *   r9   N and Z: those of r9 taken as a result
 *   r10  C in bit 31, V in bit 30
 *
 * Guest registers live in the ARMCore between instructions, so anything
 * mGBA's code looks at is always current, except:
 *   - the pinned ones: in their host registers while generated code runs,
 *     written to the ARMCore by the exits and around handler calls (which
 *     reload them after). The memory slow paths leave them where they are
 *     (saving r6/r7 across the call): mGBA's memory code only looks at
 *     gprs[PC].
 *   - the flags: r9/r10, written to the cpsr on the way out to C (exits,
 *     handler calls) and read back when coming in. The memory slow paths
 *     don't look at them.
 *   - cycles: see r13. The static cost of the instructions since the last
 *     charge is held back in the emitter and added before anything that can
 *     observe it (a call, leaving the block).
 *   - flags an instruction sets that no later instruction in the block reads
 *     before they are overwritten are not computed (liveness pass).
 *   - cpu->jitBase: gprs[PC] as the block's first instruction sees it, set
 *     on entry by blocks that call out; the stubs add the access's offset
 *     (index * length, r3) to get its own.
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

#define R_CYCLES 13
/* Only the tooling reads how many guest instructions a run executed. */
#ifndef __sh__
#define JIT_COUNT
#endif
#define R_NZ 9

/* Guest registers held in host registers wherever generated code runs, as
 * example's pin_list: the ARMCore has them only while C does. The hottest in
 * Mario Kart's race by accesses: r0 11.4M, r1 5.9M, r2 3.0M, sp 2.7M, r4
 * 2.6M, r5 2.3M in 600 frames (r3 is next at 2.3M). r6/r7 are the C ABI's to
 * clobber: the stubs that call C keep them. */
#define JIT_PINS 6
static const int _pinGuest[JIT_PINS] = { 0, 1, 2, ARM_SP, 4, 5 };
static const int _pinHost[JIT_PINS] = { 8, 11, 12, 14, 6, 7 };

/* The host register guest is pinned to, or 0 */
static int _pinned(int guest) {
	int i;
	for (i = 0; i < JIT_PINS; ++i) {
		if (_pinGuest[i] == guest) {
			return _pinHost[i];
		}
	}
	return 0;
}

/* Pinned registers to the ARMCore and back; cpu = a register holding it. */
static void _pinsOut(sh4_codegen* cg, int cpu) {
	int i;
	for (i = 0; i < JIT_PINS; ++i) {
		sh4_emit_mov_l_store_disp(cg, _pinHost[i], cpu, JIT_GBR_GPRS(_pinGuest[i]));
	}
}

static void _pinsIn(sh4_codegen* cg, int cpu) {
	int i;
	for (i = 0; i < JIT_PINS; ++i) {
		sh4_emit_mov_l_load_disp(cg, cpu, _pinHost[i], JIT_GBR_GPRS(_pinGuest[i]));
	}
}
#define R_CV 10

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
	/* Instructions in the block */
	int executed;
	/* Guest region the block is in (address >> 24). */
	uint32_t region;

	/* Memory accesses use the stall stubs (code in ROM, prefetch on). */
	bool stall;
	/* Something reads cpu->jitBase (the block sets it on entry). */
	bool usesBase;

	/* The condition (0-13) of the next instruction, the only thing to
	 * read the flags this one sets, or -1: a compare can then leave just T
	 * for it (fused, fusedIfT as _condition returns) and not the flags. */
	int fuseCond;
	bool fused;
	bool fusedIfT;

	/* Where the block's instructions start: _reload looks no further back. */
	const uint8_t* body;

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

/* The latest branch target made (for the block being compiled). */
static const uint8_t* _label;

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
		_label = e->cg.ptr;
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

/* cpu->jitCount += executed, through r0 */
static void _count(struct JITEmitter* e, int executed) {
#ifdef JIT_COUNT
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_COUNT);
	sh4_emit_add_imm(&e->cg, executed, 0);
	sh4_emit_mov_l_store_gbr(&e->cg, JIT_GBR_COUNT);
#else
	UNUSED(e);
	UNUSED(executed);
#endif
}

/* Jump to one of the fixed routines (ARMJITEmitStubs). */
static void _jumpTo(struct JITEmitter* e, const void* target) {
	_lit(e, (uint32_t) (uintptr_t) target, 1);
	sh4_emit_jmp(&e->cg, 1);
}

/* Leave through a link site, charging what is pending: a jump through a
 * pool entry that starts out pointing at a stub (placed at the end of the
 * block) that exits to C, and that C points at the target block once it is
 * compiled. The pool has { entry, key, site }; the site is
 *   [add #pending,r13]  mov.l @(entry),r1  jmp @r1  nop
 * and C makes a target within reach of a bra "bra target; add" (or nop). */
static void _site(struct JITEmitter* e, uint32_t key, int type) {
	if (e->pending < -128 || e->pending > 127) {
		_charge(e);
	}
	int lit = _litUnique(e, 0);
	_litUnique(e, key);
	_litUnique(e, (uint32_t) (uintptr_t) e->cg.ptr);
	e->sites[e->nSites].lit = lit;
	e->sites[e->nSites].type = type;
	++e->nSites;
	_charge(e);
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

/* Guest registers not pinned go through r0 (@(disp,GBR)): then both destroy
 * it. */
/* Unpinned guest is in r0 already: the last thing emitted stored it from
 * there, and nothing branches in between. */
static bool _reload(struct JITEmitter* e, int guest) {
	const uint8_t* p = e->cg.ptr;
	if (!e->body || p - 4 < e->body || _label >= p) {
		return false;
	}
	uint16_t last = p[-2] | p[-1] << 8;
	uint16_t before = p[-4] | p[-3] << 8;
	if (last != (0xC200 | JIT_GBR_GPRS(guest))) {
		return false;
	}
	/* not a delay slot */
	switch (before >> 12) {
	case 0xA: // bra
	case 0xB: // bsr
		return false;
	case 0x8:
		return (before & 0x0D00) != 0x0D00; // bt/s, bf/s
	case 0x4:
		return (before & 0xF0DF) != 0x400B; // jsr, jmp
	case 0x0:
		return (before & 0xF0DF) != 0x000B && (before & 0xF0DF) != 0x0003; // rts, rte; braf, bsrf
	default:
		return true;
	}
}

static void _ld(struct JITEmitter* e, int guest, int host) {
	if (guest == ARM_PC) {
		_imm(e, _pcValue(e), host);
		return;
	}
	int pin = _pinned(guest);
	if (pin) {
		if (pin != host) {
			sh4_emit_mov_reg(&e->cg, pin, host);
		}
		return;
	}
	if (!_reload(e, guest)) {
		sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_GPRS(guest));
	}
	if (host) {
		sh4_emit_mov_reg(&e->cg, 0, host);
	}
}

static void _st(struct JITEmitter* e, int host, int guest) {
	int pin = _pinned(guest);
	if (pin) {
		if (pin != host) {
			sh4_emit_mov_reg(&e->cg, host, pin);
		}
		return;
	}
	if (host) {
		sh4_emit_mov_reg(&e->cg, host, 0);
	}
	sh4_emit_mov_l_store_gbr(&e->cg, JIT_GBR_GPRS(guest));
}

/* The host register with guest's value: its pin, or scratch loaded. */
static int _get(struct JITEmitter* e, int guest, int scratch) {
	int pin = _pinned(guest);
	if (pin) {
		return pin;
	}
	_ld(e, guest, scratch);
	return scratch;
}

/* The same, but left in r0 (returns 0) if it isn't pinned: for the last
 * operand loaded, with nothing using r0 before it is read. */
static int _getR0(struct JITEmitter* e, int guest) {
	int pin = _pinned(guest);
	if (pin || guest == ARM_PC) {
		return _get(e, guest, 1);
	}
	if (!_reload(e, guest)) {
		sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_GPRS(guest));
	}
	return 0;
}

/* Where a result for guest rd (-1: none) is made: its pin, or r1. */
static int _dst(int rd) {
	int pin = rd >= 0 ? _pinned(rd) : 0;
	return pin ? pin : 1;
}

/* Where rd = *a OP *b is computed, with *a moved there: rd's pin, or r0
 * for an unpinned rd, unless that is *b alone (then r1, or for a
 * commutative OP the operands swap).
 * b is never r1 (or -1 for an immediate). */
static int _into(struct JITEmitter* e, int rd, int* a, int* b, bool commutative) {
	int w = _dst(rd);
	if (w == 1 && rd >= 0 && rd != ARM_PC) {
		/* rd isn't pinned: make it in r0, which _put stores from */
		w = 0;
	}
	if (w != 1 && w == *b && w != *a) {
		if (commutative) {
			*b = *a;
			*a = w;
		} else {
			w = 1;
		}
	}
	if (*a != w) {
		sh4_emit_mov_reg(&e->cg, *a, w);
	}
	return w;
}

/* The result in host to guest rd, unless it was made in rd's pin. */
static void _put(struct JITEmitter* e, int host, int rd) {
	int pin = _pinned(rd);
	if (!pin || host != pin) {
		_st(e, host, rd);
	}
}

/* ADC/SBC/RSC and friends want their operands in r1/r2. */
static void _toScratch(struct JITEmitter* e, int a, int b) {
	if (a != 1) {
		sh4_emit_mov_reg(&e->cg, a, 1);
	}
	if (b != 2) {
		sh4_emit_mov_reg(&e->cg, b, 2);
	}
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
	if (r != R_NZ) {
		sh4_emit_mov_reg(&e->cg, r, R_NZ);
	}
}

/* T = C */
static void _carryToT(struct JITEmitter* e) {
	sh4_emit_rotl(&e->cg, R_CV);
	sh4_emit_rotr(&e->cg, R_CV);
}

/* C from bit 0 of rc (which is destroyed). */
static void _flagsC(struct JITEmitter* e, int rc) {
	sh4_emit_rotl(&e->cg, R_CV);
	sh4_emit_shlr(&e->cg, rc);
	sh4_emit_rotcr(&e->cg, R_CV);
}

/* C = T, for an instruction that writes C and not V. With V needed after
 * (in flags) it is kept; otherwise it gets the old C, which is one
 * instruction instead of four. r3 is scratch. */
static void _flagsCFromT(struct JITEmitter* e, unsigned flags) {
	if (flags & F_V) {
		sh4_emit_movt(&e->cg, 3);
		_flagsC(e, 3);
		return;
	}
	sh4_emit_rotcr(&e->cg, R_CV);
}

/* CMP a,b for e->fuseCond straight into T, if an SH-4 compare does it. */
static bool _fuseCmp(struct JITEmitter* e, int a, int b) {
	int cond = e->fuseCond;
	switch (cond & ~1) {
	case 0x0: // EQ, NE
		sh4_emit_cmpeq(&e->cg, b, a);
		break;
	case 0x2: // CS, CC
		sh4_emit_cmphs(&e->cg, b, a);
		break;
	case 0x8: // HI, LS
		sh4_emit_cmphi(&e->cg, b, a);
		break;
	case 0xA: // GE, LT
		sh4_emit_cmpge(&e->cg, b, a);
		break;
	case 0xC: // GT, LE
		sh4_emit_cmpgt(&e->cg, b, a);
		break;
	default:
		return false;
	}
	e->fused = true;
	e->fusedIfT = !(cond & 1);
	e->fuseCond = -1;
	return true;
}

/* CMP a,#imm the same way; against 0 the one-register forms. */
static bool _fuseCmpImm(struct JITEmitter* e, int a, int32_t imm) {
	int cond = e->fuseCond;
	if (!imm) {
		switch (cond & ~1) {
		case 0x0: // EQ, NE
			sh4_emit_tst(&e->cg, a, a);
			break;
		case 0xA: // GE, LT
			sh4_emit_cmppz(&e->cg, a);
			break;
		case 0xC: // GT, LE
			sh4_emit_cmppl(&e->cg, a);
			break;
		default:
			goto general;
		}
		e->fused = true;
		e->fusedIfT = !(cond & 1);
		return true;
	}
	if (a == 0 && imm >= -128 && imm <= 127 && (cond & ~1) == 0x0) {
		sh4_emit_cmpeq_imm(&e->cg, imm);
		e->fused = true;
		e->fusedIfT = !(cond & 1);
		e->fuseCond = -1;
		return true;
	}
general:
	switch (cond & ~1) {
	case 0x0:
	case 0x2:
	case 0x8:
	case 0xA:
	case 0xC:
		break;
	default:
		return false;
	}
	int b = a == 3 ? 2 : 3;
	_imm(e, imm, b);
	return _fuseCmp(e, a, b);
}

/* TST a,b: only Z is the same as an SH-4 tst's. */
static bool _fuseTst(struct JITEmitter* e, int a, int b) {
	int cond = e->fuseCond;
	if ((cond & ~1) != 0x0) {
		return false;
	}
	sh4_emit_tst(&e->cg, b, a);
	e->fused = true;
	e->fusedIfT = !(cond & 1);
	e->fuseCond = -1;
	return true;
}

/* For e->fuseCond EQ/NE/MI/PL, from the result in w; returns if it did. */
static bool _fuseResult(struct JITEmitter* e, int w) {
	int cond = e->fuseCond;
	switch (cond) {
	case 0x0: // EQ
	case 0x1: // NE
		sh4_emit_tst(&e->cg, w, w);
		e->fusedIfT = cond == 0x0;
		break;
	case 0x4: // MI
	case 0x5: // PL
		sh4_emit_cmppz(&e->cg, w);
		e->fusedIfT = cond == 0x5;
		break;
	default:
		return false;
	}
	e->fused = true;
	e->fuseCond = -1;
	return true;
}

static bool _fusesByResult(int cond) {
	return cond >= 0 && (cond <= 0x1 || cond == 0x4 || cond == 0x5);
}

/* a + b or a - b (host registers, as _into), into guest register rd (-1
 * for CMP/CMN). flags: those needed after (F_*), the only ones made. */
static void _addSub(struct JITEmitter* e, bool sub, int rd, unsigned flags, int a, int b) {
	bool byResult = false;
	bool carryOut = false;
	if (e->fuseCond >= 0) {
		if (rd < 0) {
			if (sub && _fuseCmp(e, a, b)) {
				return;
			}
		} else if (_fusesByResult(e->fuseCond)) {
			byResult = true;
			flags = 0;
		} else if (sub && _fuseCmp(e, a, b)) {
			/* T first, from the operands: the result doesn't touch it */
			flags = 0;
		} else if (!sub && (e->fuseCond & ~1) == 0x2 && a != b) { // CS, CC
			/* the carry out after: the sum is below an operand */
			carryOut = true;
			flags = 0;
		}
	}
	if (!(flags & F_ALL)) {
		if (rd < 0) {
			return;
		}
		int oa = a;
		int ob = b;
		int w = _into(e, rd, &a, &b, !sub);
		if (sub) {
			sh4_emit_sub(&e->cg, b, w);
		} else {
			sh4_emit_add_reg(&e->cg, b, w);
		}
		if (carryOut) {
			sh4_emit_cmphi(&e->cg, w, oa != w ? oa : ob);
			e->fused = true;
			e->fusedIfT = e->fuseCond == 0x2;
			e->fuseCond = -1;
		}
		_put(e, w, rd);
		if (byResult) {
			_fuseResult(e, w);
		}
		return;
	}
	if (!(flags & F_V)) {
		/* C (if needed) goes in with the old C as V */
		if (rd < 0 && !(flags & F_NZ) && sub) {
			sh4_emit_cmphs(&e->cg, b, a);
			sh4_emit_rotcr(&e->cg, R_CV);
			return;
		}
		sh4_emit_mov_reg(&e->cg, a, R_NZ);
		if (sub) {
			sh4_emit_sub(&e->cg, b, R_NZ);
		} else {
			sh4_emit_add_reg(&e->cg, b, R_NZ);
		}
		if (flags & F_C) {
			if (sub) {
				sh4_emit_cmphs(&e->cg, b, a);
			} else {
				sh4_emit_cmphi(&e->cg, R_NZ, a);
			}
			sh4_emit_rotcr(&e->cg, R_CV);
		}
		if (rd >= 0) {
			_st(e, R_NZ, rd);
		}
		return;
	}
	sh4_emit_mov_reg(&e->cg, a, R_NZ);
	if (sub) {
		sh4_emit_subv(&e->cg, b, R_NZ);
		sh4_emit_rotcr(&e->cg, R_CV);
		sh4_emit_cmphs(&e->cg, b, a);
	} else {
		sh4_emit_addv(&e->cg, b, R_NZ);
		sh4_emit_rotcr(&e->cg, R_CV);
		/* carry iff the sum wrapped below an operand */
		sh4_emit_cmphi(&e->cg, R_NZ, a);
	}
	sh4_emit_rotcr(&e->cg, R_CV);
	if (rd >= 0) {
		_st(e, R_NZ, rd);
	}
}

/* a + imm or a - imm into rd, as _addSub */
static void _addSubImm(struct JITEmitter* e, bool sub, int rd, unsigned flags, int a, int32_t imm) {
	bool byResult = false;
	if (e->fuseCond >= 0) {
		if (rd < 0) {
			if (sub && _fuseCmpImm(e, a, imm)) {
				return;
			}
		} else if (_fusesByResult(e->fuseCond)) {
			byResult = true;
			flags = 0;
		} else if (sub && _fuseCmpImm(e, a, imm)) {
			flags = 0;
		}
	}
	int32_t value = sub ? -imm : imm;
	if (!(flags & (F_C | F_V)) && value >= -128 && value <= 127) {
		int w;
		if (rd < 0) {
			if (!(flags & F_NZ)) {
				return;
			}
			w = R_NZ;
			sh4_emit_mov_reg(&e->cg, a, w);
		} else {
			int b = -1;
			w = _into(e, rd, &a, &b, false);
		}
		if (value) {
			sh4_emit_add_imm(&e->cg, value, w);
		}
		if (rd >= 0) {
			_put(e, w, rd);
			if (flags & F_NZ) {
				sh4_emit_mov_reg(&e->cg, w, R_NZ);
			}
		}
		if (byResult) {
			_fuseResult(e, w);
		}
		return;
	}
	_imm(e, imm, 2);
	_addSub(e, sub, rd, flags, a, 2);
}

/* ADC/SBC: r1 + r2 + C, with r2 inverted first for SBC. */
static void _addCarry(struct JITEmitter* e, bool sub, int rd, unsigned flags) {
	if (sub) {
		sh4_emit_not(&e->cg, 2, 2);
	}
	_carryToT(e);
	sh4_emit_mov_reg(&e->cg, 1, 3);
	sh4_emit_addc(&e->cg, 2, 3);
	_st(e, 3, rd);
	if (!(flags & F_V)) {
		if (flags & F_C) {
			sh4_emit_rotcr(&e->cg, R_CV);
		}
		if (flags & F_NZ) {
			sh4_emit_mov_reg(&e->cg, 3, R_NZ);
		}
		return;
	}
	sh4_emit_movt(&e->cg, 5);
	/* V = ~(a ^ b) & (a ^ result), sign bit */
	sh4_emit_mov_reg(&e->cg, 1, 0);
	sh4_emit_xor(&e->cg, 2, 0);
	sh4_emit_not(&e->cg, 0, 0);
	sh4_emit_mov_reg(&e->cg, 1, 4);
	sh4_emit_xor(&e->cg, 3, 4);
	sh4_emit_and(&e->cg, 4, 0);
	sh4_emit_shll(&e->cg, 0);
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

static uint32_t _sysMultiple(struct ARMCore* cpu, uint32_t op, uint32_t pc);

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
	/* An event that is due goes first: what the store set up (a DMA) can't
	 * wait for the end of the block, the next store may be its registers. */
	if (cpu->nextEvent >= state->nextEvent && !retimed && cpu->cycles < cpu->nextEvent) {
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
	if (e->jit->fastmem) {
		/* The access itself, at the guest's address (fastmem.c). The mask
		 * keeps it out of the host's half of the address space; what isn't
		 * mapped faults into the stub, which returns after the access.
		 * Not in a delay slot, and only these six instructions. */
		sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_MASK);
		sh4_emit_and(&e->cg, 0, 4);
		sh4_emit_mov_imm(&e->cg, e->index * _insnLength(e), 3);
		switch (op) {
		case JIT_MEM_LOAD32:
			sh4_emit_mov_l_load(&e->cg, 4, 0);
			break;
		case JIT_MEM_LOAD16:
			sh4_emit_mov_w_load(&e->cg, 4, 0);
			sh4_emit_extu_w(&e->cg, 0, 0);
			break;
		case JIT_MEM_LOADS16:
			sh4_emit_mov_w_load(&e->cg, 4, 0);
			break;
		case JIT_MEM_LOAD8:
			sh4_emit_mov_b_load(&e->cg, 4, 0);
			sh4_emit_extu_b(&e->cg, 0, 0);
			break;
		case JIT_MEM_LOADS8:
			sh4_emit_mov_b_load(&e->cg, 4, 0);
			break;
		case JIT_MEM_STORE32:
			sh4_emit_mov_l_store(&e->cg, 5, 4);
			break;
		case JIT_MEM_STORE16:
			sh4_emit_mov_w_store(&e->cg, 5, 4);
			break;
		case JIT_MEM_STORE8:
			sh4_emit_mov_b_store(&e->cg, 5, 4);
			break;
		default:
			break;
		}
#ifdef __sh__
		/* The wait of a region without any */
		e->pending += store ? 1 : 2;
#endif
	} else {
		sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_STUBS + e->stall * JIT_MEM_OPS + op);
		sh4_emit_jsr(&e->cg, 0);
		sh4_emit_mov_imm(&e->cg, e->index * _insnLength(e), 3);
	}
	if (!store) {
		_st(e, 0, rd);
	}
	e->usesBase |= e->stall;
	/* The prefetch's 1S becomes 1N: THUMB/ARM_PREFETCH_CYCLES +
	 * *_LOAD/STORE_POST_BODY. */
	e->pending += 1 + (e->thumb ? e->nonseq16 : e->nonseq32);
}

/* ---------------------------------------------------------------- */
/* Fixed routines                                                    */
/* ---------------------------------------------------------------- */

static void _patchBranch(uint8_t* at, uint8_t* to) {
	if (to > _label) {
		_label = to;
	}
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

/* Around a C call from a stub: the pins C may clobber. */
static void _pinsSave(struct JITEmitter* e) {
	sh4_emit_mov_l_store_dec(&e->cg, 6, 15);
	sh4_emit_mov_l_store_dec(&e->cg, 7, 15);
}

static void _pinsRestore(struct JITEmitter* e) {
	sh4_emit_mov_l_load_inc(&e->cg, 15, 7);
	sh4_emit_mov_l_load_inc(&e->cg, 15, 6);
}

/* r0 = the guest base of the block whose code called at r1 (a return
 * address), from jit->baseCache or ARMJITGuestBase; r1 and r7 go. Blocks
 * don't keep their base in cpu->jitBase unless the stall model needs it. */
static void _emitGuestBase(struct JITEmitter* e) {
	e->jit->guestBase = e->cg.ptr;
	sh4_emit_mov_reg(&e->cg, 1, 0);
	sh4_emit_shlr(&e->cg, 0);
	sh4_emit_and_imm(&e->cg, 0xFF);
	sh4_emit_shll2(&e->cg, 0);
	sh4_emit_shll(&e->cg, 0);
	_lit(e, (uint32_t) (uintptr_t) e->jit->baseCache, 7);
	sh4_emit_add_reg(&e->cg, 0, 7);
	sh4_emit_mov_l_load_inc(&e->cg, 7, 0);
	sh4_emit_cmpeq(&e->cg, 1, 0);
	uint8_t* miss = e->cg.ptr;
	sh4_emit_bf(&e->cg, 0);
	sh4_emit_rts(&e->cg);
	sh4_emit_mov_l_load(&e->cg, 7, 0);
	_patchBranch(miss, e->cg.ptr);
	sh4_emit_sts_pr_dec(&e->cg, 15);
	int r;
	for (r = 2; r <= 6; ++r) {
		sh4_emit_mov_l_store_dec(&e->cg, r, 15);
	}
	_lit(e, (uint32_t) (uintptr_t) e->jit, 4);
	_lit(e, (uint32_t) (uintptr_t) ARMJITGuestBase, 0);
	sh4_emit_jsr(&e->cg, 0);
	sh4_emit_mov_reg(&e->cg, 1, 5);
	for (r = 6; r >= 2; --r) {
		sh4_emit_mov_l_load_inc(&e->cg, 15, r);
	}
	sh4_emit_lds_pr_inc(&e->cg, 15);
	sh4_emit_rts(&e->cg);
	sh4_emit_nop(&e->cg);
	_flushPool(e, false);
	ARMJITRegisterCall(e->jit, ARMJITGuestBase);
}

/* r0 = the guest base of the block the stub was called from (PR); bsr
 * slot as given. */
static void _guestBaseCall(struct JITEmitter* e) {
	sh4_emit_sts_pr(&e->cg, 1);
	sh4_emit_bsr(&e->cg, sh4_branch_disp12((uintptr_t) e->cg.ptr, (uintptr_t) e->jit->guestBase));
}

/* The slow paths: mGBA's own access through the wrappers above, with
 * cpu->cycles and PC as they expect. r2 = wrapper, r3 = offset, r4 = address,
 * r5 = value. Loads return the value in r0. A store that says stop leaves
 * for dispatchSync with this instruction counted. */
static void _emitSlowLoad(struct JITEmitter* e) {
	sh4_emit_sts_pr_dec(&e->cg, 15);
	_pinsSave(e);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_NEXT_EVENT);
	sh4_emit_add_reg(&e->cg, R_CYCLES, 0);
	sh4_emit_mov_l_store_gbr(&e->cg, JIT_GBR_CYCLES);
	sh4_emit_mov_reg(&e->cg, 4, 5);
	_guestBaseCall(e);
	sh4_emit_mov_reg(&e->cg, 3, 6);
	sh4_emit_add_reg(&e->cg, 0, 6);
	sh4_emit_jsr(&e->cg, 2);
	sh4_emit_stc_gbr(&e->cg, 4);
	sh4_emit_mov_reg(&e->cg, 0, 3);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_CYCLES);
	sh4_emit_mov_reg(&e->cg, 0, R_CYCLES);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_NEXT_EVENT);
	sh4_emit_sub(&e->cg, 0, R_CYCLES);
	_pinsRestore(e);
	sh4_emit_lds_pr_inc(&e->cg, 15);
	sh4_emit_rts(&e->cg);
	sh4_emit_mov_reg(&e->cg, 3, 0);
}

static void _emitSlowStore(struct JITEmitter* e) {
	sh4_emit_sts_pr_dec(&e->cg, 15);
	_pinsSave(e);
	sh4_emit_mov_l_store_dec(&e->cg, 3, 15);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_NEXT_EVENT);
	sh4_emit_add_reg(&e->cg, R_CYCLES, 0);
	sh4_emit_mov_l_store_gbr(&e->cg, JIT_GBR_CYCLES);
	_guestBaseCall(e);
	sh4_emit_nop(&e->cg);
	sh4_emit_mov_reg(&e->cg, 3, 7);
	sh4_emit_add_reg(&e->cg, 0, 7);
	sh4_emit_mov_reg(&e->cg, 5, 6);
	sh4_emit_mov_reg(&e->cg, 4, 5);
	sh4_emit_jsr(&e->cg, 2);
	sh4_emit_stc_gbr(&e->cg, 4);
	sh4_emit_mov_l_load_inc(&e->cg, 15, 3);
	sh4_emit_mov_reg(&e->cg, 0, 2);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_CYCLES);
	sh4_emit_mov_reg(&e->cg, 0, R_CYCLES);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_NEXT_EVENT);
	sh4_emit_sub(&e->cg, 0, R_CYCLES);
	_pinsRestore(e);
	sh4_emit_lds_pr_inc(&e->cg, 15);
	sh4_emit_tst(&e->cg, 2, 2);
	uint8_t* stop = e->cg.ptr;
	sh4_emit_bf(&e->cg, 0);
	sh4_emit_rts(&e->cg);
	sh4_emit_nop(&e->cg);
	_patchBranch(stop, e->cg.ptr);
#ifdef JIT_COUNT
	/* jitCount += offset / length + 1; a store doesn't change the mode */
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_CPSR);
	sh4_emit_shlr(&e->cg, 3);
	sh4_emit_tst_imm(&e->cg, 0x20);
	uint8_t* thumb = e->cg.ptr;
	sh4_emit_bf(&e->cg, 0);
	sh4_emit_shlr(&e->cg, 3);
	_patchBranch(thumb, e->cg.ptr);
	sh4_emit_add_imm(&e->cg, 1, 3);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_COUNT);
	sh4_emit_add_reg(&e->cg, 3, 0);
	sh4_emit_mov_l_store_gbr(&e->cg, JIT_GBR_COUNT);
#endif
	_jumpTo(e, e->jit->dispatchSync);
	sh4_emit_nop(&e->cg);
}

/* GBAMemoryStall for code in ROM with the prefetch buffer on: r2 = the
 * access's wait, r3 = offset, r0 kept (in cpu->jitTmp meanwhile). Adds the
 * result to r13 and returns T clear; r1-r5 go. The loop is mGBA's, counting
 * loads down in r5 from maxLoads - 1, which makes lastPrefetchedPc
 * pc + 2 * (7 - r5) whatever previousLoads was. */
static void _emitStall(struct JITEmitter* e) {
	struct GBA* gba = (struct GBA*) e->jit->cpu->master;
	uint32_t last = (uint32_t) (uintptr_t) &gba->memory.lastPrefetchedPc;
	sh4_emit_mov_l_store_gbr(&e->cg, JIT_GBR_TMP);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_BASE);
	sh4_emit_add_reg(&e->cg, 0, 3);
	_lit(e, last, 1);
	sh4_emit_mov_l_load(&e->cg, 1, 1);
	sh4_emit_sub(&e->cg, 3, 1);
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
	sh4_emit_add_reg(&e->cg, 3, 0);
	_lit(e, last, 3);
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
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_TMP);
	sh4_emit_rts(&e->cg);
	sh4_emit_clrt(&e->cg);
}

/* The access itself, r0 = the region's buffer, r1 = offset into it, value
 * in/out as the op says; r0 goes (or is the value). */
static void _emitAccessOp(struct JITEmitter* e, enum JITMemOp op) {
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

/* The access, r1 = offset into region md's buffer */
static void _emitAccess(struct JITEmitter* e, enum JITMemOp op, int md) {
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_MD(md));
	_emitAccessOp(e, op);
}

/* r1 = r4 & mask */
static void _emitOffset(struct JITEmitter* e, uint32_t mask) {
	_lit(e, mask, 1);
	sh4_emit_and(&e->cg, 4, 1);
}

/* r2 = the memData word md */
static void _emitMd(struct JITEmitter* e, int md) {
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_MD(md));
	sh4_emit_mov_reg(&e->cg, 0, 2);
}

/* A store into a chunk with compiled code in it goes the slow way, which
 * invalidates. Offset in reg, which goes, and r0 (md < 0: r0 is the chunk
 * table already). */
static void _emitSmcCheck(struct JITEmitter* e, int reg, int md, struct JITFixups* slow) {
	sh4_emit_shlr8(&e->cg, reg);
	sh4_emit_shll2(&e->cg, reg);
	if (md >= 0) {
		sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_MD(md));
	}
	sh4_emit_mov_l_load_r0(&e->cg, reg, reg);
	sh4_emit_tst(&e->cg, reg, reg);
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

/* _emitWaitTail for a wait known here */
static void _emitWaitTailConst(struct JITEmitter* e, bool stall, uint8_t* stallCode, int wait) {
	if (stall) {
		sh4_emit_mov_imm(&e->cg, wait, 2);
		_emitWaitTail(e, stall, stallCode);
	} else {
		sh4_emit_rts(&e->cg);
		sh4_emit_add_imm(&e->cg, wait, R_CYCLES);
	}
}

/* cpu->jitStubs[stall][op]: r3 = offset, r4 = address, r5 = value to store;
 * a load's value comes back in r0. The fast paths touch r0-r2 only (r3-r5
 * are the slow path's), the stall model r0-r5. */
static void _emitMemoryStub(struct JITEmitter* e, enum JITMemOp op, bool stall, uint8_t* stallCode,
                            uint8_t* slowLoad, uint8_t* slowStore) {
	static const void* const slow[JIT_MEM_OPS] = {
		_load32, _load16, _loadS16, _load8, _loadS8, _store32, _store16, _store8
	};
	bool store = op >= JIT_MEM_STORE32;
	int size = op == JIT_MEM_LOAD32 || op == JIT_MEM_STORE32 ? 4
	         : op == JIT_MEM_LOAD8 || op == JIT_MEM_LOADS8 || op == JIT_MEM_STORE8 ? 1 : 2;
	struct JITFixups toSlow = { .n = 0 };
	struct JITFixups notIwram = { .n = 0 };
	struct JITFixups toRom = { .n = 0 };

	e->jit->cpu->jitStubs[stall][op] = e->cg.ptr;
	sh4_emit_mov_reg(&e->cg, 4, 0);
	if (size > 1) {
		sh4_emit_tst_imm(&e->cg, size - 1);
		_bfTo(e, &toSlow);
	}
	sh4_emit_shlr16(&e->cg, 0);
	sh4_emit_shlr8(&e->cg, 0);
	sh4_emit_cmpeq_imm(&e->cg, REGION_WORKING_IRAM);
	_bfTo(e, &notIwram);

	/* IWRAM, falling through: no wait states of its own */
	if (store) {
		_emitOffset(e, SIZE_WORKING_IRAM - 1);
		sh4_emit_mov_reg(&e->cg, 1, 2);
		_emitSmcCheck(e, 2, JIT_MD_CHUNKS_IWRAM, &toSlow);
		_emitAccess(e, op, JIT_MD_IWRAM);
	} else {
		sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_MD(JIT_MD_IWRAM));
		_emitOffset(e, SIZE_WORKING_IRAM - 1);
		_emitAccessOp(e, op);
	}
	_emitWaitTailConst(e, stall, stallCode, store ? 1 : 2);

	_branchHere(&notIwram, e->cg.ptr);
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
		sh4_emit_mov_reg(&e->cg, 1, 2);
		_emitSmcCheck(e, 2, JIT_MD_CHUNKS_EWRAM, &toSlow);
		_emitAccess(e, op, JIT_MD_WRAM);
		_emitMd(e, size == 4 ? JIT_MD_EWRAM_STORE32 : JIT_MD_EWRAM_STORE16);
	} else {
		_emitMd(e, size == 4 ? JIT_MD_EWRAM_LOAD32 : JIT_MD_EWRAM_LOAD16);
		_emitAccess(e, op, JIT_MD_WRAM);
	}
	_emitWaitTail(e, stall, stallCode);

	/* ROM (wait state 0 only), never stalls: above BASE_CART0 */
	if (!store) {
		_branchHere(&toRom, e->cg.ptr);
		_emitOffset(e, SIZE_CART0 - 1);
		sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_MD(JIT_MD_ROM_SIZE));
		sh4_emit_cmphs(&e->cg, 0, 1);
		_btTo(e, &toSlow);
		_emitMd(e, size == 4 ? JIT_MD_ROM_LOAD32 : JIT_MD_ROM_LOAD16);
		_emitAccess(e, op, JIT_MD_ROM);
		_emitWaitTail(e, false, NULL);
	}

	_branchHere(&toSlow, e->cg.ptr);
	ARMJITRegisterCall(e->jit, slow[op]);
	_lit(e, (uint32_t) (uintptr_t) slow[op], 2);
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
	sh4_emit_mov_reg(&e->cg, 2, 4);
	sh4_emit_shll2(&e->cg, 4);
	sh4_emit_add_reg(&e->cg, 1, 4);
	_lit(e, size, 0);
	sh4_emit_cmphi(&e->cg, 0, 4);
	_btTo(e, slow);
	if (store) {
		/* 64 bytes at most: the chunks of the first and last words */
		sh4_emit_add_imm(&e->cg, -1, 4);
		_emitSmcCheck(e, 4, chunks, slow);
		sh4_emit_mov_reg(&e->cg, 1, 4);
		_emitSmcCheck(e, 4, -1, slow);
	}
	if (md == JIT_MD_WRAM) {
		sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_MD(JIT_MD_EWRAM_WORD));
		sh4_emit_mul_l(&e->cg, 0, 2);
		sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_MD(store ? JIT_MD_EWRAM_STM : JIT_MD_EWRAM_LDM));
		sh4_emit_sts_macl(&e->cg, 2);
		sh4_emit_add_reg(&e->cg, 0, 2);
	} else if (!store) {
		sh4_emit_add_imm(&e->cg, 1, 2);
	}
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_MD(md));
	sh4_emit_add_reg(&e->cg, 1, 0);
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

/* LDM/STM-like: r2 = the number of words, r3 = offset, r4 = the lowest
 * address, r5 = the instruction as _sysMultiple takes it. In IWRAM or EWRAM
 * without wrapping (and for a store, no code in the way): charges
 * GBALoad/StoreMultiple's wait and returns T clear with r0 = the host address
 * for the caller to copy through. Otherwise _sysMultiple does the whole
 * instruction and it returns T set (or stops, as jit->handlers). The fast
 * path leaves r3 and r5 for the slow one. */
static void _emitMultipleStub(struct JITEmitter* e, bool store, bool stall, bool thumb, uint8_t* stallCode) {
	struct JITFixups toSlow = { .n = 0 };
	struct JITFixups toIwram = { .n = 0 };
	e->jit->multipleStubs[stall][store][thumb] = e->cg.ptr;
	sh4_emit_mov_reg(&e->cg, 4, 0);
	sh4_emit_shlr16(&e->cg, 0);
	sh4_emit_shlr8(&e->cg, 0);
	sh4_emit_cmpeq_imm(&e->cg, REGION_WORKING_IRAM);
	_btTo(e, &toIwram);
	sh4_emit_cmpeq_imm(&e->cg, REGION_WORKING_RAM);
	_bfTo(e, &toSlow);
	_emitMultipleRegion(e, store, stall, stallCode, JIT_MD_WRAM, JIT_MD_CHUNKS_EWRAM, SIZE_WORKING_RAM, &toSlow);
	_branchHere(&toIwram, e->cg.ptr);
	_emitMultipleRegion(e, store, stall, stallCode, JIT_MD_IWRAM, JIT_MD_CHUNKS_IWRAM, SIZE_WORKING_IRAM, &toSlow);
	_branchHere(&toSlow, e->cg.ptr);
	_lit(e, (uint32_t) (uintptr_t) _sysMultiple, 2);
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
	e->jit->stall = stallCode;
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

static void _registerRoutines(struct ARMJIT* jit);

/* jit->handlers[thumb]: r2 = the C routine, r3 = offset as for the memory
 * stubs, r5 = argument. The flags and pins go to the cpu and back around it.
 * Returns if the block can carry on; otherwise counts the instruction and
 * leaves for dispatchSync, or for C (commonRaw) if the flags can't be held. */
static void _emitHandlerStub(struct JITEmitter* e, bool thumb, uint8_t* commonRaw) {
	e->jit->handlers[thumb] = e->cg.ptr;
	sh4_emit_sts_pr_dec(&e->cg, 15);
	sh4_emit_mov_l_store_dec(&e->cg, 3, 15);
	_emitFlagsOut(e, 1);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_NEXT_EVENT);
	sh4_emit_add_reg(&e->cg, R_CYCLES, 0);
	sh4_emit_mov_l_store_gbr(&e->cg, JIT_GBR_CYCLES);
	sh4_emit_stc_gbr(&e->cg, 4);
	_pinsOut(&e->cg, 4);
	_guestBaseCall(e);
	sh4_emit_mov_reg(&e->cg, 3, 6);
	sh4_emit_jsr(&e->cg, 2);
	sh4_emit_add_reg(&e->cg, 0, 6);
	sh4_emit_mov_reg(&e->cg, 0, 5);
	sh4_emit_stc_gbr(&e->cg, 1);
	_pinsIn(&e->cg, 1);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_CYCLES);
	sh4_emit_mov_reg(&e->cg, 0, R_CYCLES);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_NEXT_EVENT);
	sh4_emit_sub(&e->cg, 0, R_CYCLES);
	_emitFlagsIn(e);
	sh4_emit_mov_l_load_inc(&e->cg, 15, 3);
	sh4_emit_lds_pr_inc(&e->cg, 15);
	sh4_emit_tst(&e->cg, 5, 5);
	uint8_t* stop = e->cg.ptr;
	sh4_emit_bf(&e->cg, 0);
	sh4_emit_rts(&e->cg);
	sh4_emit_nop(&e->cg);
	_patchBranch(stop, e->cg.ptr);
#ifdef JIT_COUNT
	/* jitCount += offset / length + 1 */
	if (thumb) {
		sh4_emit_shlr(&e->cg, 3);
	} else {
		sh4_emit_shlr2(&e->cg, 3);
	}
	sh4_emit_add_imm(&e->cg, 1, 3);
	sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_COUNT);
	sh4_emit_add_reg(&e->cg, 3, 0);
	sh4_emit_mov_l_store_gbr(&e->cg, JIT_GBR_COUNT);
#endif
	sh4_emit_mov_reg(&e->cg, 5, 0);
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
	uint32_t* md = jit->cpu->jitMemData;
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
#ifdef JIT_FASTMEM
	ARMJITFastmemUpdate(jit);
#endif
}

/* Once, at the start of the code buffer:
 *
 * enter(cpu, entry): saves what the C ABI says a callee keeps (r8-r14, PR)
 *   plus GBR, sets up GBR, the pinned registers and r13 and jumps to a block
 *   entry. Returns the number of guest instructions run (0 without
 *   JIT_COUNT), with jit->exit saying why it stopped.
 * exits[type]: r0 = the exit's argument. Puts cpu->cycles back and returns
 *   from enter.
 * lookup: r4 = key, r5 = exit type if it isn't in the hash. Jumps to the
 *   block's entry.
 * dispatchSync: lookup for the instruction gprs[PC] and the cpsr say is
 *   next, after mGBA code has moved the cpu. */
void ARMJITEmitStubs(struct ARMJIT* jit) {
	struct JITEmitter e;
	memset(&e, 0, sizeof(e));
	e.fuseCond = -1;
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
	_pinsIn(&e.cg, 4);
	sh4_emit_mov_l_load_gbr(&e.cg, JIT_GBR_CYCLES);
	sh4_emit_mov_reg(&e.cg, 0, R_CYCLES);
	sh4_emit_mov_l_load_gbr(&e.cg, JIT_GBR_NEXT_EVENT);
	sh4_emit_sub(&e.cg, 0, R_CYCLES);
#ifdef JIT_COUNT
	sh4_emit_mov_imm(&e.cg, 0, 0);
	sh4_emit_mov_l_store_gbr(&e.cg, JIT_GBR_COUNT);
#endif
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
	sh4_emit_stc_gbr(&e.cg, 2);
	_pinsOut(&e.cg, 2);
#ifdef JIT_COUNT
	sh4_emit_mov_l_load_gbr(&e.cg, JIT_GBR_COUNT);
#else
	sh4_emit_mov_imm(&e.cg, 0, 0);
#endif
	sh4_emit_mov_l_load_inc(&e.cg, 15, 1);
	sh4_emit_ldc_gbr(&e.cg, 1);
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
	/* r1 = the set: JIT_HASH_SET(key) * 16 */
	sh4_emit_mov_reg(&e.cg, 4, 1);
	sh4_emit_shll2(&e.cg, 1);
	_imm(&e, ((1 << JIT_HASH_BITS) - 1) << 4, 2);
	sh4_emit_shll(&e.cg, 1);
	sh4_emit_and(&e.cg, 2, 1);
	_lit(&e, (uint32_t) (uintptr_t) jit->hash, 2);
	sh4_emit_add_reg(&e.cg, 2, 1);
	sh4_emit_mov_l_load_inc(&e.cg, 1, 2);
	sh4_emit_cmpeq(&e.cg, 4, 2);
	uint8_t* way1 = e.cg.ptr;
	sh4_emit_bf(&e.cg, 0);
	sh4_emit_mov_l_load(&e.cg, 1, 1);
	sh4_emit_jmp(&e.cg, 1);
	sh4_emit_nop(&e.cg);
	_patchBranch(way1, e.cg.ptr);
	sh4_emit_mov_l_load_disp(&e.cg, 1, 2, 1);
	sh4_emit_cmpeq(&e.cg, 4, 2);
	uint8_t* miss = e.cg.ptr;
	sh4_emit_bf(&e.cg, 0);
	sh4_emit_mov_l_load_disp(&e.cg, 1, 1, 2);
	sh4_emit_jmp(&e.cg, 1);
	sh4_emit_nop(&e.cg);
	_patchBranch(miss, e.cg.ptr);
	sh4_emit_mov_reg(&e.cg, 4, 0);
	uint8_t* bra = e.cg.ptr;
	sh4_emit_bra(&e.cg, 0);
	sh4_emit_mov_reg(&e.cg, 5, 1);
	_patchBranch(bra, common);

	jit->dispatchSync = e.cg.ptr;
	sh4_emit_mov_l_load_gbr(&e.cg, JIT_GBR_GPRS(ARM_PC));
	sh4_emit_mov_reg(&e.cg, 0, 4);
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

	_emitGuestBase(&e);
	_registerRoutines(jit);
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

#define PSR_USER_MASK 0xF0000000
#define PSR_PRIV_MASK 0x000000CF
#define PSR_STATE_MASK 0x00000020

static void _writePC(struct JITEmitter* e, int32_t cost, int executed);
static void _armDataProcessing(struct JITEmitter* e, uint32_t op, unsigned flags);
static void _armLoadStore(struct JITEmitter* e, uint32_t op, bool mode3);
static void _armBX(struct JITEmitter* e, uint32_t op, int executed);
static void _memory(struct JITEmitter* e, enum JITMemOp op, int rd);

/* The instructions that change the cpu's mode, or may: these are the cpu
 * core's own routines behind them, run with PC and prefetch as the
 * instruction sees them. Each charges the whole instruction and returns as
 * _handlerResult. */
static uint32_t _sysSWI(struct ARMCore* cpu, uint32_t immediate, uint32_t pc) {
	_setPC(cpu, pc);
	int32_t nextEvent = cpu->nextEvent;
	uint32_t timingKey = ARMJITTimingKey(cpu);
	int32_t cycles;
	if (cpu->executionMode == MODE_THUMB) {
		cycles = 1 + cpu->memory.activeSeqCycles16;
		cpu->irqh.swi16(cpu, immediate);
	} else {
		cycles = 1 + cpu->memory.activeSeqCycles32;
		cpu->irqh.swi32(cpu, immediate);
	}
	cpu->cycles += cycles;
	return _handlerResult(cpu, _handlerEnd(cpu, pc, nextEvent, timingKey));
}

static uint32_t _writeCPSR(struct ARMCore* cpu, int32_t operand, uint32_t pc, int32_t mask) {
	_setPC(cpu, pc);
	int32_t nextEvent = cpu->nextEvent;
	uint32_t timingKey = ARMJITTimingKey(cpu);
	int32_t cycles = 1 + cpu->memory.activeSeqCycles32;
	if (mask & PSR_USER_MASK) {
		cpu->cpsr.packed = (cpu->cpsr.packed & ~PSR_USER_MASK) | (operand & PSR_USER_MASK);
	}
	if (mask & PSR_STATE_MASK) {
		cpu->cpsr.packed = (cpu->cpsr.packed & ~PSR_STATE_MASK) | (operand & PSR_STATE_MASK);
	}
	if (cpu->privilegeMode != MODE_USER && (mask & PSR_PRIV_MASK)) {
		ARMSetPrivilegeMode(cpu, (enum PrivilegeMode) ((operand & 0x0000000F) | 0x00000010));
		cpu->cpsr.packed = (cpu->cpsr.packed & ~PSR_PRIV_MASK) | (operand & PSR_PRIV_MASK);
	}
	_ARMReadCPSR(cpu);
	if (cpu->executionMode == MODE_THUMB) {
		cpu->prefetch[0] = 0x46C0; // nop
		cpu->prefetch[1] &= 0xFFFF;
		cpu->gprs[ARM_PC] += WORD_SIZE_THUMB;
	} else {
		LOAD_32(cpu->prefetch[0], (cpu->gprs[ARM_PC] - WORD_SIZE_ARM) & cpu->memory.activeMask, cpu->memory.activeRegion);
		LOAD_32(cpu->prefetch[1], cpu->gprs[ARM_PC] & cpu->memory.activeMask, cpu->memory.activeRegion);
	}
	cpu->cycles += cycles;
	return _handlerResult(cpu, _handlerEnd(cpu, pc, nextEvent, timingKey));
}

static uint32_t _sysCPSR0(struct ARMCore* cpu, uint32_t operand, uint32_t pc) {
	return _writeCPSR(cpu, operand, pc, 0);
}

static uint32_t _sysCPSRc(struct ARMCore* cpu, uint32_t operand, uint32_t pc) {
	return _writeCPSR(cpu, operand, pc, 0x000000FF);
}

static uint32_t _sysCPSRf(struct ARMCore* cpu, uint32_t operand, uint32_t pc) {
	return _writeCPSR(cpu, operand, pc, 0xFF000000);
}

static uint32_t _sysCPSRcf(struct ARMCore* cpu, uint32_t operand, uint32_t pc) {
	return _writeCPSR(cpu, operand, pc, 0xFF0000FF);
}

/* An S instruction writing the PC, which was given value: back from an
 * exception, the cpsr from the spsr. In a mode without one the flags are
 * left as they are. */
static uint32_t _sysReturn(struct ARMCore* cpu, uint32_t value, uint32_t pc) {
	_setPC(cpu, pc);
	int32_t cycles = 1 + cpu->memory.activeSeqCycles32;
	if (_ARMModeHasSPSR(cpu->cpsr.priv)) {
		cpu->cpsr = cpu->spsr;
		_ARMReadCPSR(cpu);
	}
	cpu->gprs[ARM_PC] = value;
	if (cpu->executionMode == MODE_ARM) {
		cycles += ARMWritePC(cpu);
	} else {
		cycles += ThumbWritePC(cpu);
	}
	cpu->cycles += cycles;
	return _handlerResult(cpu, 1);
}

/* LDM/STM through the memory's own routines, for what the stubs don't do.
 * op is the ARM instruction; for Thumb the one that does the same, with
 * MULTIPLE_STACK for PUSH and POP. */
#define MULTIPLE_STACK 0x10000000

static uint32_t _sysMultiple(struct ARMCore* cpu, uint32_t op, uint32_t pc) {
	_setPC(cpu, pc);
	int32_t nextEvent = cpu->nextEvent;
	uint32_t timingKey = ARMJITTimingKey(cpu);
	bool thumb = cpu->executionMode == MODE_THUMB;
	int cycles = 1 + (thumb ? cpu->memory.activeSeqCycles16 : cpu->memory.activeSeqCycles32);
	int rn = (op >> 16) & 0xF;
	int rs = op & 0xFFFF;
	bool load = op & 0x00100000;
	bool s = op & 0x00400000;
	int direction = ((op & 0x00800000) ? 0 : LSM_D) | ((op & 0x01000000) ? LSM_B : 0);
	uint32_t address = cpu->gprs[rn];
	enum PrivilegeMode privilegeMode = cpu->privilegeMode;
	bool user = s && (!load || (!(rs & 0x8000) && rs));
	if (user) {
		ARMSetPrivilegeMode(cpu, MODE_SYSTEM);
	}
	if (load) {
		address = cpu->memory.loadMultiple(cpu, address, rs, direction, &cycles);
	} else {
		address = cpu->memory.storeMultiple(cpu, address, rs, direction, &cycles);
	}
	if ((op & 0x00200000) && (!load || !((1 << rn) & rs))) {
		cpu->gprs[rn] = address;
	}
	if (user) {
		ARMSetPrivilegeMode(cpu, privilegeMode);
	} else if (s && _ARMModeHasSPSR(cpu->cpsr.priv)) {
		cpu->cpsr = cpu->spsr;
		_ARMReadCPSR(cpu);
	}
	if (thumb) {
		cycles += cpu->memory.activeNonseqCycles16 - cpu->memory.activeSeqCycles16;
	} else {
		cycles += cpu->memory.activeNonseqCycles32 - cpu->memory.activeSeqCycles32;
	}
	if (load && ((rs & 0x8000) || (!rs && !(op & MULTIPLE_STACK)))) {
		if (cpu->executionMode == MODE_THUMB) {
			cycles += ThumbWritePC(cpu);
		} else {
			cycles += ARMWritePC(cpu);
		}
	}
	cpu->cycles += cycles;
	return _handlerResult(cpu, _handlerEnd(cpu, pc, nextEvent, timingKey));
}

/* Not an instruction, a coprocessor's, a breakpoint: op is the opcode */
static uint32_t _sysIllegal(struct ARMCore* cpu, uint32_t op, uint32_t pc) {
	_setPC(cpu, pc);
	int32_t nextEvent = cpu->nextEvent;
	uint32_t timingKey = ARMJITTimingKey(cpu);
	int32_t cycles = 1 + (cpu->executionMode == MODE_THUMB ? cpu->memory.activeSeqCycles16 : cpu->memory.activeSeqCycles32);
	cpu->irqh.hitIllegal(cpu, op);
	cpu->cycles += cycles;
	return _handlerResult(cpu, _handlerEnd(cpu, pc, nextEvent, timingKey));
}

static uint32_t _sysStub(struct ARMCore* cpu, uint32_t op, uint32_t pc) {
	_setPC(cpu, pc);
	int32_t nextEvent = cpu->nextEvent;
	uint32_t timingKey = ARMJITTimingKey(cpu);
	int32_t cycles = 1 + cpu->memory.activeSeqCycles32;
	cpu->irqh.hitStub(cpu, op);
	cpu->cycles += cycles;
	return _handlerResult(cpu, _handlerEnd(cpu, pc, nextEvent, timingKey));
}

static uint32_t _sysBreakpoint(struct ARMCore* cpu, uint32_t op, uint32_t pc) {
	_setPC(cpu, pc);
	int32_t nextEvent = cpu->nextEvent;
	uint32_t timingKey = ARMJITTimingKey(cpu);
	if (cpu->executionMode == MODE_THUMB) {
		cpu->irqh.bkpt16(cpu, op & 0xFF);
	} else {
		cpu->irqh.bkpt32(cpu, ((op >> 4) & 0xFFF0) | (op & 0xF));
	}
	return _handlerResult(cpu, _handlerEnd(cpu, pc, nextEvent, timingKey));
}

static const void* const _sysRoutines[] = {
	_sysIllegal, _sysStub, _sysBreakpoint, _sysSWI, _sysCPSR0, _sysCPSRc, _sysCPSRf, _sysCPSRcf, _sysReturn, _sysMultiple,
};

static void _registerRoutines(struct ARMJIT* jit) {
	size_t i;
	for (i = 0; i < sizeof(_sysRoutines) / sizeof(*_sysRoutines); ++i) {
		ARMJITRegisterCall(jit, _sysRoutines[i]);
	}
}

/* Call one of the above with r5 as its argument. Returns if the block can
 * carry on. */
static void _sys(struct JITEmitter* e, const void* fn) {
	_charge(e);
	_lit(e, (uint32_t) (uintptr_t) fn, 2);
	_lit(e, (uint32_t) (uintptr_t) e->jit->handlers[e->thumb], 1);
	sh4_emit_jsr(&e->cg, 1);
	sh4_emit_mov_imm(&e->cg, e->index * _insnLength(e), 3);
}

/* The instructions the block compiler doesn't take as native. Returns if
 * the block can carry on. */
static void _handler(struct JITEmitter* e, uint32_t op) {
	if (e->thumb) {
		if ((op & 0xF600) == 0xB400) { // PUSH/POP
			if (op & 0x0800) {
				_imm(e, MULTIPLE_STACK | 0x08BD0000 | (op & 0xFF) | ((op & 0x100) ? 1 << ARM_PC : 0), 5);
			} else {
				_imm(e, MULTIPLE_STACK | 0x092D0000 | (op & 0xFF) | ((op & 0x100) ? 1 << ARM_LR : 0), 5);
			}
			_sys(e, _sysMultiple);
			return;
		}
		if ((op & 0xF000) == 0xC000) { // STMIA/LDMIA
			_imm(e, 0x08A00000 | ((op & 0x0800) << 9) | ((op & 0x0700) << 8) | (op & 0xFF), 5);
			_sys(e, _sysMultiple);
			return;
		}
		if ((op & 0xFF00) == 0xDF00) { // SWI
			sh4_emit_mov_imm(&e->cg, 0, 5);
			if (op & 0xFF) {
				_imm(e, op & 0xFF, 5);
			}
			_sys(e, _sysSWI);
			return;
		}
		if ((op & 0xF800) == 0xF800) { // the second half of a BL by itself
			_ld(e, ARM_LR, 4);
			_imm(e, (op & 0x07FF) << 1, 1);
			sh4_emit_add_reg(&e->cg, 1, 4);
			_imm(e, (e->address + WORD_SIZE_THUMB) | 1, 1);
			_st(e, 1, ARM_LR);
			_writePC(e, 1 + e->seq16, e->executed);
			return;
		}
		_imm(e, op, 5);
		_sys(e, (op & 0xFF00) == 0xBE00 ? (const void*) _sysBreakpoint : (const void*) _sysIllegal);
		return;
	}
	if ((op >> 28) == 0xF) {
		/* Never */
		e->pending += 1 + e->seq32;
		return;
	}
	if ((op & 0x0E000000) == 0x08000000) { // LDM/STM
		_imm(e, op & 0x0FFFFFFF, 5);
		_sys(e, _sysMultiple);
		return;
	}
	if ((op & 0x0F000000) == 0x0F000000) { // SWI
		_imm(e, op & 0xFFFFFF, 5);
		_sys(e, _sysSWI);
		return;
	}
	if ((op & 0x0E000000) == 0x0C000000 || (op & 0x0F000000) == 0x0E000000) { // coprocessor
		_imm(e, op, 5);
		_sys(e, _sysStub);
		return;
	}
	if ((op & 0x0FB000F0) == 0x01000090) { // SWP
		enum JITMemOp load = (op & 0x00400000) ? JIT_MEM_LOAD8 : JIT_MEM_LOAD32;
		enum JITMemOp store = (op & 0x00400000) ? JIT_MEM_STORE8 : JIT_MEM_STORE32;
		int address = offsetof(struct ARMCore, shifterOperand) / 4;
		int value = offsetof(struct ARMCore, shifterCarryOut) / 4;
		int32_t before = e->pending;
		/* What's stored and where are kept over the load, which may be
		 * into either's register */
		_ld(e, (op >> 16) & 0xF, 0);
		sh4_emit_mov_l_store_gbr(&e->cg, address);
		sh4_emit_mov_reg(&e->cg, 0, 4);
		_ld(e, op & 0xF, 0);
		sh4_emit_mov_l_store_gbr(&e->cg, value);
		_memory(e, load, (op >> 12) & 0xF);
		sh4_emit_mov_l_load_gbr(&e->cg, address);
		sh4_emit_mov_reg(&e->cg, 0, 4);
		sh4_emit_mov_l_load_gbr(&e->cg, value);
		sh4_emit_mov_reg(&e->cg, 0, 5);
		_memory(e, store, -1);
		e->pending = before + 1 + e->seq32;
		return;
	}
	if ((op & 0x0E000090) == 0x00000090 || (op & 0x0C000000) == 0x04000000) {
		/* A single load or store */
		bool mode3 = (op & 0x0C000000) == 0;
		bool load = op & 0x00100000;
		bool legal;
		if (mode3) {
			legal = (op & 0x60) && (load || ((op >> 5) & 3) == 1);
		} else {
			legal = (op & 0x02000010) != 0x02000010;
		}
		if (legal) {
			_armLoadStore(e, op, mode3);
			if (load && ((op >> 12) & 0xF) == ARM_PC) {
				int32_t cost = 1 + e->nonseq32;
				e->pending -= cost;
				sh4_emit_mov_reg(&e->cg, 0, 4);
				_writePC(e, cost, e->executed);
			}
			return;
		}
	}
	if ((op & 0x0FF000F0) == 0x01200010) { // BX
		_armBX(e, op, e->executed);
		return;
	}
	if ((op & 0x0FF000F0) == 0x01200070) { // BKPT
		_imm(e, op, 5);
		_sys(e, _sysBreakpoint);
		return;
	}
	if ((op & 0x0FB000F0) == 0x01000000) { // MRS
		if (op & 0x00400000) {
			sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_CPSR + 1);
		} else {
			_emitFlagsOut(e, 1);
			sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_CPSR);
		}
		_st(e, 0, (op >> 12) & 0xF);
		e->pending += 1 + e->seq32;
		return;
	}
	if ((op & 0x0FB000F0) == 0x01200000 || (op & 0x0FB00000) == 0x03200000) { // MSR
		uint32_t mask = ((op & 0x00010000) ? 0x000000FF : 0) | ((op & 0x00080000) ? 0xFF000000 : 0);
		if (op & 0x02000000) {
			_imm(e, ROR(op & 0xFF, (op & 0x00000F00) >> 7), 5);
		} else {
			_ld(e, op & 0xF, 5);
		}
		if (op & 0x00400000) {
			mask &= PSR_USER_MASK | PSR_PRIV_MASK | PSR_STATE_MASK;
			_imm(e, mask, 1);
			sh4_emit_and(&e->cg, 1, 5);
			sh4_emit_not(&e->cg, 1, 1);
			sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_CPSR + 1);
			sh4_emit_and(&e->cg, 1, 0);
			sh4_emit_or(&e->cg, 5, 0);
			sh4_emit_or_imm(&e->cg, 0x10);
			sh4_emit_mov_l_store_gbr(&e->cg, JIT_GBR_CPSR + 1);
			e->pending += 1 + e->seq32;
			return;
		}
		static const void* const routines[4] = { _sysCPSR0, _sysCPSRc, _sysCPSRf, _sysCPSRcf };
		_sys(e, routines[((op >> 16) & 1) | ((op >> 18) & 2)]);
		return;
	}
	if ((op & 0x0C000000) == 0 && ((op >> 12) & 0xF) == ARM_PC && (op & 0x0E000090) != 0x00000090) {
		/* Data processing into the PC */
		int alu = (op >> 21) & 0xF;
		if (alu < 0x8 || alu > 0xB || (op & 0x00100000)) {
			if (alu >= 0x8 && alu <= 0xB) {
				_imm(e, _pcValue(e), 4);
			} else {
				_armDataProcessing(e, op, false);
				e->pending -= 1 + e->seq32;
				sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_GPRS(ARM_PC));
				sh4_emit_mov_reg(&e->cg, 0, 4);
			}
			if (op & 0x00100000) {
				sh4_emit_mov_reg(&e->cg, 4, 5);
				_sys(e, _sysReturn);
			} else {
				_writePC(e, 1 + e->seq32, e->executed);
			}
			return;
		}
	}
	_imm(e, op, 5);
	_sys(e, _sysIllegal);
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

/* The flags condition cond (0-15) reads. */
static unsigned _condFlags(unsigned cond) {
	static const uint8_t flags[16] = {
		F_Z, F_Z, F_C, F_C, F_N, F_N, F_V, F_V,
		F_C | F_Z, F_C | F_Z, F_N | F_V, F_N | F_V, F_NZ | F_V, F_NZ | F_V, 0, F_ALL
	};
	return flags[cond & 0xF];
}

/* Whether op is translated natively, and the flags it writes and reads.
 * Handler calls count as reading every flag. A block can stop after any
 * instruction and carry on in another from the next; what that one reads
 * is the same as this one would, so no instruction has to count as reading
 * every flag for that. */
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
			case 0xD: // MUL
				*written = F_NZ;
				return true;
			default: // register shifts: C stays as it is for a shift of 0
				*written = F_NZC;
				*read = F_C;
				return true;
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
		/* PUSH/POP/STMIA/LDMIA with a list */
		if (((op & 0xF600) == 0xB400 || (op & 0xF000) == 0xC000) && (op & 0x1FF) &&
		    ((op & 0xF000) == 0xB000 || (op & 0xFF))) {
			return true;
		}
		*read = F_ALL;
		return false;
	default:
		*read = F_ALL;
		return false;
	}
}

static void _shiftConst(struct JITEmitter* e, int r, int n, int kind);

static void _thumbShiftImmediate(struct JITEmitter* e, uint32_t op, unsigned flags) {
	int immediate = (op >> 6) & 0x1F;
	int rm = (op >> 3) & 7;
	int rd = op & 7;
	int w = _pinned(rd);
	_ld(e, rm, w);
	if (immediate && !(flags & F_C)) {
		_shiftConst(e, w, immediate, op >> 11);
		_put(e, w, rd);
		if (flags & F_NZ) {
			_flagsNZ(e, w);
		}
		return;
	}
	switch (op >> 11) {
	case 0x00: // LSL
		if (!immediate) {
			_put(e, w, rd);
			if (flags & F_NZ) {
				_flagsNZ(e, w);
			}
			return;
		}
		if (immediate > 1) {
			sh4_emit_mov_imm(&e->cg, immediate - 1, 2);
			sh4_emit_shld(&e->cg, 2, w);
		}
		sh4_emit_shll(&e->cg, w);
		break;
	case 0x01: // LSR
		if (!immediate) {
			sh4_emit_shll(&e->cg, w);
			sh4_emit_mov_imm(&e->cg, 0, w);
			break;
		}
		if (immediate > 1) {
			sh4_emit_mov_imm(&e->cg, 1 - immediate, 2);
			sh4_emit_shld(&e->cg, 2, w);
		}
		sh4_emit_shlr(&e->cg, w);
		break;
	default: // ASR
		if (!immediate) {
			/* T = sign; w - w - T is then 0 or -1 and leaves T alone */
			sh4_emit_shll(&e->cg, w);
			sh4_emit_subc(&e->cg, w, w);
			break;
		}
		if (immediate > 1) {
			sh4_emit_mov_imm(&e->cg, 1 - immediate, 2);
			sh4_emit_shad(&e->cg, 2, w);
		}
		sh4_emit_shar(&e->cg, w);
		break;
	}
	if (flags & F_C) {
		_flagsCFromT(e, flags);
	}
	_put(e, w, rd);
	if (flags & F_NZ) {
		_flagsNZ(e, w);
	}
}

/* LSL/LSR/ASR/ROR by the low byte of a register: r1 by r2. */
static void _thumbShiftRegister(struct JITEmitter* e, int alu, int rd, unsigned flags) {
	sh4_emit_extu_b(&e->cg, 2, 2);
	sh4_emit_tst(&e->cg, 2, 2);
	uint8_t* none = e->cg.ptr;
	sh4_emit_bt(&e->cg, 0);
	if (alu == 0x7) { // ROR
		sh4_emit_mov_imm(&e->cg, 31, 0);
		sh4_emit_and(&e->cg, 0, 2);
		sh4_emit_mov_reg(&e->cg, 1, 3);
		sh4_emit_neg(&e->cg, 2, 0);
		sh4_emit_shld(&e->cg, 0, 1);
		sh4_emit_add_imm(&e->cg, 32, 0);
		sh4_emit_shld(&e->cg, 0, 3);
		sh4_emit_or(&e->cg, 3, 1);
		if (flags & F_C) {
			/* C is the result's top bit, also for a multiple of 32 */
			_signToT(e, 1);
			_flagsCFromT(e, flags);
		}
	} else {
		/* By one less and then by one, which leaves C in T. Above 32 is 32
		 * of nothing, or for ASR just 32. */
		sh4_emit_mov_imm(&e->cg, 32, 3);
		sh4_emit_cmphi(&e->cg, 3, 2);
		uint8_t* within = e->cg.ptr;
		sh4_emit_bf(&e->cg, 0);
		sh4_emit_mov_reg(&e->cg, 3, 2);
		if (alu != 0x4) {
			sh4_emit_mov_imm(&e->cg, 0, 1);
		}
		_patchBranch(within, e->cg.ptr);
		sh4_emit_add_imm(&e->cg, -1, 2);
		switch (alu) {
		case 0x2: // LSL
			sh4_emit_shld(&e->cg, 2, 1);
			sh4_emit_shll(&e->cg, 1);
			break;
		case 0x3: // LSR
			sh4_emit_neg(&e->cg, 2, 2);
			sh4_emit_shld(&e->cg, 2, 1);
			sh4_emit_shlr(&e->cg, 1);
			break;
		default: // ASR
			sh4_emit_neg(&e->cg, 2, 2);
			sh4_emit_shad(&e->cg, 2, 1);
			sh4_emit_shar(&e->cg, 1);
			break;
		}
		if (flags & F_C) {
			_flagsCFromT(e, flags);
		}
	}
	_patchBranch(none, e->cg.ptr);
	_st(e, 1, rd);
	if (flags & F_NZ) {
		_flagsNZ(e, 1);
	}
	e->pending += 1;
}

/* ARM_WAIT_SMUL/UMUL of the value in a: r1 = its top bytes that are more
 * than sign (or zero) as they are, the rest zeroed by the fold. r3 goes. */
/* r1 = a >> 1 (arithmetic for sign) for _multiplyWait, which then has
 * what it needs of a: the low bit never counts. */
static void _multiplyFold(struct JITEmitter* e, bool sign, int a) {
	sh4_emit_mov_reg(&e->cg, a, 1);
	if (sign) {
		sh4_emit_shar(&e->cg, 1);
	} else {
		sh4_emit_shlr(&e->cg, 1);
	}
}

/* base + 1-4 by how many bytes of f above the lowest are nonzero, f the
 * operand (a ^ (a >> 31) for sign), from r1 = a >> 1. Most operands are
 * small: f < 256 is r1 fitting in a signed byte. Bigger ones add each
 * byte's T in with addc. r1-r3 go; r0 is kept (the stall model keeps
 * it too). */
static void _multiplyWait(struct JITEmitter* e, int base, bool sign) {
	int acc = e->stall ? 2 : R_CYCLES;
	sh4_emit_exts_b(&e->cg, 1, 3);
	sh4_emit_cmpeq(&e->cg, 1, 3);
	uint8_t* done = e->cg.ptr;
	sh4_emit_bt_s(&e->cg, 0);
	if (e->stall) {
		sh4_emit_mov_imm(&e->cg, base + 1, 2);
	} else {
		sh4_emit_mov_imm(&e->cg, 0, 3);
		e->pending += base + 1;
	}
	if (sign) {
		/* r1 = f >> 1 */
		sh4_emit_cmppz(&e->cg, 1);
		sh4_emit_subc(&e->cg, 3, 3);
		sh4_emit_not(&e->cg, 1, 1);
		sh4_emit_xor(&e->cg, 3, 1);
	}
	sh4_emit_shll(&e->cg, 1);
	sh4_emit_mov_imm(&e->cg, 0, 3);
	sh4_emit_add_imm(&e->cg, 1, acc);
	sh4_emit_shlr16(&e->cg, 1);
	sh4_emit_cmppl(&e->cg, 1);
	sh4_emit_addc(&e->cg, 3, acc);
	sh4_emit_shlr8(&e->cg, 1);
	sh4_emit_cmppl(&e->cg, 1);
	sh4_emit_addc(&e->cg, 3, acc);
	_patchBranch(done, e->cg.ptr);
	if (e->stall) {
		_lit(e, (uint32_t) (uintptr_t) e->jit->stall, 1);
		sh4_emit_jsr(&e->cg, 1);
		sh4_emit_mov_imm(&e->cg, e->index * _insnLength(e), 3);
		e->usesBase = true;
	}
}

/* a * b into rd. The wait is ARM_WAIT_SMUL of what was in rd (a). */
static void _thumbMultiply(struct JITEmitter* e, int rd, int a, int b, unsigned flags) {
	sh4_emit_mul_l(&e->cg, b, a);
	_multiplyFold(e, true, a);
	int dst = _pinned(rd);
	sh4_emit_sts_macl(&e->cg, dst);
	if (flags & F_NZ) {
		_flagsNZ(e, dst);
	}
	_multiplyWait(e, 0, true);
	_st(e, dst, rd);
	e->pending += e->nonseq16 - e->seq16;
}

static void _thumbAlu(struct JITEmitter* e, uint32_t op, unsigned flags) {
	int rd = op & 7;
	int rn = (op >> 3) & 7;
	int alu = (op >> 6) & 0xF;
	if (alu == 0x9) { // NEG
		if (!(flags & (F_C | F_V)) && !(e->fuseCond >= 0 && !_fusesByResult(e->fuseCond))) {
			int w = _pinned(rd);
			sh4_emit_neg(&e->cg, _getR0(e, rn), w);
			_put(e, w, rd);
			if (e->fuseCond >= 0) {
				_fuseResult(e, w);
			} else if (flags & F_NZ) {
				_flagsNZ(e, w);
			}
			return;
		}
		sh4_emit_mov_imm(&e->cg, 0, 1);
		_addSub(e, true, rd, flags, 1, _getR0(e, rn));
		return;
	}
	/* whichever of them isn't pinned last, in r0 */
	int b = _pinned(rd) || alu == 0xF ? _getR0(e, rn) : _get(e, rn, 2);
	int a = alu == 0xF ? 1 : _getR0(e, rd);
	int w;
	switch (alu) {
	case 0x0: // AND
		w = _into(e, rd, &a, &b, true);
		sh4_emit_and(&e->cg, b, w);
		break;
	case 0x1: // EOR
		w = _into(e, rd, &a, &b, true);
		sh4_emit_xor(&e->cg, b, w);
		break;
	case 0x2: // LSL
	case 0x3: // LSR
	case 0x4: // ASR
	case 0x7: // ROR
		_toScratch(e, a, b);
		_thumbShiftRegister(e, alu, rd, flags);
		return;
	case 0x5: // ADC
		_toScratch(e, a, b);
		_addCarry(e, false, rd, flags);
		return;
	case 0x6: // SBC
		_toScratch(e, a, b);
		_addCarry(e, true, rd, flags);
		return;
	case 0xD: // MUL
		_thumbMultiply(e, rd, a, b, flags);
		return;
	case 0x8: // TST
		if (e->fuseCond >= 0 && _fuseTst(e, a, b)) {
			return;
		}
		if (flags & F_NZ) {
			sh4_emit_mov_reg(&e->cg, a, R_NZ);
			sh4_emit_and(&e->cg, b, R_NZ);
		}
		return;
	case 0xA: // CMP
		_addSub(e, true, -1, flags, a, b);
		return;
	case 0xB: // CMN
		_addSub(e, false, -1, flags, a, b);
		return;
	case 0xC: // ORR
		w = _into(e, rd, &a, &b, true);
		sh4_emit_or(&e->cg, b, w);
		break;
	case 0xE: // BIC
		sh4_emit_not(&e->cg, b, 2);
		b = 2;
		w = _into(e, rd, &a, &b, false);
		sh4_emit_and(&e->cg, 2, w);
		break;
	default: // MVN
		w = _dst(rd);
		sh4_emit_not(&e->cg, b, w);
		break;
	}
	_put(e, w, rd);
	if (e->fuseCond >= 0 && _fuseResult(e, w)) {
		return;
	}
	if (flags & F_NZ) {
		_flagsNZ(e, w);
	}
}

static void _thumbTranslate(struct JITEmitter* e, uint32_t op, unsigned flags) {
	int rd;
	switch (op >> 11) {
	case 0x00:
	case 0x01:
	case 0x02:
		_thumbShiftImmediate(e, op, flags);
		break;
	case 0x03: { // ADD/SUB register or 3-bit immediate
		if (op & 0x0400) {
			_addSubImm(e, op & 0x0200, op & 7, flags, _getR0(e, (op >> 3) & 7), (op >> 6) & 7);
		} else {
			int b = _get(e, (op >> 6) & 7, 2);
			_addSub(e, op & 0x0200, op & 7, flags, _getR0(e, (op >> 3) & 7), b);
		}
		break;
	}
	case 0x04: { // MOV immediate
		rd = (op >> 8) & 7;
		int w = _dst(rd);
		_imm(e, op & 0xFF, w);
		_put(e, w, rd);
		if (flags & F_NZ) {
			_flagsNZ(e, w);
		}
		break;
	}
	case 0x05: // CMP immediate
	case 0x06: // ADD immediate
	case 0x07: // SUB immediate
		rd = (op >> 8) & 7;
		_addSubImm(e, (op >> 11) != 0x06, (op >> 11) == 0x05 ? -1 : rd, flags, _getR0(e, rd), op & 0xFF);
		break;
	case 0x08:
		if (!(op & 0x0400)) {
			_thumbAlu(e, op, flags);
			break;
		}
		rd = (op & 7) | ((op >> 4) & 8);
		switch ((op >> 8) & 3) {
		case 0: { // ADD, no flags
			int b = _get(e, (op >> 3) & 0xF, 2);
			_addSub(e, false, rd, 0, _getR0(e, rd), b);
			break;
		}
		case 1: { // CMP
			int b = _get(e, (op >> 3) & 0xF, 2);
			_addSub(e, true, -1, flags, _getR0(e, rd), b);
			break;
		}
		case 2: // MOV, no flags
			if (_pinned(rd)) {
				_ld(e, (op >> 3) & 0xF, _pinned(rd));
			} else {
				_st(e, _getR0(e, (op >> 3) & 0xF), rd);
			}
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
	case 0x14: { // ADD Rd, PC, #imm
		rd = (op >> 8) & 7;
		int w = _dst(rd);
		_imm(e, (_pcValue(e) & ~3) + ((op & 0xFF) << 2), w);
		_put(e, w, rd);
		break;
	}
	case 0x15: // ADD Rd, SP, #imm
		_addSubImm(e, false, (op >> 8) & 7, 0, _get(e, ARM_SP, 1), (op & 0xFF) << 2);
		break;
	case 0x16: // ADD/SUB SP, #imm
		_addSubImm(e, op & 0x0080, ARM_SP, 0, _get(e, ARM_SP, 1), (op & 0x7F) << 2);
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
	_count(e, executed);
	_site(e, JIT_KEY(next, e->thumb), JIT_EXIT_SITE_FALL);
}

/* T from the flags for Thumb/ARM condition cond (0-13). Returns whether the
 * condition holds when T is set (otherwise when it is clear). */
static bool _condition(struct JITEmitter* e, int cond) {
	if (e->fused) {
		e->fused = false;
		return e->fusedIfT;
	}
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
	/* charged on each way out, in the link's delay slot */
	int32_t pending = e->pending += 1 + e->seq16;
	bool takenIfT = _condition(e, (op >> 8) & 0xF);
	uint8_t* skip = e->cg.ptr;
	if (takenIfT) {
		sh4_emit_bt(&e->cg, 0);
	} else {
		sh4_emit_bf(&e->cg, 0);
	}
	_fallThrough(e, e->address + WORD_SIZE_THUMB, executed);
	_patchBranch(skip, e->cg.ptr);
	e->pending = pending;
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

/* Leave for the address in r4, in the mode its bit 0 says: the C side does
 * the mode, the region and the branch's cycles. What the instruction itself
 * costs is pending. */
static void _exitAnywhere(struct JITEmitter* e, int executed) {
	if (e->region == REGION_BIOS) {
		/* What a read of the BIOS from outside gets is the last word it
		 * fetched: the one two after this instruction. */
		struct GBA* gba = (struct GBA*) e->jit->cpu->master;
		uint32_t at = (e->address + 2 * _insnLength(e)) & (SIZE_BIOS - 1);
		uint32_t word;
		if (e->thumb) {
			LOAD_16(word, at, gba->memory.bios);
		} else {
			LOAD_32(word, at, gba->memory.bios);
		}
		_lit(e, word, 0);
		sh4_emit_mov_l_store_gbr(&e->cg, JIT_GBR_PREFETCH1);
	}
	_charge(e);
	_count(e, executed);
	sh4_emit_mov_reg(&e->cg, 4, 0);
	_jumpTo(e, e->jit->exits[JIT_EXIT_BRANCH]);
	sh4_emit_nop(&e->cg);
}

/* A write to the PC that stays in the mode, the new value in r4: within the
 * region the target is looked up here. */
static void _writePC(struct JITEmitter* e, int32_t cost, int executed) {
	if (e->thumb) {
		sh4_emit_mov_reg(&e->cg, 4, 0);
		sh4_emit_or_imm(&e->cg, 1);
		sh4_emit_mov_reg(&e->cg, 0, 4);
	} else {
		sh4_emit_mov_imm(&e->cg, -2, 0);
		sh4_emit_and(&e->cg, 0, 4);
	}
	_charge(e);
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
	_exitAnywhere(e, executed);
}

/* BX Rm: within the region and staying in Thumb, look the target up here. */
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
	e->pending = 1 + e->seq16;
	_exitAnywhere(e, executed);
}

/* ADD/MOV into the PC */
static void _thumbHighPC(struct JITEmitter* e, uint32_t op, int executed) {
	_ld(e, (op >> 3) & 0xF, 4);
	if (!(op & 0x0200)) {
		_imm(e, _pcValue(e), 1);
		sh4_emit_add_reg(&e->cg, 1, 4);
	}
	_writePC(e, 1 + e->seq16, executed);
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
	_imm(e, 0x08000000 | (before << 24) | (up << 23) | (writeback << 21) | (!store << 20) | (rn << 16) | mask |
	        (e->thumb && rn == ARM_SP ? MULTIPLE_STACK : 0), 5);
	sh4_emit_mov_imm(&e->cg, n, 2);
	_lit(e, (uint32_t) (uintptr_t) e->jit->multipleStubs[e->stall][store][e->thumb], 1);
	sh4_emit_jsr(&e->cg, 1);
	sh4_emit_mov_imm(&e->cg, e->index * _insnLength(e), 3);
	e->usesBase |= e->stall;
	uint8_t* slow = e->cg.ptr;
	sh4_emit_bt(&e->cg, 0);
	/* out of r0, which the guest registers go through */
	sh4_emit_mov_reg(&e->cg, 0, 2);
	if (store) {
		sh4_emit_add_imm(&e->cg, 4 * n, 2);
		for (r = 15; r >= 0; --r) {
			if (mask & (1 << r)) {
				sh4_emit_mov_l_store_dec(&e->cg, _getR0(e, r), 2);
			}
		}
	} else {
		for (r = 0; r < 16; ++r) {
			if (mask & (1 << r)) {
				int h = r == ARM_PC ? 4 : _pinned(r);
				sh4_emit_mov_l_load_inc(&e->cg, 2, h);
				if (r != ARM_PC) {
					_put(e, h, r);
				}
			}
		}
	}
	if (writeback && (store || !(mask & (1 << rn)))) {
		int w = _dst(rn);
		_ld(e, rn, w);
		_addImmediate(e, up ? 4 * n : -4 * n, w);
		_put(e, w, rn);
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
	CARRY_1,
	/* bit 0 of r3 */
	CARRY_R3
};

/* r <<= n, r >>= n (logical or arithmetic) for 1 <= n <= 31; r0 is
 * scratch, or r3 when r is r0. */
static void _shiftConst(struct JITEmitter* e, int r, int n, int kind) {
	int count = r ? 0 : 3;
	switch (kind) {
	case 0:
		switch (n) {
		case 1: sh4_emit_shll(&e->cg, r); return;
		case 2: sh4_emit_shll2(&e->cg, r); return;
		case 8: sh4_emit_shll8(&e->cg, r); return;
		case 16: sh4_emit_shll16(&e->cg, r); return;
		}
		sh4_emit_mov_imm(&e->cg, n, count);
		sh4_emit_shld(&e->cg, count, r);
		return;
	case 1:
		switch (n) {
		case 1: sh4_emit_shlr(&e->cg, r); return;
		case 2: sh4_emit_shlr2(&e->cg, r); return;
		case 8: sh4_emit_shlr8(&e->cg, r); return;
		case 16: sh4_emit_shlr16(&e->cg, r); return;
		}
		sh4_emit_mov_imm(&e->cg, -n, count);
		sh4_emit_shld(&e->cg, count, r);
		return;
	default:
		if (n == 1) {
			sh4_emit_shar(&e->cg, r);
			return;
		}
		sh4_emit_mov_imm(&e->cg, -n, count);
		sh4_emit_shad(&e->cg, count, r);
		return;
	}
}

/* The shift-by-immediate forms (addressing modes 1 and 2) of guest rm into
 * host dst, as mGBA's _shift* and ADDR_MODE_2_* compute them. With carry,
 * says where the shifter carry-out is (CARRY_T: in T). r0 and r3 are
 * scratch; dst may be r0 but not for ROR. */
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

/* The shift-by-register forms of guest rm into host dst, as mGBA's _shift*.
 * With carry the carry-out is left in r3. r0, r4 and r5 are scratch. */
static int _armShiftRegister(struct JITEmitter* e, uint32_t op, int dst, bool carry) {
	int rm = op & 0xF;
	if (rm == ARM_PC) {
		_imm(e, _pcValue(e) + WORD_SIZE_ARM, dst);
	} else {
		_ld(e, rm, dst);
	}
	_ld(e, (op >> 8) & 0xF, 0);
	sh4_emit_extu_b(&e->cg, 0, 0);
	if (carry) {
		sh4_emit_mov_reg(&e->cg, R_CV, 3);
		sh4_emit_rotl(&e->cg, 3);
	}
	sh4_emit_tst(&e->cg, 0, 0);
	uint8_t* none = e->cg.ptr;
	sh4_emit_bt(&e->cg, 0);
	int type = (op >> 5) & 3;
	if (type == 3) { // ROR
		sh4_emit_mov_imm(&e->cg, 31, 4);
		sh4_emit_and(&e->cg, 4, 0);
		sh4_emit_mov_reg(&e->cg, dst, 4);
		sh4_emit_neg(&e->cg, 0, 5);
		sh4_emit_shld(&e->cg, 5, dst);
		sh4_emit_add_imm(&e->cg, 32, 5);
		sh4_emit_shld(&e->cg, 5, 4);
		sh4_emit_or(&e->cg, 4, dst);
		if (carry) {
			sh4_emit_mov_reg(&e->cg, dst, 3);
			sh4_emit_rotl(&e->cg, 3);
		}
	} else {
		sh4_emit_mov_imm(&e->cg, 32, 4);
		sh4_emit_cmphi(&e->cg, 4, 0);
		uint8_t* within = e->cg.ptr;
		sh4_emit_bf(&e->cg, 0);
		sh4_emit_mov_reg(&e->cg, 4, 0);
		if (type != 2) {
			sh4_emit_mov_imm(&e->cg, 0, dst);
		}
		_patchBranch(within, e->cg.ptr);
		sh4_emit_add_imm(&e->cg, -1, 0);
		switch (type) {
		case 0: // LSL
			sh4_emit_shld(&e->cg, 0, dst);
			sh4_emit_shll(&e->cg, dst);
			break;
		case 1: // LSR
			sh4_emit_neg(&e->cg, 0, 0);
			sh4_emit_shld(&e->cg, 0, dst);
			sh4_emit_shlr(&e->cg, dst);
			break;
		default: // ASR
			sh4_emit_neg(&e->cg, 0, 0);
			sh4_emit_shad(&e->cg, 0, dst);
			sh4_emit_shar(&e->cg, dst);
			break;
		}
		if (carry) {
			sh4_emit_movt(&e->cg, 3);
		}
	}
	_patchBranch(none, e->cg.ptr);
	e->pending += 1;
	return carry ? CARRY_R3 : CARRY_KEEP;
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

static void _armDataProcessing(struct JITEmitter* e, uint32_t op, unsigned flags) {
	int alu = (op >> 21) & 0xF;
	int rd = (op >> 12) & 0xF;
	int rn = (op >> 16) & 0xF;
	bool logical = _armIsLogical(alu);
	bool reverse = alu == 0x3 || alu == 0x7; // RSB, RSC
	int dst = reverse ? 1 : 2;
	int src = reverse ? 2 : 1;
	if (!(op & 0x00100000)) {
		flags = 0;
	}
	bool shifterC = logical && (flags & F_C);
	int carry;
	/* operand 2's host register */
	int b = dst;
	/* BIC/MVN of an immediate: b already holds it inverted */
	bool inverted = false;
	/* ADD/SUB/CMP/CMN of an immediate: _addSubImm has it, b is -1 */
	bool immediate = false;
	uint32_t value = 0;
	if (op & 0x02000000) {
		int rotate = (op >> 7) & 0x1E;
		value = ROR(op & 0xFF, rotate);
		inverted = alu == 0xE || alu == 0xF;
		immediate = alu == 0x2 || alu == 0x4 || alu == 0xA || alu == 0xB;
		if (immediate) {
			b = -1;
		} else if (reverse) {
			_imm(e, value, dst);
		}
		carry = !rotate ? CARRY_KEEP : value >> 31 ? CARRY_1 : CARRY_0;
	} else if (op & 0x00000010) {
		carry = _armShiftRegister(e, op, dst, shifterC);
	} else if (!(op & 0x00000FF0)) {
		/* in r0 if rn won't be loaded after it */
		if (!reverse && (alu == 0xD || alu == 0xF || _pinned(rn))) {
			b = _getR0(e, op & 0xF);
		} else {
			b = _get(e, op & 0xF, dst);
		}
		carry = CARRY_KEEP;
	} else {
		/* straight into rd's pin for MOV; else in r0 if rn won't be
		 * loaded after it and it doesn't cost a move */
		bool ror = ((op >> 5) & 3) == 3;
		if (alu == 0xD) {
			b = _pinned(rd);
		} else if (!reverse && !ror && _pinned(rn) && alu != 0x5 && alu != 0x6 && alu != 0x7 &&
		           (alu != 0x2 || _pinned(rd))) {
			b = 0;
		}
		if (b == 0 && ror) {
			b = dst;
		}
		carry = _armShiftImmediate(e, op, b, shifterC);
	}
	if (shifterC && carry != CARRY_KEEP) {
		/* C now: straight in with V dead, else by way of r3 at the end */
		if (!(flags & F_V)) {
			switch (carry) {
			case CARRY_T:
				break;
			case CARRY_0:
				sh4_emit_clrt(&e->cg);
				break;
			case CARRY_1:
				sh4_emit_sett(&e->cg);
				break;
			default:
				sh4_emit_shlr(&e->cg, 3);
				break;
			}
			sh4_emit_rotcr(&e->cg, R_CV);
			carry = CARRY_KEEP;
		} else if (carry == CARRY_T) {
			sh4_emit_movt(&e->cg, 3);
		} else if (carry == CARRY_0 || carry == CARRY_1) {
			sh4_emit_mov_imm(&e->cg, carry == CARRY_1, 3);
		}
	} else {
		carry = CARRY_KEEP;
	}
	/* rn's host register */
	int a = src;
	if (alu != 0xD && alu != 0xF) {
		if (rn == ARM_PC && (op & 0x02000010) == 0x00000010) {
			_imm(e, _pcValue(e) + WORD_SIZE_ARM, src);
		} else if (reverse) {
			a = _get(e, rn, src);
		} else {
			a = _getR0(e, rn);
		}
	}
	/* an immediate after rn, so rn can come straight from a store */
	if ((op & 0x02000000) && !immediate && !reverse) {
		_imm(e, inverted ? ~value : value, dst);
	}
	/* where a logical result is */
	int w = 1;
	switch (alu) {
	case 0x0: // AND
		w = _into(e, rd, &a, &b, true);
		sh4_emit_and(&e->cg, b, w);
		break;
	case 0x1: // EOR
		w = _into(e, rd, &a, &b, true);
		sh4_emit_xor(&e->cg, b, w);
		break;
	case 0x8: // TST
	case 0x9: // TEQ
		if (alu == 0x8 && e->fuseCond >= 0 && _fuseTst(e, a, b)) {
			break;
		}
		if (!(flags & F_NZ)) {
			break;
		}
		w = R_NZ;
		sh4_emit_mov_reg(&e->cg, a, w);
		if (alu == 0x8) {
			sh4_emit_and(&e->cg, b, w);
		} else {
			sh4_emit_xor(&e->cg, b, w);
		}
		break;
	case 0x2: // SUB
		if (immediate) {
			_addSubImm(e, true, rd, flags, a, value);
			break;
		}
		_addSub(e, true, rd, flags, a, b);
		break;
	case 0x3: // RSB
		_addSub(e, true, rd, flags, b, a);
		break;
	case 0x4: // ADD
		if (immediate) {
			_addSubImm(e, false, rd, flags, a, value);
			break;
		}
		_addSub(e, false, rd, flags, a, b);
		break;
	case 0x5: // ADC
		_toScratch(e, a, b);
		_addCarry(e, false, rd, flags);
		break;
	case 0x6: // SBC
		_toScratch(e, a, b);
		_addCarry(e, true, rd, flags);
		break;
	case 0x7: // RSC
		_toScratch(e, b, a);
		_addCarry(e, true, rd, flags);
		break;
	case 0xA: // CMP
	case 0xB: // CMN
		if (immediate) {
			_addSubImm(e, alu == 0xA, -1, flags, a, value);
			break;
		}
		_addSub(e, alu == 0xA, -1, flags, a, b);
		break;
	case 0xC: // ORR
		w = _into(e, rd, &a, &b, true);
		sh4_emit_or(&e->cg, b, w);
		break;
	case 0xD: // MOV
		w = _pinned(rd) ? _pinned(rd) : b;
		if (w != b) {
			sh4_emit_mov_reg(&e->cg, b, w);
		}
		break;
	case 0xE: // BIC
		if (!inverted) {
			sh4_emit_not(&e->cg, b, 2);
			b = 2;
		}
		w = _into(e, rd, &a, &b, true);
		sh4_emit_and(&e->cg, b, w);
		break;
	case 0xF: // MVN
		if (inverted) {
			w = _pinned(rd) ? _pinned(rd) : b;
			if (w != b) {
				sh4_emit_mov_reg(&e->cg, b, w);
			}
			break;
		}
		w = _dst(rd);
		sh4_emit_not(&e->cg, b, w);
		break;
	}
	if (logical) {
		if (alu < 0x8 || alu > 0xB) {
			_put(e, w, rd);
		}
		if (carry != CARRY_KEEP) {
			_flagsC(e, 3);
		}
		if (e->fuseCond >= 0 && _fuseResult(e, w)) {
			flags &= ~F_NZ;
		}
		if (flags & F_NZ) {
			_flagsNZ(e, w);
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

/* r += or -= the offset in r1 */
static void _addOffset(struct JITEmitter* e, bool up, int r) {
	if (up) {
		sh4_emit_add_reg(&e->cg, 1, r);
	} else {
		sh4_emit_sub(&e->cg, 1, r);
	}
}

/* r += the offset: small, the immediate itself, else in r1 */
static void _addOffsetTo(struct JITEmitter* e, bool small, int32_t offset, bool up, int r) {
	if (!small) {
		_addOffset(e, up, r);
	} else if (offset) {
		sh4_emit_add_imm(&e->cg, offset, r);
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
		if (rd == ARM_PC) {
			_imm(e, _pcValue(e) + WORD_SIZE_ARM, 5);
		} else {
			_ld(e, rd, 5);
		}
	}
	/* An immediate offset that fits goes in with add #imm */
	bool immediate = mode3 ? op & 0x00400000 : !(op & 0x02000000);
	int32_t offset = 0;
	bool small = false;
	if (immediate) {
		offset = mode3 ? ((op >> 4) & 0xF0) | (op & 0xF) : op & 0xFFF;
		if (!up) {
			offset = -offset;
		}
		small = offset >= -128 && offset <= 127;
	}
	if (!small) {
		_armOffset(e, op, mode3);
	}
	/* The PC isn't written back to */
	if (rn == ARM_PC) {
		if (small) {
			_imm(e, _pcValue(e) + (pre ? offset : 0), 4);
		} else {
			_imm(e, _pcValue(e), 4);
			if (pre) {
				_addOffset(e, up, 4);
			}
		}
	} else if (_pinned(rn)) {
		int pin = _pinned(rn);
		if (pre && !writeback) {
			sh4_emit_mov_reg(&e->cg, pin, 4);
			_addOffsetTo(e, small, offset, up, 4);
		} else if (pre) {
			_addOffsetTo(e, small, offset, up, pin);
			sh4_emit_mov_reg(&e->cg, pin, 4);
		} else {
			sh4_emit_mov_reg(&e->cg, pin, 4);
			_addOffsetTo(e, small, offset, up, pin);
		}
	} else {
		sh4_emit_mov_l_load_gbr(&e->cg, JIT_GBR_GPRS(rn));
		if (pre) {
			_addOffsetTo(e, small, offset, up, 0);
			if (writeback) {
				sh4_emit_mov_l_store_gbr(&e->cg, JIT_GBR_GPRS(rn));
			}
			sh4_emit_mov_reg(&e->cg, 0, 4);
		} else {
			sh4_emit_mov_reg(&e->cg, 0, 4);
			if (!small || offset) {
				_addOffsetTo(e, small, offset, up, 0);
				sh4_emit_mov_l_store_gbr(&e->cg, JIT_GBR_GPRS(rn));
			}
		}
	}
	_memory(e, mop, load ? rd : -1);
}

/* MUL/MLA. With S the C flag is left as it is. */
static void _armMultiply(struct JITEmitter* e, uint32_t op, unsigned flags) {
	int rd = (op >> 16) & 0xF;
	bool accumulate = op & 0x00200000;
	e->pending += 1 + e->nonseq32;
	if (rd == ARM_PC) {
		return;
	}
	/* mul.l leaves its operands alone: a can be in r0 if b won't be */
	int a = _pinned((op >> 8) & 0xF) ? _getR0(e, op & 0xF) : _get(e, op & 0xF, 1);
	int b = _getR0(e, (op >> 8) & 0xF);
	sh4_emit_mul_l(&e->cg, b, a);
	_multiplyFold(e, true, b);
	int dst = _pinned(rd);
	if (accumulate) {
		int c = _get(e, (op >> 12) & 0xF, 4);
		if (c == dst) {
			sh4_emit_sts_macl(&e->cg, 5);
			sh4_emit_add_reg(&e->cg, 5, dst);
		} else {
			sh4_emit_sts_macl(&e->cg, dst);
			sh4_emit_add_reg(&e->cg, c, dst);
		}
	} else {
		sh4_emit_sts_macl(&e->cg, dst);
	}
	if ((flags & F_NZ) && (op & 0x00100000)) {
		_flagsNZ(e, dst);
	}
	_multiplyWait(e, accumulate ? 1 : 0, true);
	_st(e, dst, rd);
}

/* UMULL, UMLAL, SMULL, SMLAL */
static void _armMultiplyLong(struct JITEmitter* e, uint32_t op, unsigned flags) {
	int rdHi = (op >> 16) & 0xF;
	int rdLo = (op >> 12) & 0xF;
	bool accumulate = op & 0x00200000;
	bool sign = op & 0x00400000;
	e->pending += 1 + e->nonseq32;
	if (rdHi == ARM_PC || rdLo == ARM_PC) {
		return;
	}
	int a = _get(e, op & 0xF, 1);
	int b = _get(e, (op >> 8) & 0xF, 2);
	if (sign) {
		sh4_emit_dmuls_l(&e->cg, b, a);
	} else {
		sh4_emit_dmulu_l(&e->cg, b, a);
	}
	_multiplyFold(e, sign, b);
	sh4_emit_sts_macl(&e->cg, 5);
	sh4_emit_sts_mach(&e->cg, 2);
	if (accumulate) {
		_ld(e, rdHi, 4);
		_ld(e, rdLo, 0);
		sh4_emit_clrt(&e->cg);
		sh4_emit_addc(&e->cg, 0, 5);
		sh4_emit_addc(&e->cg, 4, 2);
	}
	_st(e, 5, rdLo);
	_st(e, 2, rdHi);
	if ((flags & F_NZ) && (op & 0x00100000)) {
		/* N is the top word's, Z is for the two */
		sh4_emit_mov_reg(&e->cg, 2, 0);
		sh4_emit_tst(&e->cg, 5, 5);
		uint8_t* zero = e->cg.ptr;
		sh4_emit_bt(&e->cg, 0);
		sh4_emit_or_imm(&e->cg, 1);
		_patchBranch(zero, e->cg.ptr);
		sh4_emit_mov_reg(&e->cg, 0, R_NZ);
	}
	_multiplyWait(e, accumulate ? 2 : 1, sign);
}

/* Whether op is translated natively (B/BL/BX are handled by the block
 * loop), and the flags it writes and reads. */
static bool _armAnalyze(uint32_t op, bool stall, unsigned* written, unsigned* read) {
	unsigned cond = op >> 28;
	*written = 0;
	*read = _condFlags(cond);
	if (cond == 0xF) {
		*read = F_ALL;
		return false;
	}
	int rd = (op >> 12) & 0xF;
	int rn = (op >> 16) & 0xF;
	int rm = op & 0xF;
	if ((op & 0x0FC000F0) == 0x00000090) { // MUL/MLA
		if (op & 0x00100000) {
			*written = F_NZ;
		}
		return true;
	}
	if ((op & 0x0F8000F0) == 0x00800090) { // UMULL and the like
		if (op & 0x00100000) {
			*written = F_NZ;
		}
		return true;
	}
	if ((op & 0x0E000090) == 0x00000090) {
		if (!(op & 0x60) || (!(op & 0x00100000) && ((op >> 5) & 3) != 1)) { // SWP, LDRD/STRD
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
		if ((alu >= 0x8 && alu <= 0xB && !s) || rd == ARM_PC) {
			*read = F_ALL;
			return false;
		}
		if ((op & 0x02000010) == 0x00000010 && s && _armIsLogical(alu)) {
			/* C stays as it is for a shift of 0 */
			*written = F_NZC;
			*read |= F_C;
			return true;
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
	if (_armMultipleNative(op)) {
		return true;
	}
	*read = F_ALL;
	return false;
}

static bool _armIsMemory(uint32_t op) {
	return (op & 0x0C000000) == 0x04000000 || (op & 0x0E000000) == 0x08000000 ||
	       ((op & 0x0E000090) == 0x00000090 && (op & 0x60));
}

static void _armTranslate(struct JITEmitter* e, uint32_t op, unsigned flags) {
	if ((op & 0x0E000000) == 0x08000000) {
		_armMultiple(e, op, e->index + 1);
	} else if ((op & 0x0FC000F0) == 0x00000090) {
		_armMultiply(e, op, flags);
	} else if ((op & 0x0F8000F0) == 0x00800090) {
		_armMultiplyLong(e, op, flags);
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
static void _armConditional(struct JITEmitter* e, uint32_t op, bool native, unsigned flags) {
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
		int32_t pending = e->pending;
		bool takenIfT = _condition(e, cond);
		uint8_t* skip = e->cg.ptr;
		if (takenIfT) {
			sh4_emit_bt(&e->cg, 0);
		} else {
			sh4_emit_bf(&e->cg, 0);
		}
		_fallThrough(e, e->address + WORD_SIZE_ARM, executed);
		_patchBranch(skip, e->cg.ptr);
		e->pending = pending;
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
	e->pending = 1 + e->seq32;
	_exitAnywhere(e, executed);
}

/* ---------------------------------------------------------------- */
/* Flags across blocks                                               */
/* ---------------------------------------------------------------- */

/* How far a block looks past its end for what reads its flags: guest
 * instructions along each path, and branches followed. */
#define JIT_SCAN_BUDGET 24
#define JIT_SCAN_DEPTH 3

/* Code a scan may look at: ROM and the BIOS, which don't change, and RAM
 * only within [lo, hi), the chunks of the block it is for. [min, max) is
 * what it looked at there, which the block's SMC range then covers. */
struct JITScan {
	struct ARMJIT* jit;
	uint32_t lo;
	uint32_t hi;
	uint32_t min;
	uint32_t max;
};

static bool _isRAM(uint32_t address) {
	return (address >> BASE_OFFSET) == REGION_WORKING_RAM || (address >> BASE_OFFSET) == REGION_WORKING_IRAM;
}

static bool _scanFetch(struct JITScan* s, uint32_t address, bool thumb, uint32_t* op) {
	uint32_t len = thumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM;
	if (_isRAM(address)) {
		if (address < s->lo || address + len > s->hi) {
			return false;
		}
		if (address < s->min) {
			s->min = address;
		}
		if (address + len > s->max) {
			s->max = address + len;
		}
	}
	uint32_t bytes;
	const uint8_t* p = ARMJITSource(s->jit, address, &bytes);
	if (!p || bytes < len) {
		return false;
	}
	*op = _read(p, thumb);
	return true;
}

/* The bx lr of mGBA's HLE BIOS StallCall (subs r11, #4; bhi StallCall;
 * bx lr): only swiBase calls it, and puts the cpsr back after, so nothing
 * reads the flags it leaves. */
static bool _hleStallReturn(struct JITScan* s, uint32_t pc, uint32_t op) {
	uint32_t sub, loop;
	return pc < SIZE_BIOS && op == 0xE12FFF1E && _scanFetch(s, pc - 8, false, &sub) && sub == 0xE25BB004 &&
	       _scanFetch(s, pc - 4, false, &loop) && loop == 0x8AFFFFFD;
}

/* The flags code from pc on may read before it writes them, on any path;
 * where a path can't be followed (an indirect branch, a handler, out of
 * budget or depth) every flag not yet written counts. */
static unsigned _flagsIn(struct JITScan* s, uint32_t pc, bool thumb, int budget, int depth) {
	unsigned live = 0;
	/* not yet read or written along this path */
	unsigned open = F_ALL;
	uint32_t lr = 0;
	bool prefix = false;
	while (open) {
		uint32_t op;
		if (--budget < 0 || !_scanFetch(s, pc, thumb, &op)) {
			return live | open;
		}
		unsigned written, read;
		uint32_t next = pc + (thumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM);
		uint32_t target;
		unsigned cond = 0xE;
		if (thumb) {
			bool afterPrefix = prefix;
			prefix = false;
			if ((op & 0xF800) == 0xE000) { // B
				target = pc + 4 + ((int32_t) ((op & 0x07FF) << 21) >> 20);
			} else if ((op & 0xF000) == 0xD000 && ((op >> 8) & 0xF) < 0xE) { // Bcc
				cond = (op >> 8) & 0xF;
				target = pc + 4 + (int8_t) op * 2;
			} else if ((op & 0xF800) == 0xF000) { // BL prefix
				lr = pc + 4 + ((int32_t) ((op & 0x07FF) << 21) >> 9);
				prefix = true;
				pc = next;
				continue;
			} else if ((op & 0xF800) == 0xF800 && afterPrefix) { // BL
				target = lr + ((op & 0x07FF) << 1);
			} else if (_thumbEndsBlock(op)) {
				return live | open;
			} else {
				_thumbAnalyze(op, &written, &read);
				live |= read & open;
				open &= ~(read | written);
				pc = next;
				continue;
			}
		} else {
			cond = op >> 28;
			if ((op & 0x0E000000) == 0x0A000000 && cond != 0xF) { // B, BL
				target = pc + 8 + ((int32_t) (op << 8) >> 6);
			} else if ((op & 0x0DB0F000) == 0x0120F000 && ((op & 0x02000000) || !(op & 0x00000FF0)) &&
			           cond == 0xE) { // MSR
				if (!(op & 0x00400000) && (op & 0x00080000)) {
					/* the cpsr's flags, from the operand */
					return live;
				}
				pc = next;
				continue;
			} else if (_armEndsBlock(op)) {
				/* the BIOS's exception returns put the spsr in the cpsr */
				if (pc < SIZE_BIOS && (op == 0xE1B0F00E || op == 0xE25EF004)) {
					return live;
				}
				return _hleStallReturn(s, pc, op) ? live : live | open;
			} else {
				_armAnalyze(op, false, &written, &read);
				live |= read & open;
				open &= ~read;
				if (cond == 0xE) {
					open &= ~written;
				}
				pc = next;
				continue;
			}
		}
		/* a branch to target if cond holds */
		unsigned r = _condFlags(cond) & open;
		live |= r;
		open &= ~r;
		if (!open) {
			break;
		}
		if (!depth) {
			return live | open;
		}
		unsigned after = _flagsIn(s, target, thumb, budget, depth - 1);
		if (cond != 0xE) {
			after |= _flagsIn(s, next, thumb, budget, depth - 1);
		}
		return live | (after & open);
	}
	return live;
}

/* For jittest: the flags code at pc may read, as a block ending anywhere
 * before it would have worked it out (or fewer). */
unsigned ARMJITFlagsIn(struct ARMJIT* jit, uint32_t pc, bool thumb) {
	struct JITScan scan = { jit, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0 };
	return _flagsIn(&scan, pc, thumb, JIT_SCAN_BUDGET + JIT_MAX_BLOCK_INSNS, JIT_SCAN_DEPTH);
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
	static uint32_t longLo, longHi;
	if (!longHi && getenv("JIT_LONG")) {
		sscanf(getenv("JIT_LONG"), "%x-%x", &longLo, &longHi);
	}
	if (maxInsns > 0 && avail > (uint32_t) maxInsns && !(pc >= longLo && pc < longHi)) {
		avail = maxInsns;
	}
#endif

	struct GBA* gba = (struct GBA*) jit->cpu->master;
	bool stall = (pc >> BASE_OFFSET) >= REGION_CART0 && gba->memory.prefetch;

	/* Pass 1: extent, and which flags anything reads after each
	 * instruction. */
	uint32_t ops[JIT_MAX_BLOCK_INSNS];
	bool native[JIT_MAX_BLOCK_INSNS];
	unsigned written[JIT_MAX_BLOCK_INSNS];
	unsigned read[JIT_MAX_BLOCK_INSNS];
	unsigned flagsLive[JIT_MAX_BLOCK_INSNS];
	uint32_t n;
	for (n = 0; n < avail; ++n) {
		ops[n] = _read(&src[n * len], thumb);
		if (thumb) {
			native[n] = _thumbAnalyze(ops[n], &written[n], &read[n]);
		} else {
			native[n] = _armAnalyze(ops[n], stall, &written[n], &read[n]);
		}
		if (thumb ? _thumbEndsBlock(ops[n]) : _armEndsBlock(ops[n])) {
			++n;
			break;
		}
	}
	/* Only the stall model reads cpu->jitBase: C gets the block from the
	 * return address (_emitGuestBase). */
	bool usesBase = false;
	int i;
	for (i = 0; stall && i < (int) n; ++i) {
		uint32_t op = ops[i];
		if (native[i] && (thumb ? (op >> 11) >= 0x09 && (op >> 11) <= 0x19 : _armIsMemory(op))) {
			usesBase = true;
		}
		if (thumb ? (op & 0xFFC0) == 0x4340 : (op & 0x0F0000F0) == 0x00000090) { // a multiply's wait
			usesBase = true;
		}
	}
	/* What the code after the block reads, from the branch that ends it
	 * (or the next instruction) on; then back through the block. A
	 * conditional ARM instruction may not write its flags, so they still
	 * count as needed from before it. */
	uint32_t end = pc + n * len;
	struct JITScan scan = { jit, 0, 0, 0xFFFFFFFF, 0 };
	if (_isRAM(pc)) {
		scan.lo = pc & ~((1 << JIT_CHUNK_SHIFT) - 1);
		scan.hi = ((end - 1) | ((1 << JIT_CHUNK_SHIFT) - 1)) + 1;
	}
	int last = n;
	if (thumb ? _thumbEndsBlock(ops[n - 1]) : _armEndsBlock(ops[n - 1])) {
		last = n - 1;
		if (thumb && last > 0 && (ops[last] & 0xF800) == 0xF800 && (ops[last - 1] & 0xF800) == 0xF000) {
			--last;
		}
	}
	unsigned needed = _flagsIn(&scan, pc + last * len, thumb, JIT_SCAN_BUDGET, JIT_SCAN_DEPTH);
	for (i = n - 1; i >= last; --i) {
		flagsLive[i] = F_ALL;
	}
	for (i = last - 1; i >= 0; --i) {
		flagsLive[i] = needed;
		unsigned kill = thumb || (ops[i] >> 28) == 0xE ? written[i] : 0;
		needed = (needed & ~kill) | read[i];
	}
	/* A compare whose flags only the next instruction's condition reads
	 * (a branch ending the block: nothing after it either way) */
	int fuse[JIT_MAX_BLOCK_INSNS];
	for (i = 0; i < (int) n; ++i) {
		fuse[i] = -1;
		if (i + 1 >= (int) n || !native[i] || (!thumb && (ops[i] >> 28) != 0xE)) {
			continue;
		}
		uint32_t next = ops[i + 1];
		uint32_t at = pc + (i + 1) * len;
		int cond;
		uint32_t target;
		if (thumb) {
			if ((next & 0xF000) != 0xD000 || ((next >> 8) & 0xF) >= 0xE) {
				continue;
			}
			cond = (next >> 8) & 0xF;
			if ((written[i] & _condFlags(cond)) != _condFlags(cond)) {
				continue;
			}
			target = at + 4 + (int8_t) next * 2;
		} else {
			cond = next >> 28;
			if (cond >= 0xE || (written[i] & _condFlags(cond)) != _condFlags(cond)) {
				continue;
			}
			if ((next & 0x0E000000) == 0x0A000000) {
				target = at + 8 + ((int32_t) (next << 8) >> 6);
			} else {
				if (!native[i + 1] || i + 1 >= last || flagsLive[i + 1] || flagsLive[i] != _condFlags(cond)) {
					continue;
				}
				fuse[i] = cond;
				continue;
			}
		}
		if (_flagsIn(&scan, target, thumb, JIT_SCAN_BUDGET, JIT_SCAN_DEPTH) ||
		    _flagsIn(&scan, at + len, thumb, JIT_SCAN_BUDGET, JIT_SCAN_DEPTH)) {
			continue;
		}
		fuse[i] = cond;
	}

	struct JITEmitter e;
	memset(&e, 0, sizeof(e));
	e.fuseCond = -1;
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
	_label = entry;
	sh4_emit_cmppz(&e.cg, R_CYCLES);
	sh4_emit_bt(&e.cg, sh4_branch_disp8((uintptr_t) e.cg.ptr, (uintptr_t) start));
	if (usesBase) {
		_imm(&e, pc + 2 * len, 0);
		sh4_emit_mov_l_store_gbr(&e.cg, JIT_GBR_BASE);
	}

	bool ended = false;
	e.executed = n;
	e.body = e.cg.ptr;
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
			if ((op & 0xFF87) == 0x4700) {
				_thumbBX(&e, op, n);
				ended = true;
				break;
			}
			if ((op & 0xFD87) == 0x4487) {
				_thumbHighPC(&e, op, n);
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
			if ((op & 0x0FFFFFF0) == 0x012FFF10 && (op >> 28) == 0xE) {
				_armBX(&e, op, n);
				ended = true;
				break;
			}
		}
		e.fuseCond = fuse[i];
		if (!thumb) {
			_armConditional(&e, op, native[i], flagsLive[i]);
		} else if (native[i]) {
			if (!_thumbMultiple(&e, op, i + 1)) {
				_thumbTranslate(&e, op, flagsLive[i]);
			}
		} else {
			_handler(&e, op);
		}
		e.fuseCond = -1;
		if (e.fused && fuse[i] < 0) {
			abort();
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
	block->end = end;
	block->lo = scan.min < pc ? scan.min : pc;
	block->hi = scan.max > end ? scan.max : end;
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
