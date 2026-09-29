/* SH-4 instruction encoder.
 *
 * One function per instruction form, each emitting a single 16-bit word
 * little-endian.  No state beyond the output cursor: the emitter above this
 * decides what to emit, this decides only how it is spelled.
 *
 * Verified against sh-elf-as byte for byte, plus a disassembler round-trip for
 * the PC-relative and raw-displacement forms the assembler will not accept.
 * Taken from rearmed_bloom (docs/sh4jit/sh4.h), provenance tags removed.
 *
 * Register arguments are 0-15.  Displacement arguments are already scaled the
 * way the encoding wants them: @(disp,Rn) longword forms take disp/4, word
 * forms disp/2.  The *_fits helpers test a byte offset before scaling.
 */

#ifndef SH4_H
#define SH4_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
        uint8_t *ptr;
        uint8_t *end;
        int      overflow;
} sh4_codegen;

/* Every encoder funnels through here.  A full buffer sets `overflow` and
 * drops the word: the caller checks once at the end of a block rather than
 * after every instruction. */
static inline void sh4_word(sh4_codegen *cg, uint16_t w)
{
        if (cg->ptr + 2 > cg->end) {
                cg->overflow = 1;
                return;
        }
        cg->ptr[0] = (uint8_t)(w & 0xff);
        cg->ptr[1] = (uint8_t)(w >> 8);
        cg->ptr += 2;
}

/* Operand field placement.  n is bits 11:8, m is bits 7:4.  Getting these the
 * wrong way round is the classic silent bug, which is why the test uses
 * distinct rm/rn throughout. */
#define SH4_N(op, n)       ((uint16_t)((op) | (((n) & 15) << 8)))
#define SH4_NM(op, n, m)   ((uint16_t)((op) | (((n) & 15) << 8) | (((m) & 15) << 4)))
#define SH4_ND4(op, n, d)  ((uint16_t)((op) | (((n) & 15) << 4) | ((d) & 15)))
#define SH4_NMD(op, n, m, d) ((uint16_t)((op) | (((n) & 15) << 8) | (((m) & 15) << 4) | ((d) & 15)))
#define SH4_D8(op, d)      ((uint16_t)((op) | ((d) & 0xff)))
#define SH4_ND8(op, n, d)  ((uint16_t)((op) | (((n) & 15) << 8) | ((d) & 0xff)))
#define SH4_D12(op, d)     ((uint16_t)((op) | ((d) & 0xfff)))

/* ---------------------------------------------------------------- */
/* No operands                                                       */
/* ---------------------------------------------------------------- */

static inline void sh4_emit_nop(sh4_codegen *c)    { sh4_word(c, 0x0009); }
static inline void sh4_emit_rts(sh4_codegen *c)    { sh4_word(c, 0x000b); }
static inline void sh4_emit_sett(sh4_codegen *c)   { sh4_word(c, 0x0018); }
static inline void sh4_emit_clrt(sh4_codegen *c)   { sh4_word(c, 0x0008); }
static inline void sh4_emit_clrmac(sh4_codegen *c) { sh4_word(c, 0x0028); }
static inline void sh4_emit_div0u(sh4_codegen *c)  { sh4_word(c, 0x0019); }

/* ---------------------------------------------------------------- */
/* Single register                                                   */
/* ---------------------------------------------------------------- */

static inline void sh4_emit_movt(sh4_codegen *c, int n)   { sh4_word(c, SH4_N(0x0029, n)); }
static inline void sh4_emit_cmppz(sh4_codegen *c, int n)  { sh4_word(c, SH4_N(0x4011, n)); }
static inline void sh4_emit_cmppl(sh4_codegen *c, int n)  { sh4_word(c, SH4_N(0x4015, n)); }
static inline void sh4_emit_dt(sh4_codegen *c, int n)     { sh4_word(c, SH4_N(0x4010, n)); }

static inline void sh4_emit_shll(sh4_codegen *c, int n)   { sh4_word(c, SH4_N(0x4000, n)); }
static inline void sh4_emit_shlr(sh4_codegen *c, int n)   { sh4_word(c, SH4_N(0x4001, n)); }
static inline void sh4_emit_shal(sh4_codegen *c, int n)   { sh4_word(c, SH4_N(0x4020, n)); }
static inline void sh4_emit_shar(sh4_codegen *c, int n)   { sh4_word(c, SH4_N(0x4021, n)); }
static inline void sh4_emit_rotl(sh4_codegen *c, int n)   { sh4_word(c, SH4_N(0x4004, n)); }
static inline void sh4_emit_rotr(sh4_codegen *c, int n)   { sh4_word(c, SH4_N(0x4005, n)); }
static inline void sh4_emit_rotcl(sh4_codegen *c, int n)  { sh4_word(c, SH4_N(0x4024, n)); }
static inline void sh4_emit_rotcr(sh4_codegen *c, int n)  { sh4_word(c, SH4_N(0x4025, n)); }
static inline void sh4_emit_shll2(sh4_codegen *c, int n)  { sh4_word(c, SH4_N(0x4008, n)); }
static inline void sh4_emit_shlr2(sh4_codegen *c, int n)  { sh4_word(c, SH4_N(0x4009, n)); }
static inline void sh4_emit_shll8(sh4_codegen *c, int n)  { sh4_word(c, SH4_N(0x4018, n)); }
static inline void sh4_emit_shlr8(sh4_codegen *c, int n)  { sh4_word(c, SH4_N(0x4019, n)); }
static inline void sh4_emit_shll16(sh4_codegen *c, int n) { sh4_word(c, SH4_N(0x4028, n)); }
static inline void sh4_emit_shlr16(sh4_codegen *c, int n) { sh4_word(c, SH4_N(0x4029, n)); }

/* Control transfer through a register.  Note these take the register in bits
 * 11:8 like an n-form even though the operand is a source. */
static inline void sh4_emit_jmp(sh4_codegen *c, int m)   { sh4_word(c, SH4_N(0x402b, m)); }
static inline void sh4_emit_jsr(sh4_codegen *c, int m)   { sh4_word(c, SH4_N(0x400b, m)); }
static inline void sh4_emit_braf(sh4_codegen *c, int m)  { sh4_word(c, SH4_N(0x0023, m)); }
static inline void sh4_emit_bsrf(sh4_codegen *c, int m)  { sh4_word(c, SH4_N(0x0003, m)); }

/* System registers */
static inline void sh4_emit_lds_mach(sh4_codegen *c, int m)    { sh4_word(c, SH4_N(0x400a, m)); }
static inline void sh4_emit_lds_macl(sh4_codegen *c, int m)    { sh4_word(c, SH4_N(0x401a, m)); }
static inline void sh4_emit_lds_pr(sh4_codegen *c, int m)      { sh4_word(c, SH4_N(0x402a, m)); }
static inline void sh4_emit_lds_pr_inc(sh4_codegen *c, int m)  { sh4_word(c, SH4_N(0x4026, m)); }
static inline void sh4_emit_ldc_gbr(sh4_codegen *c, int m)     { sh4_word(c, SH4_N(0x401e, m)); }
static inline void sh4_emit_sts_mach(sh4_codegen *c, int n)    { sh4_word(c, SH4_N(0x000a, n)); }
static inline void sh4_emit_sts_macl(sh4_codegen *c, int n)    { sh4_word(c, SH4_N(0x001a, n)); }
static inline void sh4_emit_sts_pr(sh4_codegen *c, int n)      { sh4_word(c, SH4_N(0x002a, n)); }
static inline void sh4_emit_sts_pr_dec(sh4_codegen *c, int n)  { sh4_word(c, SH4_N(0x4022, n)); }
static inline void sh4_emit_stc_gbr(sh4_codegen *c, int n)     { sh4_word(c, SH4_N(0x0012, n)); }
static inline void sh4_emit_stc_sr(sh4_codegen *c, int n)      { sh4_word(c, SH4_N(0x0002, n)); }
static inline void sh4_emit_ldc_sr(sh4_codegen *c, int m)      { sh4_word(c, SH4_N(0x400e, m)); }

/* Write back one operand-cache block.  Mandatory over freshly emitted code:
 * it sits in the operand cache and the instruction fetcher will not see it. */
static inline void sh4_emit_ocbwb(sh4_codegen *c, int n)       { sh4_word(c, SH4_N(0x00b3, n)); }

/* ---------------------------------------------------------------- */
/* Two registers                                                     */
/* ---------------------------------------------------------------- */

static inline void sh4_emit_mov_reg(sh4_codegen *c, int m, int n) { sh4_word(c, SH4_NM(0x6003, n, m)); }

static inline void sh4_emit_add_reg(sh4_codegen *c, int m, int n) { sh4_word(c, SH4_NM(0x300c, n, m)); }
static inline void sh4_emit_addc(sh4_codegen *c, int m, int n)    { sh4_word(c, SH4_NM(0x300e, n, m)); }
static inline void sh4_emit_addv(sh4_codegen *c, int m, int n)    { sh4_word(c, SH4_NM(0x300f, n, m)); }
static inline void sh4_emit_sub(sh4_codegen *c, int m, int n)     { sh4_word(c, SH4_NM(0x3008, n, m)); }
static inline void sh4_emit_subc(sh4_codegen *c, int m, int n)    { sh4_word(c, SH4_NM(0x300a, n, m)); }
static inline void sh4_emit_subv(sh4_codegen *c, int m, int n)    { sh4_word(c, SH4_NM(0x300b, n, m)); }

static inline void sh4_emit_and(sh4_codegen *c, int m, int n)     { sh4_word(c, SH4_NM(0x2009, n, m)); }
static inline void sh4_emit_or(sh4_codegen *c, int m, int n)      { sh4_word(c, SH4_NM(0x200b, n, m)); }
static inline void sh4_emit_xor(sh4_codegen *c, int m, int n)     { sh4_word(c, SH4_NM(0x200a, n, m)); }
static inline void sh4_emit_tst(sh4_codegen *c, int m, int n)     { sh4_word(c, SH4_NM(0x2008, n, m)); }
static inline void sh4_emit_not(sh4_codegen *c, int m, int n)     { sh4_word(c, SH4_NM(0x6007, n, m)); }
static inline void sh4_emit_neg(sh4_codegen *c, int m, int n)     { sh4_word(c, SH4_NM(0x600b, n, m)); }
static inline void sh4_emit_negc(sh4_codegen *c, int m, int n)    { sh4_word(c, SH4_NM(0x600a, n, m)); }

static inline void sh4_emit_exts_b(sh4_codegen *c, int m, int n)  { sh4_word(c, SH4_NM(0x600e, n, m)); }
static inline void sh4_emit_exts_w(sh4_codegen *c, int m, int n)  { sh4_word(c, SH4_NM(0x600f, n, m)); }
static inline void sh4_emit_extu_b(sh4_codegen *c, int m, int n)  { sh4_word(c, SH4_NM(0x600c, n, m)); }
static inline void sh4_emit_extu_w(sh4_codegen *c, int m, int n)  { sh4_word(c, SH4_NM(0x600d, n, m)); }
static inline void sh4_emit_swap_b(sh4_codegen *c, int m, int n)  { sh4_word(c, SH4_NM(0x6008, n, m)); }
static inline void sh4_emit_swap_w(sh4_codegen *c, int m, int n)  { sh4_word(c, SH4_NM(0x6009, n, m)); }
static inline void sh4_emit_xtrct(sh4_codegen *c, int m, int n)   { sh4_word(c, SH4_NM(0x200d, n, m)); }

static inline void sh4_emit_cmpeq(sh4_codegen *c, int m, int n)   { sh4_word(c, SH4_NM(0x3000, n, m)); }
static inline void sh4_emit_cmphs(sh4_codegen *c, int m, int n)   { sh4_word(c, SH4_NM(0x3002, n, m)); }
static inline void sh4_emit_cmpge(sh4_codegen *c, int m, int n)   { sh4_word(c, SH4_NM(0x3003, n, m)); }
static inline void sh4_emit_cmphi(sh4_codegen *c, int m, int n)   { sh4_word(c, SH4_NM(0x3006, n, m)); }
static inline void sh4_emit_cmpgt(sh4_codegen *c, int m, int n)   { sh4_word(c, SH4_NM(0x3007, n, m)); }
static inline void sh4_emit_cmpstr(sh4_codegen *c, int m, int n)  { sh4_word(c, SH4_NM(0x200c, n, m)); }

static inline void sh4_emit_div1(sh4_codegen *c, int m, int n)    { sh4_word(c, SH4_NM(0x3004, n, m)); }
static inline void sh4_emit_div0s(sh4_codegen *c, int m, int n)   { sh4_word(c, SH4_NM(0x2007, n, m)); }
static inline void sh4_emit_dmuls_l(sh4_codegen *c, int m, int n) { sh4_word(c, SH4_NM(0x300d, n, m)); }
static inline void sh4_emit_dmulu_l(sh4_codegen *c, int m, int n) { sh4_word(c, SH4_NM(0x3005, n, m)); }
static inline void sh4_emit_mul_l(sh4_codegen *c, int m, int n)   { sh4_word(c, SH4_NM(0x0007, n, m)); }

/* Arithmetic and logical shift by a register amount.  A negative count shifts
 * the other way, which is what makes a variable MIPS shift branchless. */
static inline void sh4_emit_shad(sh4_codegen *c, int m, int n)    { sh4_word(c, SH4_NM(0x400c, n, m)); }
static inline void sh4_emit_shld(sh4_codegen *c, int m, int n)    { sh4_word(c, SH4_NM(0x400d, n, m)); }

/* ---------------------------------------------------------------- */
/* Memory — register indirect                                        */
/* ---------------------------------------------------------------- */

static inline void sh4_emit_mov_b_load(sh4_codegen *c, int m, int n)  { sh4_word(c, SH4_NM(0x6000, n, m)); }
static inline void sh4_emit_mov_w_load(sh4_codegen *c, int m, int n)  { sh4_word(c, SH4_NM(0x6001, n, m)); }
static inline void sh4_emit_mov_l_load(sh4_codegen *c, int m, int n)  { sh4_word(c, SH4_NM(0x6002, n, m)); }
static inline void sh4_emit_mov_b_store(sh4_codegen *c, int m, int n) { sh4_word(c, SH4_NM(0x2000, n, m)); }
static inline void sh4_emit_mov_w_store(sh4_codegen *c, int m, int n) { sh4_word(c, SH4_NM(0x2001, n, m)); }
static inline void sh4_emit_mov_l_store(sh4_codegen *c, int m, int n) { sh4_word(c, SH4_NM(0x2002, n, m)); }

static inline void sh4_emit_mov_l_load_inc(sh4_codegen *c, int m, int n)  { sh4_word(c, SH4_NM(0x6006, n, m)); }
static inline void sh4_emit_mov_l_store_dec(sh4_codegen *c, int m, int n) { sh4_word(c, SH4_NM(0x2006, n, m)); }

/* Indexed by R0.  This is the form the indirect-branch dispatcher uses:
 * @(R0,Rm) makes R0 the operand rather than an accident. */
static inline void sh4_emit_mov_b_load_r0(sh4_codegen *c, int m, int n)  { sh4_word(c, SH4_NM(0x000c, n, m)); }
static inline void sh4_emit_mov_w_load_r0(sh4_codegen *c, int m, int n)  { sh4_word(c, SH4_NM(0x000d, n, m)); }
static inline void sh4_emit_mov_l_load_r0(sh4_codegen *c, int m, int n)  { sh4_word(c, SH4_NM(0x000e, n, m)); }
static inline void sh4_emit_mov_b_store_r0(sh4_codegen *c, int m, int n) { sh4_word(c, SH4_NM(0x0004, n, m)); }
static inline void sh4_emit_mov_w_store_r0(sh4_codegen *c, int m, int n) { sh4_word(c, SH4_NM(0x0005, n, m)); }
static inline void sh4_emit_mov_l_store_r0(sh4_codegen *c, int m, int n) { sh4_word(c, SH4_NM(0x0006, n, m)); }

/* ---------------------------------------------------------------- */
/* Memory — displacement                                             */
/* ---------------------------------------------------------------- */

/* Longword: 4-bit displacement, scaled by 4, so 0-60 bytes.  Byte and word
 * displacement forms exist only with R0 as the other operand. */
static inline void sh4_emit_mov_l_load_disp(sh4_codegen *c, int m, int n, int d)
{ sh4_word(c, SH4_NMD(0x5000, n, m, d)); }
static inline void sh4_emit_mov_l_store_disp(sh4_codegen *c, int m, int n, int d)
{ sh4_word(c, SH4_NMD(0x1000, n, m, d)); }

static inline void sh4_emit_mov_b_load_disp_r0(sh4_codegen *c, int m, int d)
{ sh4_word(c, SH4_ND4(0x8400, m, d)); }
static inline void sh4_emit_mov_w_load_disp_r0(sh4_codegen *c, int m, int d)
{ sh4_word(c, SH4_ND4(0x8500, m, d)); }
static inline void sh4_emit_mov_b_store_disp_r0(sh4_codegen *c, int n, int d)
{ sh4_word(c, SH4_ND4(0x8000, n, d)); }
static inline void sh4_emit_mov_w_store_disp_r0(sh4_codegen *c, int n, int d)
{ sh4_word(c, SH4_ND4(0x8100, n, d)); }

/* GBR-relative: 8-bit displacement, so a longword reaches 0-1020 — seventeen
 * times what @(disp,Rn) reaches.  The destination is architecturally R0. */
static inline void sh4_emit_mov_b_load_gbr(sh4_codegen *c, int d)  { sh4_word(c, SH4_D8(0xc400, d)); }
static inline void sh4_emit_mov_w_load_gbr(sh4_codegen *c, int d)  { sh4_word(c, SH4_D8(0xc500, d)); }
static inline void sh4_emit_mov_l_load_gbr(sh4_codegen *c, int d)  { sh4_word(c, SH4_D8(0xc600, d)); }
static inline void sh4_emit_mov_b_store_gbr(sh4_codegen *c, int d) { sh4_word(c, SH4_D8(0xc000, d)); }
static inline void sh4_emit_mov_w_store_gbr(sh4_codegen *c, int d) { sh4_word(c, SH4_D8(0xc100, d)); }
static inline void sh4_emit_mov_l_store_gbr(sh4_codegen *c, int d) { sh4_word(c, SH4_D8(0xc200, d)); }

/* PC-relative literal loads.  SH-4 has no 32-bit immediate, so every constant
 * that will not fit in a signed byte comes from a pool reached by these.
 * Reach is 512 bytes for the word form and 1020 for the longword. */
static inline void sh4_emit_mov_w_load_pc(sh4_codegen *c, int d, int n) { sh4_word(c, SH4_ND8(0x9000, n, d)); }
static inline void sh4_emit_mov_l_load_pc(sh4_codegen *c, int d, int n) { sh4_word(c, SH4_ND8(0xd000, n, d)); }
static inline void sh4_emit_mova(sh4_codegen *c, int d)                 { sh4_word(c, SH4_D8(0xc700, d)); }

/* ---------------------------------------------------------------- */
/* Immediate                                                         */
/* ---------------------------------------------------------------- */

/* Signed 8-bit, sign-extended to 32.  The cheapest constant tier: no pool
 * entry at all. */
static inline void sh4_emit_mov_imm(sh4_codegen *c, int i, int n)  { sh4_word(c, SH4_ND8(0xe000, n, i)); }
static inline void sh4_emit_add_imm(sh4_codegen *c, int i, int n)  { sh4_word(c, SH4_ND8(0x7000, n, i)); }

/* Immediate logic is R0-only, like the GBR forms. */
static inline void sh4_emit_cmpeq_imm(sh4_codegen *c, int i) { sh4_word(c, SH4_D8(0x8800, i)); }
static inline void sh4_emit_and_imm(sh4_codegen *c, int i)   { sh4_word(c, SH4_D8(0xc900, i)); }
static inline void sh4_emit_or_imm(sh4_codegen *c, int i)    { sh4_word(c, SH4_D8(0xcb00, i)); }
static inline void sh4_emit_xor_imm(sh4_codegen *c, int i)   { sh4_word(c, SH4_D8(0xca00, i)); }
static inline void sh4_emit_tst_imm(sh4_codegen *c, int i)   { sh4_word(c, SH4_D8(0xc800, i)); }

/* ---------------------------------------------------------------- */
/* Branches                                                          */
/* ---------------------------------------------------------------- */

/* Displacements are in words and relative to the branch's own address plus 4,
 * because the delay slot has already been fetched.  Both slot instructions
 * execute unconditionally, including on the not-taken path of bt/s and bf/s. */
static inline int32_t sh4_branch_disp8(uintptr_t from, uintptr_t to)
{ return (int32_t)(((intptr_t)to - (intptr_t)from - 4) >> 1); }
static inline int32_t sh4_branch_disp12(uintptr_t from, uintptr_t to)
{ return (int32_t)(((intptr_t)to - (intptr_t)from - 4) >> 1); }

static inline int sh4_disp8_fits(int32_t d)  { return d >= -128 && d <= 127; }
static inline int sh4_disp12_fits(int32_t d) { return d >= -2048 && d <= 2047; }

static inline void sh4_emit_bra(sh4_codegen *c, int32_t d)  { sh4_word(c, SH4_D12(0xa000, d)); }
static inline void sh4_emit_bsr(sh4_codegen *c, int32_t d)  { sh4_word(c, SH4_D12(0xb000, d)); }
static inline void sh4_emit_bt(sh4_codegen *c, int32_t d)   { sh4_word(c, SH4_D8(0x8900, d)); }
static inline void sh4_emit_bf(sh4_codegen *c, int32_t d)   { sh4_word(c, SH4_D8(0x8b00, d)); }
static inline void sh4_emit_bt_s(sh4_codegen *c, int32_t d) { sh4_word(c, SH4_D8(0x8d00, d)); }
static inline void sh4_emit_bf_s(sh4_codegen *c, int32_t d) { sh4_word(c, SH4_D8(0x8f00, d)); }

/* ---------------------------------------------------------------- */
/* Floating point                                                    */
/* ---------------------------------------------------------------- */

/* The GTE is the only thing that uses these, and it uses them the way the
 * reference does: the rotation matrix goes into the BACK bank so that `ftrv`
 * can multiply by it as XMTRX, and `frchg` gets in and out within one command.
 * Nothing holds a float across a call.
 *
 * FPSCR is single precision, round-to-nearest, set once — so there is no
 * `fschg` here and no double-precision form of anything. */
static inline void sh4_emit_frchg(sh4_codegen *c) { sh4_word(c, 0xfbfd); }
static inline void sh4_emit_fschg(sh4_codegen *c) { sh4_word(c, 0xf3fd); }

static inline void sh4_emit_fldi0(sh4_codegen *c, int n) { sh4_word(c, SH4_N(0xf08d, n)); }
static inline void sh4_emit_fldi1(sh4_codegen *c, int n) { sh4_word(c, SH4_N(0xf09d, n)); }

static inline void sh4_emit_fmov(sh4_codegen *c, int m, int n)       { sh4_word(c, SH4_NM(0xf00c, n, m)); }
static inline void sh4_emit_fmov_load(sh4_codegen *c, int m, int n)  { sh4_word(c, SH4_NM(0xf008, n, m)); }
static inline void sh4_emit_fmov_store(sh4_codegen *c, int m, int n) { sh4_word(c, SH4_NM(0xf00a, n, m)); }
static inline void sh4_emit_fmov_load_inc(sh4_codegen *c, int m, int n)  { sh4_word(c, SH4_NM(0xf009, n, m)); }
static inline void sh4_emit_fmov_store_dec(sh4_codegen *c, int m, int n) { sh4_word(c, SH4_NM(0xf00b, n, m)); }
static inline void sh4_emit_fmov_load_r0(sh4_codegen *c, int m, int n)   { sh4_word(c, SH4_NM(0xf006, n, m)); }

static inline void sh4_emit_fadd(sh4_codegen *c, int m, int n) { sh4_word(c, SH4_NM(0xf000, n, m)); }
static inline void sh4_emit_fsub(sh4_codegen *c, int m, int n) { sh4_word(c, SH4_NM(0xf001, n, m)); }
static inline void sh4_emit_fmul(sh4_codegen *c, int m, int n) { sh4_word(c, SH4_NM(0xf002, n, m)); }
static inline void sh4_emit_fdiv(sh4_codegen *c, int m, int n) { sh4_word(c, SH4_NM(0xf003, n, m)); }
static inline void sh4_emit_fcmpeq(sh4_codegen *c, int m, int n) { sh4_word(c, SH4_NM(0xf004, n, m)); }
static inline void sh4_emit_fcmpgt(sh4_codegen *c, int m, int n) { sh4_word(c, SH4_NM(0xf005, n, m)); }
static inline void sh4_emit_fneg(sh4_codegen *c, int n) { sh4_word(c, SH4_N(0xf04d, n)); }
static inline void sh4_emit_fabs(sh4_codegen *c, int n) { sh4_word(c, SH4_N(0xf05d, n)); }

/* Integer <-> float goes through FPUL, which is also the only path between the
 * two register files: there is no direct move. */
static inline void sh4_emit_float(sh4_codegen *c, int n) { sh4_word(c, SH4_N(0xf02d, n)); }
static inline void sh4_emit_ftrc(sh4_codegen *c, int m)  { sh4_word(c, SH4_N(0xf03d, m)); }
static inline void sh4_emit_lds_fpul(sh4_codegen *c, int m)     { sh4_word(c, SH4_N(0x405a, m)); }
static inline void sh4_emit_sts_fpul(sh4_codegen *c, int n)     { sh4_word(c, SH4_N(0x005a, n)); }
static inline void sh4_emit_sts_fpul_dec(sh4_codegen *c, int n) { sh4_word(c, SH4_N(0x4052, n)); }
static inline void sh4_emit_lds_fpul_inc(sh4_codegen *c, int m) { sh4_word(c, SH4_N(0x4056, m)); }
static inline void sh4_emit_lds_fpscr(sh4_codegen *c, int m)    { sh4_word(c, SH4_N(0x406a, m)); }
static inline void sh4_emit_sts_fpscr(sh4_codegen *c, int n)    { sh4_word(c, SH4_N(0x006a, n)); }

/* The two that make the matrix product worth doing at all.  `n` is a vector
 * number 0-3, i.e. FR(4n)..FR(4n+3). */
static inline void sh4_emit_ftrv(sh4_codegen *c, int n)
{ sh4_word(c, (uint16_t)(0xf1fd | ((n & 3) << 10))); }
static inline void sh4_emit_fipr(sh4_codegen *c, int m, int n)
{ sh4_word(c, (uint16_t)(0xf0ed | ((n & 3) << 10) | ((m & 3) << 8))); }

#endif /* SH4_H */
