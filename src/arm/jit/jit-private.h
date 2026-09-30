#ifndef ARM_JIT_PRIVATE_H
#define ARM_JIT_PRIVATE_H

#include <mgba/internal/arm/jit.h>
#include <mgba/internal/arm/arm.h>

#include "sh4-emit.h"

/* GBR points at the ARMCore. These are the @(disp,GBR) longword indices the
 * generated code uses; jit.c checks them against offsetof so a layout change
 * fails the build instead of the guest. */
#define JIT_GBR_GPRS(R) (R)
#define JIT_GBR_CPSR 16
#define JIT_GBR_CYCLES 18
#define JIT_GBR_NEXT_EVENT 19
#define JIT_GBR_PREFETCH0 71
#define JIT_GBR_PREFETCH1 72
/* mov.b @(disp,GBR): the byte of the cpsr holding N, Z, C, V (bits 7-4). */
#define JIT_GBR_FLAGS 67
#define JIT_GBR_SEQ16 86
#define JIT_GBR_NONSEQ16 88
/* gprs[PC] as the block's first instruction sees it: set on entry by blocks
 * that call out, read by the stubs to make an instruction's PC. */
#define JIT_GBR_BASE 104
/* cpu->jitStubs[stall][op]: the memory stubs, called through r0 */
#define JIT_GBR_STUBS 105
#define JIT_GBR_MASK 121
#define JIT_GBR_COUNT 122
#define JIT_GBR_TMP 123
#define JIT_GBR_MD(I) (124 + (I))

/* Guest code longer than this is split. Also bounds how far back an SMC
 * store has to look for a block that covers it. */
#define JIT_MAX_BLOCK_INSNS 32

/* SMC chunks: 256 bytes of EWRAM or IWRAM each. */
#define JIT_CHUNK_SHIFT 8
#define JIT_EWRAM_CHUNKS (0x40000 >> JIT_CHUNK_SHIFT)
#define JIT_IWRAM_CHUNKS (0x8000 >> JIT_CHUNK_SHIFT)
#define JIT_CHUNKS (JIT_EWRAM_CHUNKS + JIT_IWRAM_CHUNKS)

/* Indirect branch cache: key -> block entry, two-way set associative, the
 * most recently inserted in way 0. JIT_HASH_SIZE entries in all; a set is
 * jit->hash[JIT_HASH_SET(key) * 2 ...+ 1]. */
#define JIT_HASH_BITS 13
#define JIT_HASH_SIZE (2 << JIT_HASH_BITS)
#define JIT_HASH_SET(KEY) (((KEY) >> 1) & ((1 << JIT_HASH_BITS) - 1))

/* A block's key: its guest address, bit 0 set for Thumb. That is also what
 * BX takes, so an indirect branch looks its target up without converting. */
#define JIT_KEY(PC, THUMB) ((PC) | ((THUMB) ? 1 : 0))
#define JIT_KEY_PC(KEY) ((KEY) & ~1u)
#define JIT_KEY_THUMB(KEY) ((KEY) & 1)

/* Why generated code gave control back (jit->exit.type). exit.arg is a key,
 * or for the SITE kinds the address of a link site: the literal pair
 * { entry the site jumps to, key of its target }. */
enum JITExitType {
	/* An event is due at the start of block `key`. */
	JIT_EXIT_EVENT,
	/* Falling through into a block that isn't linked yet. */
	JIT_EXIT_SITE_FALL,
	/* A branch into the same region, cycles already charged, not linked
	 * yet. */
	JIT_EXIT_SITE_BRANCH,
	/* A branch that has to go through mGBA (region change, the idle loop,
	 * an indirect target that isn't cached): nothing done past computing
	 * the target. */
	JIT_EXIT_BRANCH,
	/* mGBA code already left the cpu at the next instruction. */
	JIT_EXIT_SYNC,
	/* An indirect branch within the region whose target isn't in the hash:
	 * cycles charged, bookkeeping not done. */
	JIT_EXIT_LOOKUP,
	JIT_EXIT_MAX
};

/* Per-block prologue, in front of block->code:
 *   pre+0   mov.l @(key),r0
 *   pre+2   mov.l @(target),r1
 *   pre+4   jmp @r1
 *   pre+6   nop
 *   pre+8   .long key
 *   pre+12  .long target    (the event exit; a dead block's replacement)
 *   entry   cmp/pz r13      (sett once the block is dead)
 *   entry+2 bt pre
 */
#define JIT_PRE_BYTES 16
#define JIT_PRE_KEY 8
#define JIT_PRE_TARGET 12

/* Memory accesses generated code makes through the stubs (ARMJITEmitStubs),
 * in the order of cpu->jitStubs. The stall set is for code running from ROM
 * with the prefetch buffer on (GBAMemoryStall). */
enum JITMemOp {
	JIT_MEM_LOAD32,
	JIT_MEM_LOAD16,
	JIT_MEM_LOADS16,
	JIT_MEM_LOAD8,
	JIT_MEM_LOADS8,
	JIT_MEM_STORE32,
	JIT_MEM_STORE16,
	JIT_MEM_STORE8,
	JIT_MEM_OPS
};

/* What the memory stubs' fast paths read: cpu->jitMemData[index], at
 * @(JIT_GBR_MD(index),GBR). Refreshed by ARMJITUpdateMemory. */
enum {
	JIT_MD_WRAM,
	JIT_MD_IWRAM,
	JIT_MD_ROM,
	JIT_MD_ROM_SIZE,
	JIT_MD_CHUNKS_EWRAM,
	JIT_MD_CHUNKS_IWRAM,
	JIT_MD_EWRAM_LOAD32,
	JIT_MD_EWRAM_LOAD16,
	JIT_MD_EWRAM_STORE32,
	JIT_MD_EWRAM_STORE16,
	JIT_MD_ROM_LOAD32,
	JIT_MD_ROM_LOAD16,
	/* LDM/STM in EWRAM: wait = LDM/STM + n * WORD */
	JIT_MD_EWRAM_LDM,
	JIT_MD_EWRAM_STM,
	JIT_MD_EWRAM_WORD,
	JIT_MD_MAX
};

struct JITBlock {
	uint32_t pc;
	uint32_t end;
	/* The guest code it depends on (a store there kills it): [pc, end) and
	 * what it looked at after for the flags, within its chunks. */
	uint32_t lo;
	uint32_t hi;
	uint8_t* code;
	uint32_t codeSize;
	uint16_t nInsns;
	uint8_t thumb;
	uint8_t dead;
	int16_t chunk[2];
	struct JITBlock* next[2];
	struct JITBlock** slot;
};

struct ARMJITHost;

struct JITExit {
	uint32_t arg;
	uint32_t type;
};

struct ARMJIT {
	struct ARMCore* cpu;

	uint8_t* code;
	uint32_t codeSize;
	uint32_t codeBase;
	uint32_t codeUsed;

	/* Fixed code at the start of the buffer (ARMJITEmitStubs). */
	const void* enter;
	const void* exits[JIT_EXIT_MAX];
	const void* lookup;
	const void* dispatchSync;
	/* mGBA's handler for an instruction: [thumb] */
	const void* handlers[2];
	/* GBAMemoryStall on the wait in r2 (_emitStall) */
	const void* stall;
	/* r0 = the guest base of the block that called at r1 (_emitGuestBase) */
	const void* guestBase;
	/* LDM/STM and friends: [stall][store][thumb] (_emitMultipleStub) */
	const void* multipleStubs[2][2][2];

	struct JITBlock* blocks;
	uint32_t maxBlocks;
	uint32_t nBlocks;

	/* Guest PC -> block: [addr >> 24][(addr >> 12) & 0xFFF][(addr & 0xFFF) >> 1].
	 * Both levels allocated on first compile. */
	struct JITBlock*** pages[16];

	struct JITBlock* chunks[JIT_CHUNKS];

	/* { key, entry } pairs, read by generated code. */
	uint32_t (*hash)[2];
	/* { return address, guest base } pairs for ARMJITGuestBase, by
	 * (return address >> 1) & 0xFF. */
	uint32_t baseCache[256][2];
	struct JITExit exit;

	/* Wait states the blocks were compiled with; a change flushes them. */
	uint32_t timingKey;

	void (*store32)(struct ARMCore*, uint32_t address, int32_t value, int* cycleCounter);
	void (*store16)(struct ARMCore*, uint32_t address, int16_t value, int* cycleCounter);
	void (*store8)(struct ARMCore*, uint32_t address, int8_t value, int* cycleCounter);
	uint32_t (*storeMultiple)(struct ARMCore*, uint32_t baseAddress, int mask, enum LSMDirection direction,
	                          int* cycleCounter);

	/* Memory accesses go straight to guest addresses (fastmem.c) */
	bool fastmem;

	struct ARMJITStats stats;
	struct ARMJITHost* host;
};

/* fastmem.c */
#if defined(__sh__) && defined(__DREAMCAST__) && defined(M_ARM_JIT_FASTMEM)
#define JIT_FASTMEM
bool ARMJITFastmemInit(struct ARMJIT* jit);
void ARMJITFastmemInstall(struct ARMJIT* jit);
void ARMJITFastmemDeinit(struct ARMJIT* jit);
void ARMJITFastmemUpdate(struct ARMJIT* jit);
void ARMJITFastmemProtect(struct ARMJIT* jit, uint32_t start, uint32_t end);
void ARMJITFastmemUnprotect(struct ARMJIT* jit);
void ARMJITFastmemUnprotectPage(struct ARMJIT* jit, uint32_t address);
uint32_t ARMJITFastmemFaults(void);
#endif

/* jit-emit.c */
struct JITBlock* ARMJITCompile(struct ARMJIT* jit, struct JITBlock* block, uint32_t pc, bool thumb,
                               const uint8_t* src, uint32_t srcBytes);
void ARMJITEmitStubs(struct ARMJIT* jit);
uint32_t ARMJITTimingKey(const struct ARMCore* cpu);
/* Refresh cpu->jitMemData from mGBA's memory state. */
void ARMJITUpdateMemory(struct ARMJIT* jit);
/* The flags (F_* in jit-emit.c: V, C, Z, N from bit 0) code at pc may read
 * before writing them. */
unsigned ARMJITFlagsIn(struct ARMJIT* jit, uint32_t pc, bool thumb);

/* jit.c */
/* Where guest code at pc lives on the host, and how many bytes of it can be
 * read before the region ends. NULL for regions we don't compile from. */
const uint8_t* ARMJITSource(struct ARMJIT* jit, uint32_t pc, uint32_t* bytes);
/* Every C function generated code calls goes through here, so the Linux
 * host can refuse calls to anything else. No-op on the Dreamcast. */
void ARMJITRegisterCall(struct ARMJIT* jit, const void* fn);
/* The guest base (pc + 2 * length) of the block whose code has ret in it,
 * cached. */
uint32_t ARMJITGuestBase(struct ARMJIT* jit, uint32_t ret);
/* The wait states changed under the running code: drop everything. Safe to
 * call from a helper; the caller's block must leave straight after. */
void ARMJITTimingChanged(struct ARMJIT* jit);

/* jit-host.c (Linux) */
#ifndef __sh__
bool ARMJITHostInit(struct ARMJIT* jit);
void ARMJITHostDeinit(struct ARMJIT* jit);
void ARMJITHostRegister(struct ARMJIT* jit, uint32_t fn);
/* Let generated code touch [p, p + size) by its host address. */
void ARMJITHostMap(struct ARMJIT* jit, const void* p, uint32_t size);
uint32_t ARMJITHostRun(struct ARMJIT* jit, const void* code);
#endif

#endif
