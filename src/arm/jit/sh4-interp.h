/*
 * sh4_interp.h — a small SH-4 interpreter, enough to execute what pollen
 * emits.
 *
 * This exists to be an oracle, not an emulator.  It implements exactly the
 * instructions the emit pass produces, and faults loudly on anything else, so
 * that "pollen emitted something I do not model" is a test failure rather than
 * a silently wrong answer.  Every instruction here is written from the Renesas
 * manual independently of the encoder it is checking — the point is two
 * readings of the ISA, not one reading used twice.
 *
 * Memory is a flat little-endian window supplied by the caller, plus GBR,
 * which points into it.  There is no MMU, no cache, no FPU and no exceptions.
 */

#ifndef SH4_INTERP_H
#define SH4_INTERP_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

struct sh4_state {
	uint32_t r[16];
	uint32_t gbr, pr, macl, mach;
	uint32_t pc;
	unsigned t;
	unsigned q, m;			/* the division step's own flags   */

	/* THE FLOAT SIDE.  Single precision only: the emitter never sets
	 * FPSCR.PR, so there is no double path to model and nothing here
	 * rounds twice.
	 *
	 * Two banks, because `frchg` is how the matrix reaches `ftrv`: the
	 * instruction names XMTRX, which is always the bank that is NOT
	 * current, so the emitter loads the matrix with the banks swapped and
	 * swaps back.  Modelling that as one array and an index is the whole
	 * of it — `fr[n]` is `bank[cur][n]` and `XF[n]` is `bank[cur^1][n]`. */
	float    bank[2][16];
	unsigned fbank;			/* FPSCR.FR — which bank is FR0-15 */
	uint32_t fpul;			/* the integer/float crossing point */
	uint32_t fpscr;

	/* Calls nest: a block ends at its own rts, not at a service routine's.
	 * Without this the first helper that returned would look like the
	 * block finishing, and everything after it would silently not run. */
	int depth;

	/* And the run ends when control returns HERE, whatever the nesting.
	 * Depth alone is not enough: a routine entered with jsr is allowed to
	 * abandon its return and jump elsewhere - the block entry hook does
	 * exactly that when it takes an interrupt - which leaves the count one
	 * too high forever after. Returning to the sentinel is unambiguous. */
	uint32_t ret_at;

	uint8_t *mem;			/* base of the flat window        */
	uint32_t mem_at, mem_size;

	/* A second window, for the stack.  It exists so the first can be the
	 * real guest state block rather than a copy of it - leaf routines
	 * called out of generated code write that block directly, and a copy
	 * would silently discard everything they did. */
	uint8_t *mem2;
	uint32_t mem2_at, mem2_size;

	/* A third, for guest RAM, which generated code reaches directly rather
	 * than through an accessor once the address passes the guard.  Writes
	 * below `mem3_ro` are dropped, which is how the caller excludes stores
	 * that land on the block being executed. */
	uint8_t *mem3;
	uint32_t mem3_at, mem3_size, mem3_ro;

	/* A fourth, for the block table a linked exit reads.  Separate from RAM
	 * so that an index computed wrongly lands outside every window and
	 * faults, rather than quietly reading guest memory. */
	uint8_t *mem4;
	uint32_t mem4_at, mem4_size;

	/* A fifth, for the retirement counters a profile build's block prologue
	 * charges.  They are ordinary host variables, so this window is mapped
	 * at their real address - which is what makes the emitted probe run
	 * here rather than fault on an address nothing knows about. */
	uint8_t *mem5;
	uint32_t mem5_at, mem5_size;

	/* A sixth, for the clock hook's countdown when a build shares one hook
	 * call between several blocks.  Same reason as the fifth: an ordinary
	 * host variable that emitted code reaches by its real address. */
	uint8_t *mem6;
	uint32_t mem6_at, mem6_size;

	/* mem7 — the instrument windows.  The censuses the emitter writes to
	 * are ordinary host arrays outside every guest window, so a harness
	 * that executes a profiled build has to map them or the first counted
	 * event is an out-of-range store. */
	uint8_t *mem7;
	uint32_t mem7_at, mem7_size;

	/* mem8 — a second instrument window, for the trampoline's live-
	 * register dump (`emit_tramp_live`).  Same reason as mem7: a host
	 * array the stub reaches by its real address. */
	uint8_t *mem8;
	uint32_t mem8_at, mem8_size;

	/*
	 * The compile trampoline, which every table slot holds until a block is
	 * compiled for it.  On the target it is emitted SH-4 that calls the
	 * compiler and jumps to the result; here there is no compiler, so a
	 * jump to it is modelled as what its not-compiled path does - work out
	 * the guest PC from the slot address left in r2, publish it to the
	 * state block, and return the way an ordinary exit would.
	 */
	uint32_t tramp_at, tramp_table_at, tramp_pc_disp;
	/* Which bits of a guest address pick its slot; the link masks with
	 * this, so it is what decides whether a published PC belongs to the
	 * slot the trampoline was entered through. */
	uint32_t tramp_slot_mask;

	/* Accesses that reached RAM without an accessor.  Zero here would mean
	 * every load and store took the slow path and the fast one is untested,
	 * which "all pass" would not otherwise tell you. */
	uint32_t mem3_hits;

	uint32_t code_at;		/* address the code is mapped at  */
	const uint8_t *code;
	uint32_t code_size;

	/* Calls out of the block land here.  The emitter calls the core's
	 * memory accessors, which cannot be executed as SH-4, so they are
	 * recognised by address and run natively. */
	struct {
		uint32_t addr;
		void *fn;
		int two_args;		/* store, rather than load         */
		int ret_t;		/* answer comes back in T, not r0  */
	} helper[32];
	int n_helpers;

	/* The emulator's two hooks.  A harness runs one block against fixed
	 * buffers; the emulator's SH-4 address space is guest RAM and its
	 * mirrors, the scratchpad, the I/O page, the BIOS, the state block,
	 * the block table, a stack and the code arena -- more regions than
	 * there are windows here, all known only at run time.  `xlat` answers
	 * the same question `sh4_host` does and is asked first, so returning
	 * NULL leaves the tools' behaviour unchanged.  `call_out` is how a
	 * shim is performed natively instead of executed, and `on_store` /
	 * `on_load` are the differential trace's eyes on emitted memory
	 * traffic. */
	uint8_t *(*xlat)(struct sh4_state *s, uint32_t a, uint32_t n);
	int (*call_out)(struct sh4_state *s, uint32_t tgt);
	void (*on_store)(struct sh4_state *s, uint32_t a, uint32_t v,
			 uint32_t n);
	void (*on_load)(struct sh4_state *s, uint32_t a, uint32_t v,
			uint32_t n);
	/* Two more eyes, for the cache model (fgl_cache.c): every fetch, and
	 * every PC-relative literal read -- which is an operand-cache access
	 * on the machine even though it comes out of the code window here. */
	void (*on_fetch)(struct sh4_state *s, uint32_t pc);
	void (*on_literal)(struct sh4_state *s, uint32_t a, uint32_t n);
	void *user;

	int stopped;			/* rts executed                   */
	int in_slot;			/* executing a delay slot         */
	int fault;			/* unmodelled instruction, etc.   */
	char fault_msg[128];
	uint32_t steps;
};

static inline void sh4_fault(struct sh4_state *s, const char *what, uint16_t insn)
{
	if (!s->fault) {
		s->fault = 1;
		snprintf(s->fault_msg, sizeof(s->fault_msg),
			 "%s (insn %04x at pc %08x)", what, insn, s->pc);
	}
}

/* An out-of-range access, with the address that was out of range.  Without it
 * every such fault reads the same and the only way to find out which window
 * was missing is to add this and run again. */
static inline void sh4_fault_at(struct sh4_state *s, const char *what,
				uint32_t addr)
{
	if (!s->fault) {
		s->fault = 1;
		snprintf(s->fault_msg, sizeof(s->fault_msg),
			 "%s: %08x (at pc %08x)", what, addr, s->pc);
	}
}

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

/*
 * The trampoline's block header.
 *
 * On the target every block, the trampoline included, is preceded by a
 * twelve-byte header, and an indirect link reads the `guest_pc` in it before
 * jumping - so the harness has to have one too or every link faults on a read
 * just below the trampoline.  0xffffffff is what the real one holds: an
 * address no guest target can equal, so the check always fails here and the
 * block takes its ordinary exit.
 */
static uint32_t sh4_tramp_hdr[3] = { 0xffffffffu, 0, 0 };

/* Resolve an address to whichever window holds it, or NULL. */
/* IN-RANGE WITHOUT `a + n`, WHICH WRAPS.
 *
 * `a >= at && a + n <= at + size` is the obvious test and it is wrong at the
 * top of the address space: at `a = fffffffc, n = 4` the sum is 0, so the
 * comparison passes against ANY window and the caller is handed
 * `base + fffffffc`.  With a null window (`mem` unset, size 0) that is a
 * segfault; with a real one it is a wild read.
 *
 * Found by `tools/superopt`, whose search runs sequences nobody wrote --
 * `exts.b r13,r0` turns the address mask into fffffffc and the next
 * instruction dereferences it.  Generated code never does this, which is
 * exactly why it survived: the harness only ever ran code the emitter
 * produced.  Subtraction cannot wrap once `a >= at` holds. */
static inline int sh4_in(uint32_t a, uint32_t n, uint32_t at, uint32_t size)
{
	return a >= at && n <= size && a - at <= size - n;
}

static inline uint8_t *sh4_host(struct sh4_state *s, uint32_t a, uint32_t n)
{
	if (s->xlat) {
		uint8_t *p = s->xlat(s, a, n);

		if (p)
			return p;
	}

	if (s->tramp_at && a >= s->tramp_at - sizeof(sh4_tramp_hdr) &&
	    a + n <= s->tramp_at)
		return (uint8_t *)sh4_tramp_hdr +
		       (a - (s->tramp_at - sizeof(sh4_tramp_hdr)));

	if (s->mem && sh4_in(a, n, s->mem_at, s->mem_size))
		return s->mem + (a - s->mem_at);
	if (s->mem2 && sh4_in(a, n, s->mem2_at, s->mem2_size))
		return s->mem2 + (a - s->mem2_at);
	if (s->mem3 && sh4_in(a, n, s->mem3_at, s->mem3_size)) {
		s->mem3_hits++;
		return s->mem3 + (a - s->mem3_at);
	}
	if (s->mem4 && sh4_in(a, n, s->mem4_at, s->mem4_size))
		return s->mem4 + (a - s->mem4_at);
	if (s->mem5 && sh4_in(a, n, s->mem5_at, s->mem5_size))
		return s->mem5 + (a - s->mem5_at);
	if (s->mem6 && sh4_in(a, n, s->mem6_at, s->mem6_size))
		return s->mem6 + (a - s->mem6_at);
	if (s->mem7 && sh4_in(a, n, s->mem7_at, s->mem7_size))
		return s->mem7 + (a - s->mem7_at);
	if (s->mem8 && sh4_in(a, n, s->mem8_at, s->mem8_size))
		return s->mem8 + (a - s->mem8_at);
	/*
	 * The code window, readable as data.  On the target a block and its
	 * twelve-byte header are ordinary RAM, and an indirect link reads the
	 * header of the block it is about to jump into - so refusing data reads
	 * here models a separation the machine does not have.
	 */
	if (s->code && sh4_in(a, n, s->code_at, s->code_size))
		return (uint8_t *)s->code + (a - s->code_at);
	return NULL;
}

/* A store the window refuses without that being an error. */
static inline int sh4_write_dropped(struct sh4_state *s, uint32_t a)
{
	return s->mem3 && a >= s->mem3_at && a < s->mem3_at + s->mem3_ro;
}

static inline uint32_t sh4_rd(struct sh4_state *s, uint32_t a, uint32_t n)
{
	uint8_t *p = sh4_host(s, a, n);
	uint32_t v = 0;

	if (!p) { sh4_fault_at(s, "load out of range", a); return 0; }
	memcpy(&v, p, n);
	if (s->on_load)
		s->on_load(s, a, v, n);
	return v;
}

static inline void sh4_wr(struct sh4_state *s, uint32_t a, uint32_t v,
			  uint32_t n)
{
	uint8_t *p;

	if (sh4_write_dropped(s, a)) {
		s->mem3_hits++;
		return;
	}
	p = sh4_host(s, a, n);
	if (!p) { sh4_fault_at(s, "store out of range", a); return; }
	if (s->on_store)
		s->on_store(s, a, v, n);
	memcpy(p, &v, n);
}

static inline uint32_t sh4_rd32(struct sh4_state *s, uint32_t a)
{ return sh4_rd(s, a, 4); }

static inline void sh4_wr32(struct sh4_state *s, uint32_t a, uint32_t v)
{ sh4_wr(s, a, v, 4); }

/* Instruction fetch comes from the code window, which is separate so that a
 * runaway PC is caught rather than executing data. */
static inline uint16_t sh4_fetch(struct sh4_state *s, uint32_t pc)
{
	uint32_t off = pc - s->code_at;

	if (pc < s->code_at || off + 2 > s->code_size) {
		sh4_fault(s, "pc left the block", 0);
		return 0x0009;
	}
	return (uint16_t)(s->code[off] | (s->code[off + 1] << 8));
}

/* PC-relative literals read out of the code window too. */
static inline uint32_t sh4_rd32_code(struct sh4_state *s, uint32_t a)
{
	uint32_t off = a - s->code_at;
	uint32_t v;

	if (a < s->code_at || off + 4 > s->code_size) {
		sh4_fault(s, "literal outside the block", 0);
		return 0;
	}
	memcpy(&v, s->code + off, 4);
	if (s->on_literal)
		s->on_literal(s, a, 4);
	return v;
}

/* ------------------------------------------------------------------ */

/* FR and XF, as the manual names them.  Everything float goes through these
 * two so the bank rule is stated once. */
#define FR(n)  (s->bank[s->fbank][(n)])
#define XF(n)  (s->bank[s->fbank ^ 1u][(n)])
/* FPSCR.SZ: the register field names a pair.  Bit 0 of the field picks the
 * back bank (XDn) instead of DRn; the pair is the even register and the one
 * above it, and memory holds them in address order - the even one first
 * (`sh_insns.cpp` FMOV_LOAD_XD / FMOV_STORE_XD, and what bloom's
 * `gte_mtx_load` relies on). */
#define SZ_ (s->fpscr & 0x00100000u)
#define PAIR_(f, i) (s->bank[s->fbank ^ ((f) & 1u)][((f) & 0xeu) + (i)])

/* The bit pattern of a single, and back — `fmov` moves bits and must not
 * round, and `lds Rm,FPUL` hands an integer across untouched. */
static inline uint32_t sh4_f2b(float f)
{
	uint32_t b;

	memcpy(&b, &f, 4);
	return b;
}

static inline float sh4_b2f(uint32_t b)
{
	float f;

	memcpy(&f, &b, 4);
	return f;
}

#define N_ ((insn >> 8) & 0xf)
#define M_ ((insn >> 4) & 0xf)
#define D4 (insn & 0xf)
#define D8 (insn & 0xff)
#define I8 ((int8_t)(insn & 0xff))

/*
 * Execute one instruction.  Delay slots are handled by executing the slot
 * instruction before the transfer takes effect, which is what the hardware
 * does and what the emitter's `rts; nop` pairs rely on.
 */
/*
 * A DELAY SLOT MAY NOT HOLD A PC-RELATIVE INSTRUCTION, AND THE ASSEMBLER
 * WILL NOT TELL YOU.
 *
 * `mov.w 84f, r0` (i.e. `mov.w @(disp,PC),Rn`), `mov.l @(disp,PC),Rn` and
 * `mova` read the PC, which in a slot is the branch target's, not the
 * instruction's; SH-4 hardware raises a slot illegal instruction exception
 * instead of guessing.  So does a branch in a slot.  This interpreter would
 * happily execute all of them and the fault would only appear on the DC --
 * it did, in the RTPS/RTPT saturation stubs (2026-09-10), as a crash at the
 * first clamp.  Fault here instead, where it costs one run to find.
 */
static inline int sh4_slot_illegal(uint16_t insn)
{
	switch (insn >> 12) {
	case 0x8:				/* bt/bf/bt.s/bf.s, mov.w @(d,PC) */
		return ((insn >> 8) & 0xf) >= 0x8;
	case 0x9:				/* mov.w @(disp,PC),Rn */
	case 0xa:				/* bra */
	case 0xb:				/* bsr */
	case 0xc:				/* trapa and mova (0xc7) */
		return (insn >> 12) != 0xc || ((insn >> 8) & 0xf) == 0x7;
	case 0xd:				/* mov.l @(disp,PC),Rn */
		return 1;
	case 0x0:
		switch (insn & 0xff) {
		case 0x0b:			/* rts */
		case 0x23:			/* braf */
		case 0x03:			/* bsrf */
			return 1;
		}
		return 0;
	case 0x4:
		switch (insn & 0xff) {
		case 0x2b:			/* jmp */
		case 0x0b:			/* jsr */
			return 1;
		}
		return 0;
	}
	return 0;
}

static inline void sh4_step(struct sh4_state *s)
{
	uint16_t insn = sh4_fetch(s, s->pc);
	uint32_t next = s->pc + 2;

	if (s->fault)
		return;
	if (s->in_slot && sh4_slot_illegal(insn)) {
		sh4_fault(s, "slot illegal instruction", insn);
		return;
	}
	s->steps++;
	if (s->on_fetch)
		s->on_fetch(s, s->pc);

	switch (insn >> 12) {
	case 0x0:
		switch (insn & 0xff) {
		case 0x0b:					/* rts */
			{
				uint32_t tgt = s->pr;
				s->pc = next;
				s->in_slot = 1;
				sh4_step(s);			/* delay slot */
				s->in_slot = 0;
				if (s->fault) return;
				s->pc = tgt;
				if (s->ret_at && tgt == s->ret_at)
					s->stopped = 1;
				else if (s->depth)
					s->depth--;
				else
					s->stopped = 1;
				return;
			}
		case 0x09: goto done;				/* nop */
		case 0x19: s->m = s->q = s->t = 0; goto done;	/* div0u */
		case 0x08: s->t = 0; goto done;			/* clrt */
		case 0x18: s->t = 1; goto done;			/* sett */
		case 0x12: s->r[N_] = s->gbr; goto done;	/* stc gbr,Rn */
		case 0x2a: s->r[N_] = s->pr; goto done;		/* sts pr,Rn */
		case 0x0a: s->r[N_] = s->mach; goto done;
		case 0x1a: s->r[N_] = s->macl; goto done;
		case 0x29: s->r[N_] = s->t; goto done;		/* movt */
		case 0x5a: s->r[N_] = s->fpul; goto done;	/* sts fpul,Rn */
		case 0x6a: s->r[N_] = s->fpscr; goto done;	/* sts fpscr,Rn */
		case 0xb3: goto done;		/* ocbwb - no cache here */
		}
		if ((insn & 0xf) == 0x7) {			/* mul.l */
			s->macl = s->r[N_] * s->r[M_];
			goto done;
		}
		/* Indexed loads: the block table is read this way, because a
		 * guest address masked to its slot's offset is the index. */
		switch (insn & 0xf) {
		case 0xc: s->r[N_] = (uint32_t)(int8_t)
				sh4_rd(s, s->r[0] + s->r[M_], 1);
			  goto done;			/* mov.b @(R0,Rm),Rn */
		case 0xd: s->r[N_] = (uint32_t)(int16_t)
				sh4_rd(s, s->r[0] + s->r[M_], 2);
			  goto done;			/* mov.w */
		case 0xe: s->r[N_] = sh4_rd(s, s->r[0] + s->r[M_], 4);
			  goto done;			/* mov.l */

		/* And the stores that pair with them — how the kernel writes a
		 * table entry it has just indexed. */
		case 0x4: sh4_wr(s, s->r[0] + s->r[N_], s->r[M_], 1);
			  goto done;			/* mov.b Rm,@(R0,Rn) */
		case 0x5: sh4_wr(s, s->r[0] + s->r[N_], s->r[M_], 2);
			  goto done;			/* mov.w */
		case 0x6: sh4_wr32(s, s->r[0] + s->r[N_], s->r[M_]);
			  goto done;			/* mov.l */
		case 0xf: {				/* mac.l @Rm+,@Rn+ */
			/* SR.S is never set by anything here, so no
			 * saturation: a plain 64-bit accumulate into
			 * MACH:MACL (`sh_insns.cpp` MACL, S == 0). */
			int32_t a = (int32_t)sh4_rd32(s, s->r[N_]);
			int32_t b;
			uint64_t acc;
			s->r[N_] += 4;
			b = (int32_t)sh4_rd32(s, s->r[M_]);
			s->r[M_] += 4;
			acc = ((uint64_t)s->mach << 32) | s->macl;
			acc += (uint64_t)((int64_t)a * b);
			s->macl = (uint32_t)acc;
			s->mach = (uint32_t)(acc >> 32);
			goto done;
		}
		}
		sh4_fault(s, "unmodelled 0x0", insn);
		return;

	case 0x1:						/* mov.l Rm,@(d,Rn) */
		sh4_wr32(s, s->r[N_] + D4 * 4, s->r[M_]);
		goto done;

	case 0x2:
		switch (insn & 0xf) {
		case 0x0: sh4_wr(s, s->r[N_], s->r[M_], 1); goto done;	/* mov.b Rm,@Rn */
		case 0x1: sh4_wr(s, s->r[N_], s->r[M_], 2); goto done;	/* mov.w */
		case 0x2: sh4_wr(s, s->r[N_], s->r[M_], 4); goto done;	/* mov.l */
		case 0x6:					/* mov.l Rm,@-Rn */
			s->r[N_] -= 4;
			sh4_wr32(s, s->r[N_], s->r[M_]);
			goto done;
		case 0x8: s->t = (s->r[N_] & s->r[M_]) == 0; goto done;	/* tst */
		case 0x9: s->r[N_] &= s->r[M_]; goto done;		/* and */
		case 0xa: s->r[N_] ^= s->r[M_]; goto done;		/* xor */
		case 0xb: s->r[N_] |= s->r[M_]; goto done;		/* or  */
		}
		sh4_fault(s, "unmodelled 0x2", insn);
		return;

	case 0x3:
		switch (insn & 0xf) {
		case 0x0: s->t = s->r[N_] == s->r[M_]; goto done;	/* cmp/eq */
		case 0x2: s->t = s->r[N_] >= s->r[M_]; goto done;	/* cmp/hs */
		case 0x3: s->t = (int32_t)s->r[N_] >= (int32_t)s->r[M_]; goto done;
		case 0x6: s->t = s->r[N_] > s->r[M_]; goto done;	/* cmp/hi */
		case 0x7: s->t = (int32_t)s->r[N_] > (int32_t)s->r[M_]; goto done;
		case 0x4: {						/* div1 */
			uint32_t tmp0, tmp2 = s->r[M_];
			unsigned old_q = s->q, tmp1;

			s->q = (s->r[N_] & 0x80000000u) != 0;
			s->r[N_] = (s->r[N_] << 1) | (uint32_t)s->t;

			if (!old_q) {
				if (!s->m) {
					tmp0 = s->r[N_];
					s->r[N_] -= tmp2;
					tmp1 = s->r[N_] > tmp0;
					s->q = s->q ? !tmp1 : tmp1;
				} else {
					tmp0 = s->r[N_];
					s->r[N_] += tmp2;
					tmp1 = s->r[N_] < tmp0;
					s->q = s->q ? tmp1 : !tmp1;
				}
			} else {
				if (!s->m) {
					tmp0 = s->r[N_];
					s->r[N_] += tmp2;
					tmp1 = s->r[N_] < tmp0;
					s->q = s->q ? !tmp1 : tmp1;
				} else {
					tmp0 = s->r[N_];
					s->r[N_] -= tmp2;
					tmp1 = s->r[N_] > tmp0;
					s->q = s->q ? tmp1 : !tmp1;
				}
			}
			s->t = (s->q == s->m);
			goto done;
		}
		case 0xa: {						/* subc */
			uint32_t a = s->r[N_], b = s->r[M_], t = s->t;
			uint32_t r = a - b - t;
			s->t = (a < b) || (a - b < t);
			s->r[N_] = r;
			goto done;
		}
		case 0x8: s->r[N_] -= s->r[M_]; goto done;		/* sub */
		case 0xf: {						/* addv */
			int32_t a = (int32_t)s->r[N_], b = (int32_t)s->r[M_];
			int32_t r = (int32_t)((uint32_t)a + (uint32_t)b);
			s->t = (~(a ^ b) & (a ^ r)) < 0;
			s->r[N_] = (uint32_t)r;
			goto done;
		}
		case 0xb: {						/* subv */
			int32_t a = (int32_t)s->r[N_], b = (int32_t)s->r[M_];
			int32_t r = (int32_t)((uint32_t)a - (uint32_t)b);
			s->t = ((a ^ b) & (a ^ r)) < 0;
			s->r[N_] = (uint32_t)r;
			goto done;
		}
		case 0xc: s->r[N_] += s->r[M_]; goto done;		/* add */
		case 0xe: {						/* addc */
			uint32_t a = s->r[N_], b = s->r[M_], t = s->t;
			uint32_t r = a + b + t;
			s->t = (r < a) || (r == a && t);
			s->r[N_] = r;
			goto done;
		}
		case 0xd: {						/* dmuls.l */
			int64_t p = (int64_t)(int32_t)s->r[N_] *
				    (int32_t)s->r[M_];
			s->macl = (uint32_t)p;
			s->mach = (uint32_t)((uint64_t)p >> 32);
			goto done;
		}
		case 0x5: {						/* dmulu.l */
			uint64_t p = (uint64_t)s->r[N_] * s->r[M_];
			s->macl = (uint32_t)p;
			s->mach = (uint32_t)(p >> 32);
			goto done;
		}
		}
		sh4_fault(s, "unmodelled 0x3", insn);
		return;

	case 0x4:
		switch (insn & 0xff) {
		case 0x00: s->t = s->r[N_] >> 31; s->r[N_] <<= 1; goto done;
		case 0x01: s->t = s->r[N_] & 1; s->r[N_] >>= 1; goto done;
		case 0x04: s->t = s->r[N_] >> 31;			/* rotl */
			   s->r[N_] = (s->r[N_] << 1) | s->t; goto done;
		case 0x05: s->t = s->r[N_] & 1;				/* rotr */
			   s->r[N_] = (s->r[N_] >> 1) | (s->t << 31); goto done;
		case 0x08: s->r[N_] <<= 2; goto done;
		case 0x09: s->r[N_] >>= 2; goto done;
		case 0x18: s->r[N_] <<= 8; goto done;
		case 0x19: s->r[N_] >>= 8; goto done;
		case 0x28: s->r[N_] <<= 16; goto done;
		case 0x29: s->r[N_] >>= 16; goto done;
		case 0x20: s->t = s->r[N_] >> 31; s->r[N_] <<= 1; goto done;
		case 0x21: s->t = s->r[N_] & 1;
			   s->r[N_] = (uint32_t)((int32_t)s->r[N_] >> 1);
			   goto done;
		case 0x25: {						/* rotcr */
			unsigned lsb = s->r[N_] & 1;
			s->r[N_] = (s->r[N_] >> 1) | (s->t << 31);
			s->t = lsb;
			goto done;
		}
		case 0x24: {						/* rotcl */
			unsigned msb = s->r[N_] >> 31;
			s->r[N_] = (s->r[N_] << 1) | s->t;
			s->t = msb;
			goto done;
		}
		case 0x10: s->r[N_]--; s->t = (s->r[N_] == 0); goto done;
		case 0x11: s->t = (int32_t)s->r[N_] >= 0; goto done;	/* cmp/pz */
		case 0x15: s->t = (int32_t)s->r[N_] > 0; goto done;	/* cmp/pl */
		case 0x1e: s->gbr = s->r[N_]; goto done;		/* ldc Rm,gbr */
		case 0x2a: s->pr = s->r[N_]; goto done;			/* lds Rm,pr */
		case 0x0a: s->mach = s->r[N_]; goto done;		/* lds Rm,mach */
		case 0x1a: s->macl = s->r[N_]; goto done;		/* lds Rm,macl */
		case 0x22:						/* sts.l pr,@-Rn */
			s->r[N_] -= 4;
			sh4_wr32(s, s->r[N_], s->pr);
			goto done;
		case 0x26:						/* lds.l @Rm+,pr */
			s->pr = sh4_rd32(s, s->r[N_]);
			s->r[N_] += 4;
			goto done;
		case 0x5a: s->fpul = s->r[N_]; goto done;	/* lds Rm,fpul */
		case 0x6a: s->fpscr = s->r[N_]; goto done;	/* lds Rm,fpscr */
		case 0x52:						/* sts.l fpul,@-Rn */
			s->r[N_] -= 4;
			sh4_wr32(s, s->r[N_], s->fpul);
			goto done;
		case 0x56:						/* lds.l @Rm+,fpul */
			s->fpul = sh4_rd32(s, s->r[N_]);
			s->r[N_] += 4;
			goto done;
		case 0x0b: {						/* jsr @Rm */
			uint32_t tgt = s->r[N_];
			int i;

			s->pr = next + 2;
			s->pc = next;
			s->in_slot = 1;
			sh4_step(s);			/* delay slot first */
			s->in_slot = 0;
			if (s->fault) return;

			if (s->call_out && s->call_out(s, tgt)) {
				s->pc = s->pr;
				return;
			}

			for (i = 0; i < s->n_helpers; i++) {
				if (s->helper[i].addr != tgt)
					continue;
				if (s->helper[i].ret_t) {
					int (*f)(void) = (int (*)(void))
						s->helper[i].fn;
					s->t = f() != 0;
				} else if (s->helper[i].two_args) {
					void (*f)(uint32_t, uint32_t) =
						(void (*)(uint32_t, uint32_t))
						s->helper[i].fn;
					f(s->r[4], s->r[5]);
				} else {
					uint32_t (*f)(uint32_t) =
						(uint32_t (*)(uint32_t))
						s->helper[i].fn;
					s->r[0] = f(s->r[4]);
				}
				s->pc = s->pr;	/* return past the delay slot */
				return;
			}

			s->depth++;
			s->pc = tgt;
			return;
		}
		case 0x2b: {						/* jmp @Rm */
			uint32_t tgt = s->r[N_];
			s->pc = next;
			s->in_slot = 1;
			sh4_step(s);			/* delay slot first */
			s->in_slot = 0;
			if (s->fault) return;

			if (s->tramp_at && tgt == s->tramp_at) {
				uint32_t slot = s->r[2] - s->tramp_table_at;
				uint32_t pc = 0x80000000u | slot;
				uint32_t said = sh4_rd32(s, s->gbr +
							 s->tramp_pc_disp);

				/*
				 * The linking block publishes the address it
				 * branched to. A slot index carries no segment
				 * bits, so reconstructing one always names the
				 * first mirror - take what the block said when
				 * it belongs to this slot.
				 */
				if ((said & s->tramp_slot_mask) == slot)
					pc = said;

				sh4_wr32(s, s->gbr + s->tramp_pc_disp, pc);
				s->r[0] = 0;		/* POLLEN_EXIT_NEXT */
				s->pc = s->pr;
				s->stopped = 1;
				return;
			}

			/* A TAIL CALL INTO C: `jmp` to a callee address runs
			 * it natively, as `jsr` does, and then returns to PR
			 * on the callee's behalf (gte_rtp.S reaches
			 * gte_mvmva_slow this way). */
			if (s->call_out && s->call_out(s, tgt)) {
				tgt = s->pr;
				if (s->ret_at && tgt == s->ret_at)
					s->stopped = 1;
				else if (s->depth)
					s->depth--;
				else
					s->stopped = 1;
				s->pc = tgt;
				return;
			}

			/* A TAIL JUMP TO THE RETURN ADDRESS IS A RETURN.  A
			 * kernel body that leaves through a service slot ends
			 * this way rather than with `rts`, and without this it
			 * reads as the run falling off the end. */
			if (s->ret_at && tgt == s->ret_at)
				s->stopped = 1;
			s->pc = tgt;
			return;
		}
		}
		switch (insn & 0xf) {
		case 0xc: {						/* shad */
			int32_t cnt = (int32_t)s->r[M_];
			if (cnt >= 0)
				s->r[N_] = (cnt & 31) ? s->r[N_] << (cnt & 31)
						      : s->r[N_];
			else if ((cnt & 31) == 0)
				s->r[N_] = (uint32_t)((int32_t)s->r[N_] >> 31);
			else
				s->r[N_] = (uint32_t)((int32_t)s->r[N_] >>
						      (32 - (cnt & 31)));
			goto done;
		}
		case 0xd: {						/* shld */
			int32_t cnt = (int32_t)s->r[M_];
			if (cnt >= 0)
				s->r[N_] = (cnt & 31) ? s->r[N_] << (cnt & 31)
						      : s->r[N_];
			else if ((cnt & 31) == 0)
				s->r[N_] = 0;
			else
				s->r[N_] >>= (32 - (cnt & 31));
			goto done;
		}
		}
		sh4_fault(s, "unmodelled 0x4", insn);
		return;

	case 0x5:						/* mov.l @(d,Rm),Rn */
		s->r[N_] = sh4_rd32(s, s->r[M_] + D4 * 4);
		goto done;

	case 0x6:
		switch (insn & 0xf) {
		/* The narrow loads sign-extend; the emitter relies on it. */
		case 0x0: s->r[N_] = (uint32_t)(int8_t)sh4_rd(s, s->r[M_], 1);
			  goto done;					/* mov.b @Rm,Rn */
		case 0x1: s->r[N_] = (uint32_t)(int16_t)sh4_rd(s, s->r[M_], 2);
			  goto done;					/* mov.w */
		case 0x2: s->r[N_] = sh4_rd(s, s->r[M_], 4); goto done;	/* mov.l */
		case 0x3: s->r[N_] = s->r[M_]; goto done;		/* mov */
		case 0x6:					/* mov.l @Rm+,Rn */
			s->r[N_] = sh4_rd32(s, s->r[M_]);
			s->r[M_] += 4;
			goto done;
		case 0x4:					/* mov.b @Rm+,Rn */
			s->r[N_] = (uint32_t)(int8_t)sh4_rd(s, s->r[M_], 1);
			if (N_ != M_) s->r[M_] += 1;
			goto done;
		case 0x5:					/* mov.w @Rm+,Rn */
			s->r[N_] = (uint32_t)(int16_t)sh4_rd(s, s->r[M_], 2);
			if (N_ != M_) s->r[M_] += 2;
			goto done;
		case 0x7: s->r[N_] = ~s->r[M_]; goto done;		/* not */
		case 0x8: {					/* swap.b Rm,Rn */
			uint32_t v = s->r[M_];
			s->r[N_] = (v & 0xffff0000u) | ((v & 0xff) << 8) |
				   ((v >> 8) & 0xff);
			goto done;
		}
		case 0x9:					/* swap.w Rm,Rn */
			s->r[N_] = (s->r[M_] << 16) | (s->r[M_] >> 16);
			goto done;
		case 0xa: {					/* negc Rm,Rn */
			uint32_t t = 0u - s->r[M_];
			uint32_t v = t - s->t;
			s->t = (0u < t) | (t < v);
			s->r[N_] = v;
			goto done;
		}
		case 0xb: s->r[N_] = (uint32_t)(-(int32_t)s->r[M_]); goto done;
		case 0xc: s->r[N_] = (uint8_t)s->r[M_]; goto done;	/* extu.b */
		case 0xd: s->r[N_] = (uint16_t)s->r[M_]; goto done;	/* extu.w */
		case 0xe: s->r[N_] = (uint32_t)(int8_t)s->r[M_]; goto done;
		case 0xf: s->r[N_] = (uint32_t)(int16_t)s->r[M_]; goto done;
		}
		sh4_fault(s, "unmodelled 0x6", insn);
		return;

	case 0x7:						/* add #imm,Rn */
		s->r[N_] += (uint32_t)(int32_t)I8;
		goto done;

	case 0x8:
		switch ((insn >> 8) & 0xf) {
		case 0x9:					/* bt */
			if (s->t) { s->pc = next + 2 + I8 * 2; return; }
			goto done;
		case 0xb:					/* bf */
			if (!s->t) { s->pc = next + 2 + I8 * 2; return; }
			goto done;
		case 0xd:					/* bt/s */
			if (s->t) {
				uint32_t tgt = next + 2 + I8 * 2;
				s->pc = next;
				s->in_slot = 1;
				sh4_step(s);
				s->in_slot = 0;
				if (s->fault) return;
				s->pc = tgt;
				return;
			}
			goto done;
		case 0xf:					/* bf/s */
			if (!s->t) {
				uint32_t tgt = next + 2 + I8 * 2;
				s->pc = next;
				s->in_slot = 1;
				sh4_step(s);
				s->in_slot = 0;
				if (s->fault) return;
				s->pc = tgt;
				return;
			}
			goto done;
		case 0x8: s->t = (s->r[0] == (uint32_t)(int32_t)I8); goto done;

		/* R0 against a byte or halfword displacement off a register —
		 * how the kernel's event blocks are read a field at a time. */
		case 0x0: sh4_wr(s, s->r[M_] + (insn & 0xf), s->r[0], 1);
			goto done;			/* mov.b r0,@(d,Rn) */
		case 0x1: sh4_wr(s, s->r[M_] + (insn & 0xf) * 2, s->r[0], 2);
			goto done;			/* mov.w r0,@(d,Rn) */
		case 0x4: s->r[0] = (uint32_t)(int8_t)
				sh4_rd(s, s->r[M_] + (insn & 0xf), 1);
			goto done;			/* mov.b @(d,Rm),r0 */
		case 0x5: s->r[0] = (uint32_t)(int16_t)
				sh4_rd(s, s->r[M_] + (insn & 0xf) * 2, 2);
			goto done;			/* mov.w @(d,Rm),r0 */
		}
		sh4_fault(s, "unmodelled 0x8", insn);
		return;

	case 0x9:						/* mov.w @(d,pc),Rn */
		{
			uint32_t a = s->pc + 4 + D8 * 2;
			uint32_t off = a - s->code_at;
			if (a < s->code_at || off + 2 > s->code_size) {
				sh4_fault(s, "literal outside the block", insn);
				return;
			}
			if (s->on_literal)
				s->on_literal(s, a, 2);
			s->r[N_] = (uint32_t)(int16_t)(s->code[off] |
						       (s->code[off + 1] << 8));
			goto done;
		}

	case 0xa:						/* bra */
		{
			int32_t d = (insn & 0x800) ? (int32_t)(insn | ~0xfff)
						   : (int32_t)(insn & 0xfff);
			uint32_t tgt = next + 2 + d * 2;
			s->pc = next;
			s->in_slot = 1;
			sh4_step(s);
			s->in_slot = 0;
			if (s->fault) return;
			s->pc = tgt;
			return;
		}

	case 0xb:						/* bsr */
		{
			int32_t d = (insn & 0x800) ? (int32_t)(insn | ~0xfff)
						   : (int32_t)(insn & 0xfff);
			uint32_t tgt = next + 2 + d * 2;
			s->pr = next + 2;
			s->pc = next;
			s->in_slot = 1;
			sh4_step(s);
			s->in_slot = 0;
			if (s->fault) return;
			/* COUNTED, THE SAME AS `jsr`.  `rts` stops the run at
			 * depth zero, so a call that is not counted makes the
			 * callee's return look like the outermost one and the
			 * caller never resumes — silently, with no fault. */
			s->depth++;
			s->pc = tgt;
			return;
		}

	case 0xc:
		switch ((insn >> 8) & 0xf) {
		case 0x0: /* mov.b r0,@(d,gbr) */
			sh4_wr(s, s->gbr + D8, s->r[0], 1);
			goto done;
		case 0x1: /* mov.w r0,@(d,gbr) — displacement scaled by two */
			sh4_wr(s, s->gbr + D8 * 2, s->r[0], 2);
			goto done;
		case 0x2: sh4_wr32(s, s->gbr + D8 * 4, s->r[0]); goto done;
		case 0x4: /* mov.b @(d,gbr),r0 */
			s->r[0] = (uint32_t)(int8_t)sh4_rd(s, s->gbr + D8, 1);
			goto done;
		case 0x5: /* mov.w @(d,gbr),r0 — sign extended, disp x2.
			   * The COP2 file's halfword registers are read this
			   * way and nothing else the emitter produces is, which
			   * is why this arrived with the FPU rather than with
			   * the integer core. */
			s->r[0] = (uint32_t)(int16_t)sh4_rd(s, s->gbr + D8 * 2, 2);
			goto done;
		case 0x6: s->r[0] = sh4_rd32(s, s->gbr + D8 * 4); goto done;
		case 0x7: s->r[0] = ((s->pc + 4) & ~3u) + D8 * 4; goto done;
		case 0x8: s->t = (s->r[0] & D8) == 0; goto done;	/* tst # */
		case 0x9: s->r[0] &= D8; goto done;			/* and # */
		case 0xa: s->r[0] ^= D8; goto done;			/* xor # */
		case 0xb: s->r[0] |= D8; goto done;			/* or  # */
		}
		sh4_fault(s, "unmodelled 0xc", insn);
		return;

	case 0xd:						/* mov.l @(d,pc),Rn */
		s->r[N_] = sh4_rd32_code(s, ((s->pc + 4) & ~3u) + D8 * 4);
		goto done;

	case 0xe:						/* mov #imm,Rn */
		s->r[N_] = (uint32_t)(int32_t)I8;
		goto done;

	/*
	 * THE FLOAT UNIT.  Single precision throughout — the emitter never
	 * sets FPSCR.PR or FPSCR.SZ, so a double or a paired move here would
	 * be emitted code this interpreter is right to reject.
	 *
	 * Written from the instruction set, not from the encoder it checks:
	 * the value of this file is that it is a second reading, and one that
	 * decoded what our own emitter happened to produce would agree with it
	 * for the wrong reason.
	 */
	case 0xf:
		switch (insn & 0xf) {
		case 0x0: FR(N_) += FR(M_); goto done;		/* fadd  */
		case 0x1: FR(N_) -= FR(M_); goto done;		/* fsub  */
		case 0x2: FR(N_) *= FR(M_); goto done;		/* fmul  */
		case 0x3: FR(N_) /= FR(M_); goto done;		/* fdiv  */
		case 0x4: s->t = FR(N_) == FR(M_); goto done;	/* fcmp/eq */
		case 0x5: s->t = FR(N_) >  FR(M_); goto done;	/* fcmp/gt */

		case 0x6:					/* fmov.s @(R0,Rm),FRn */
			if (SZ_) {
				PAIR_(N_, 0) = sh4_b2f(sh4_rd32(s, s->r[0] + s->r[M_]));
				PAIR_(N_, 1) = sh4_b2f(sh4_rd32(s, s->r[0] + s->r[M_] + 4));
				goto done;
			}
			FR(N_) = sh4_b2f(sh4_rd32(s, s->r[0] + s->r[M_]));
			goto done;
		case 0x7:					/* fmov.s FRm,@(R0,Rn) */
			if (SZ_) {
				sh4_wr32(s, s->r[0] + s->r[N_], sh4_f2b(PAIR_(M_, 0)));
				sh4_wr32(s, s->r[0] + s->r[N_] + 4, sh4_f2b(PAIR_(M_, 1)));
				goto done;
			}
			sh4_wr32(s, s->r[0] + s->r[N_], sh4_f2b(FR(M_)));
			goto done;
		case 0x8:					/* fmov.s @Rm,FRn */
			if (SZ_) {
				PAIR_(N_, 0) = sh4_b2f(sh4_rd32(s, s->r[M_]));
				PAIR_(N_, 1) = sh4_b2f(sh4_rd32(s, s->r[M_] + 4));
				goto done;
			}
			FR(N_) = sh4_b2f(sh4_rd32(s, s->r[M_]));
			goto done;
		case 0x9:					/* fmov.s @Rm+,FRn */
			if (SZ_) {
				PAIR_(N_, 0) = sh4_b2f(sh4_rd32(s, s->r[M_]));
				PAIR_(N_, 1) = sh4_b2f(sh4_rd32(s, s->r[M_] + 4));
				s->r[M_] += 8;
				goto done;
			}
			FR(N_) = sh4_b2f(sh4_rd32(s, s->r[M_]));
			s->r[M_] += 4;
			goto done;
		case 0xa:					/* fmov.s FRm,@Rn */
			if (SZ_) {
				sh4_wr32(s, s->r[N_], sh4_f2b(PAIR_(M_, 0)));
				sh4_wr32(s, s->r[N_] + 4, sh4_f2b(PAIR_(M_, 1)));
				goto done;
			}
			sh4_wr32(s, s->r[N_], sh4_f2b(FR(M_)));
			goto done;
		case 0xb:					/* fmov.s FRm,@-Rn */
			if (SZ_) {
				s->r[N_] -= 8;
				sh4_wr32(s, s->r[N_], sh4_f2b(PAIR_(M_, 0)));
				sh4_wr32(s, s->r[N_] + 4, sh4_f2b(PAIR_(M_, 1)));
				goto done;
			}
			s->r[N_] -= 4;
			sh4_wr32(s, s->r[N_], sh4_f2b(FR(M_)));
			goto done;
		case 0xc:					/* fmov FRm,FRn */
			if (SZ_) {
				PAIR_(N_, 0) = PAIR_(M_, 0);
				PAIR_(N_, 1) = PAIR_(M_, 1);
				goto done;
			}
			FR(N_) = FR(M_);
			goto done;

		case 0xe:					/* fmac */
			/* FUSED: `sh_insns.cpp` normal_fmac forms the exact
			 * product in extended precision and rounds once after
			 * the add.  Two roundings here would differ from the
			 * machine in the last bit, which `ftrc` then makes a
			 * whole unit of MAC. */
			FR(N_) = fmaf(FR(0), FR(M_), FR(N_));
			goto done;

		case 0xd:
			switch (insn & 0xff) {
			case 0x0d: FR(N_) = sh4_b2f(s->fpul); goto done;
			case 0x1d: s->fpul = sh4_f2b(FR(N_)); goto done;
			case 0x2d:				/* float FPUL,FRn */
				FR(N_) = (float)(int32_t)s->fpul;
				goto done;
			case 0x3d:				/* ftrc FRm,FPUL */
				/* TOWARD ZERO, and saturating rather than
				 * wrapping.
				 *
				 * NOT FROM THE MANUAL, unlike every encoding
				 * here: `reference/sh4.pdf` is the hardware
				 * manual and carries the instruction tables but
				 * not the per-instruction operation. The
				 * behaviour below is taken from a second
				 * implementation instead —
				 * `docs/flycast_docs/sh4_fpu.cpp` — which gives
				 * NaN -> 0x80000000 and clamps positive at
				 * 0x7FFFFF80, the largest value a single can
				 * represent below the bound rather than the
				 * bound itself.
				 *
				 * The negative side is explicit here where that
				 * one leans on the host's own conversion and
				 * patches up afterwards. None of it is reached
				 * by anything this project measures: the GTE
				 * works around 10^4. */
				{
					float v = FR(N_);

					if (!(v == v))
						s->fpul = 0x80000000u;
					else if (v >= 2147483520.0f)
						s->fpul = 0x7fffffffu;
					else if (v <= -2147483648.0f)
						s->fpul = 0x80000000u;
					else
						s->fpul = (uint32_t)(int32_t)v;
				}
				goto done;
			case 0x4d: FR(N_) = -FR(N_); goto done;	/* fneg */
			case 0x5d:				/* fabs */
				FR(N_) = FR(N_) < 0.0f ? -FR(N_) : FR(N_);
				goto done;
			case 0x8d: FR(N_) = 0.0f; goto done;	/* fldi0 */
			case 0x9d: FR(N_) = 1.0f; goto done;	/* fldi1 */
			case 0xfd:
				if ((insn & 0x0f00) == 0x0b00) {
					s->fbank ^= 1u;		/* frchg */
					goto done;
				}
				if ((insn & 0x0f00) == 0x0300) {
					/* fschg — the size bit; the moves above
					 * honour it (bloom's GTE loads XMTRX
					 * as eight pairs). */
					s->fpscr ^= 0x00100000u;
					goto done;
				}
				if ((insn & 0x03ff) == 0x01fd) {
					/* ftrv XMTRX,FVn.  XMTRX is COLUMN
					 * major: element (row i, column j) is
					 * XF[j*4+i], which is why the emitter's
					 * matrix goes in transposed-looking and
					 * is nonetheless right.
					 *
					 * CONFIRMED TWICE, because everything
					 * about the transform turns on it and a
					 * transposed reading here would certify
					 * a broken emitter: `reference/sh4.pdf`
					 * prints the matrix as XF0 XF4 XF8 XF12
					 * across the top row, and
					 * `docs/flycast_docs/sh4_fpu.cpp` sums
					 * xf[0],xf[4],xf[8],xf[12] for the first
					 * output.
					 *
					 * The real unit is APPROXIMATE — the
					 * manual says so and sets the inexact
					 * flag unconditionally — so neither this
					 * nor Flycast, which accumulates in
					 * double, reproduces it exactly. Left
					 * exact for the same reason as `fipr`
					 * below. */
					unsigned n = insn & 0x0c00;
					float v[4], o[4];
					int i, j;

					n >>= 8;
					for (i = 0; i < 4; i++)
						v[i] = FR(n + (unsigned)i);
					/* ACCUMULATED IN DOUBLE, ROUNDED ONCE,
					 * which is Flycast's model and closer
					 * to the unit's own (28-bit products,
					 * aligned and summed, one final
					 * rounding - `sh_insns.cpp` FTRV) than
					 * four single-precision roundings in
					 * sequence would be.  The host stand-in
					 * bloom's self-test compares the GTE
					 * assembly against (gte_fpu.c
					 * `gte_xform`, GTE_ASM) sums the same
					 * way, so a disagreement there is real. */
					for (i = 0; i < 4; i++) {
						double d = 0.0;
						for (j = 0; j < 4; j++)
							d += (double)XF((unsigned)
								(j * 4 + i)) *
								(double)v[j];
						o[i] = (float)d;
					}
					for (i = 0; i < 4; i++)
						FR(n + (unsigned)i) = o[i];
					goto done;
				}
				break;
			case 0xed: {				/* fipr FVm,FVn */
				unsigned n = (insn >> 8) & 0xc;
				unsigned m = (insn >> 6) & 0xc;
				float d = 0.0f;
				int i;

				/* THE REAL ONE IS NOT THIS ACCURATE.  `fipr`
				 * carries about 21 bits, not 24, so a sum that
				 * cancels can come out with the wrong sign on
				 * the machine and the right one here.  Left
				 * exact deliberately: an oracle that reproduced
				 * the imprecision could not tell a real
				 * disagreement from that one. */
				{
					double dd = 0.0;
					for (i = 0; i < 4; i++)
						dd += (double)FR(n + (unsigned)i) *
						      (double)FR(m + (unsigned)i);
					d = (float)dd;	/* as `ftrv` above */
				}
				FR(n + 3u) = d;
				goto done;
			}
			}
			break;
		}
		sh4_fault(s, "unmodelled 0xf", insn);
		return;
	}

	sh4_fault(s, "unmodelled", insn);
	return;

done:
	s->pc = next;
}

#undef N_
#undef M_
#undef SZ_
#undef PAIR_
#undef D4
#undef D8
#undef I8

/* Run until rts, fault, or the step budget runs out. */
static inline void sh4_run(struct sh4_state *s, uint32_t budget)
{
	s->steps = 0;
	while (!s->stopped && !s->fault) {
		if (s->steps >= budget) {
			sh4_fault(s, "step budget exhausted", 0);
			return;
		}
		sh4_step(s);
	}
}

#endif /* SH4_INTERP_H */
