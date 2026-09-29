/* Recompiler driver: block table, code cache, SMC tracking, and the C side
 * of the exits.
 *
 * Generated code runs block to block by itself: direct branches jump
 * straight to their target once it has been compiled (the link site is
 * patched on first use), indirect ones go through a hash lookup, and every
 * block starts by checking whether an event is due. It comes back here only
 * for that, for a target that isn't compiled or can't be linked, or when
 * mGBA code it called has already moved the cpu on (jit->exit says which).
 * ARMJITRun deals with the exit so the cpu is left exactly between two
 * instructions, as the interpreter would leave it. */
#include "jit-private.h"
#include <stdio.h>

#include <mgba/internal/arm/isa-inlines.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/memory.h>
#include <mgba-util/math.h>

#ifdef __sh__
#include <kos/cache.h>
#include <malloc.h>
#endif

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static_assert(offsetof(struct ARMCore, gprs) == JIT_GBR_GPRS(0) * 4, "ARMCore.gprs moved");
static_assert(offsetof(struct ARMCore, cpsr) == JIT_GBR_CPSR * 4, "ARMCore.cpsr moved");
static_assert(offsetof(struct ARMCore, cycles) == JIT_GBR_CYCLES * 4, "ARMCore.cycles moved");
static_assert(offsetof(struct ARMCore, nextEvent) == JIT_GBR_NEXT_EVENT * 4, "ARMCore.nextEvent moved");
static_assert(offsetof(struct ARMCore, prefetch) == JIT_GBR_PREFETCH0 * 4, "ARMCore.prefetch moved");
static_assert(offsetof(struct ARMCore, prefetch[1]) == JIT_GBR_PREFETCH1 * 4, "ARMCore.prefetch moved");
static_assert(offsetof(struct ARMCore, cpsr) + 3 == JIT_GBR_FLAGS, "cpsr flags byte moved");
static_assert(offsetof(struct ARMCore, memory.activeSeqCycles16) == JIT_GBR_SEQ16 * 4, "ARMMemory moved");
static_assert(offsetof(struct ARMCore, memory.activeNonseqCycles16) == JIT_GBR_NONSEQ16 * 4, "ARMMemory moved");

#ifdef __sh__
#define JIT_CODE_SIZE 0x80000
#define JIT_MAX_BLOCKS 0x4000
#else
#define JIT_CODE_SIZE 0x400000
#define JIT_MAX_BLOCKS 0x10000
#endif

/* Room a single block may need; a compile never starts with less free. */
#define JIT_BLOCK_RESERVE 0x2000

static void _store32(struct ARMCore* cpu, uint32_t address, int32_t value, int* cycleCounter);
static void _store16(struct ARMCore* cpu, uint32_t address, int16_t value, int* cycleCounter);
static void _store8(struct ARMCore* cpu, uint32_t address, int8_t value, int* cycleCounter);
static uint32_t _storeMultiple(struct ARMCore* cpu, uint32_t baseAddress, int mask, enum LSMDirection direction,
                               int* cycleCounter);

bool ARMJITInit(struct ARMCore* cpu) {
	if (cpu->jit) {
		return true;
	}
	struct ARMJIT* jit = calloc(1, sizeof(*jit));
	if (!jit) {
		return false;
	}
	jit->cpu = cpu;
	jit->codeSize = JIT_CODE_SIZE;
#ifdef __sh__
	jit->code = memalign(32, jit->codeSize);
#else
	jit->code = malloc(jit->codeSize);
#endif
	jit->maxBlocks = JIT_MAX_BLOCKS;
	jit->blocks = calloc(jit->maxBlocks, sizeof(*jit->blocks));
	if (!jit->code || !jit->blocks) {
		free(jit->code);
		free(jit->blocks);
		free(jit);
		return false;
	}
#ifndef __sh__
	if (!ARMJITHostInit(jit)) {
		free(jit->code);
		free(jit->blocks);
		free(jit);
		return false;
	}
#endif
	jit->hash = calloc(JIT_HASH_SIZE, sizeof(*jit->hash));
	if (!jit->hash) {
		free(jit->code);
		free(jit->blocks);
		free(jit);
		return false;
	}
#ifndef __sh__
	struct GBA* gba = (struct GBA*) cpu->master;
	ARMJITHostMap(jit, jit->hash, JIT_HASH_SIZE * sizeof(*jit->hash));
	ARMJITHostMap(jit, &jit->exit, sizeof(jit->exit));
	ARMJITHostMap(jit, &gba->memory, sizeof(gba->memory));
	ARMJITHostMap(jit, jit->memStubs, sizeof(jit->memStubs));
	ARMJITHostMap(jit, jit->memData, sizeof(jit->memData));
	ARMJITHostMap(jit, jit->chunks, sizeof(jit->chunks));
	ARMJITHostMap(jit, gba->memory.wram, SIZE_WORKING_RAM);
	ARMJITHostMap(jit, gba->memory.iwram, SIZE_WORKING_IRAM);
	if (gba->memory.rom) {
		ARMJITHostMap(jit, gba->memory.rom, SIZE_CART0);
	}
#endif
#ifdef JIT_FASTMEM
	jit->fastmem = ARMJITFastmemInit(jit);
#endif
#ifndef __sh__
	jit->fastmem = getenv("JIT_FASTMEM") != NULL;
#endif
	ARMJITEmitStubs(jit);
	jit->codeBase = jit->codeUsed;
#ifdef JIT_FASTMEM
	ARMJITFastmemInstall(jit);
#endif
	jit->timingKey = ARMJITTimingKey(cpu);
	ARMJITUpdateMemory(jit);

	jit->store32 = cpu->memory.store32;
	jit->store16 = cpu->memory.store16;
	jit->store8 = cpu->memory.store8;
	jit->storeMultiple = cpu->memory.storeMultiple;
	cpu->memory.store32 = _store32;
	cpu->memory.store16 = _store16;
	cpu->memory.store8 = _store8;
	cpu->memory.storeMultiple = _storeMultiple;

	cpu->jit = jit;
	return true;
}

void ARMJITDeinit(struct ARMCore* cpu) {
	struct ARMJIT* jit = cpu->jit;
	if (!jit) {
		return;
	}
	cpu->memory.store32 = jit->store32;
	cpu->memory.store16 = jit->store16;
	cpu->memory.store8 = jit->store8;
#ifdef JIT_FASTMEM
	ARMJITFastmemDeinit(jit);
#endif
	cpu->memory.storeMultiple = jit->storeMultiple;
	int i, j;
	for (i = 0; i < 16; ++i) {
		if (!jit->pages[i]) {
			continue;
		}
		for (j = 0; j < 0x1000; ++j) {
			free(jit->pages[i][j]);
		}
		free(jit->pages[i]);
	}
#ifndef __sh__
	ARMJITHostDeinit(jit);
#endif
	free(jit->code);
	free(jit->blocks);
	free(jit->hash);
	free(jit);
	cpu->jit = NULL;
}

void ARMJITFlush(struct ARMCore* cpu) {
	struct ARMJIT* jit = cpu->jit;
	if (!jit) {
		return;
	}
	int i, j;
	for (i = 0; i < 16; ++i) {
		if (!jit->pages[i]) {
			continue;
		}
		for (j = 0; j < 0x1000; ++j) {
			if (jit->pages[i][j]) {
				memset(jit->pages[i][j], 0, 0x800 * sizeof(struct JITBlock*));
			}
		}
	}
	memset(jit->chunks, 0, sizeof(jit->chunks));
	memset(jit->hash, 0, JIT_HASH_SIZE * sizeof(*jit->hash));
	jit->nBlocks = 0;
	jit->codeUsed = jit->codeBase;
	++jit->stats.flushes;
#ifdef JIT_FASTMEM
	ARMJITFastmemUnprotect(jit);
#endif
}

void ARMJITGetStats(struct ARMCore* cpu, struct ARMJITStats* stats) {
	struct ARMJIT* jit = cpu->jit;
	if (!jit) {
		memset(stats, 0, sizeof(*stats));
		return;
	}
	*stats = jit->stats;
	stats->codeBytes = jit->codeUsed - jit->codeBase;
}

void ARMJITTimingChanged(struct ARMJIT* jit) {
	ARMJITFlush(jit->cpu);
	jit->timingKey = ARMJITTimingKey(jit->cpu);
	ARMJITUpdateMemory(jit);
}

void ARMJITRegisterCall(struct ARMJIT* jit, const void* fn) {
#ifdef __sh__
	UNUSED(jit);
	UNUSED(fn);
#else
	ARMJITHostRegister(jit, (uint32_t) (uintptr_t) fn);
#endif
}

static struct JITBlock** _slot(struct ARMJIT* jit, uint32_t pc, bool create) {
	struct JITBlock*** page = jit->pages[pc >> 24];
	if (!page) {
		if (!create) {
			return NULL;
		}
		page = calloc(0x1000, sizeof(*page));
		if (!page) {
			return NULL;
		}
		jit->pages[pc >> 24] = page;
	}
	struct JITBlock** leaf = page[(pc >> 12) & 0xFFF];
	if (!leaf) {
		if (!create) {
			return NULL;
		}
		leaf = calloc(0x800, sizeof(*leaf));
		if (!leaf) {
			return NULL;
		}
		page[(pc >> 12) & 0xFFF] = leaf;
	}
	return &leaf[(pc & 0xFFF) >> 1];
}

/* SMC chunk for a RAM address, or -1. Mirrors fold to the same chunk. */
static int _chunk(uint32_t address) {
	switch (address >> BASE_OFFSET) {
	case REGION_WORKING_RAM:
		return (address & (SIZE_WORKING_RAM - 1)) >> JIT_CHUNK_SHIFT;
	case REGION_WORKING_IRAM:
		return JIT_EWRAM_CHUNKS + ((address & (SIZE_WORKING_IRAM - 1)) >> JIT_CHUNK_SHIFT);
	default:
		return -1;
	}
}

/* Where guest code at pc lives on the host, and how many bytes of it can be
 * read before the region ends. NULL for regions we don't compile from. */
static const uint8_t* _source(struct ARMJIT* jit, uint32_t pc, uint32_t* bytes) {
	struct GBA* gba = (struct GBA*) jit->cpu->master;
	struct GBAMemory* memory = &gba->memory;
	uint32_t offset;
	switch (pc >> BASE_OFFSET) {
	case REGION_BIOS:
		if (pc >= SIZE_BIOS) {
			return NULL;
		}
		*bytes = SIZE_BIOS - pc;
		return (const uint8_t*) memory->bios + pc;
	case REGION_WORKING_RAM:
		offset = pc & (SIZE_WORKING_RAM - 1);
		*bytes = SIZE_WORKING_RAM - offset;
		return (const uint8_t*) memory->wram + offset;
	case REGION_WORKING_IRAM:
		offset = pc & (SIZE_WORKING_IRAM - 1);
		*bytes = SIZE_WORKING_IRAM - offset;
		return (const uint8_t*) memory->iwram + offset;
	case REGION_CART0:
	case REGION_CART0_EX:
	case REGION_CART1:
	case REGION_CART1_EX:
	case REGION_CART2:
	case REGION_CART2_EX:
		offset = pc & (SIZE_CART0 - 1);
		if (!memory->rom || offset >= memory->romSize) {
			return NULL;
		}
		*bytes = memory->romSize - offset;
		return (const uint8_t*) memory->rom + offset;
	default:
		return NULL;
	}
}

static void _link(struct ARMJIT* jit, struct JITBlock* block) {
	int first = _chunk(block->pc);
	int last = _chunk(block->end - 1);
	block->chunk[0] = first;
	block->chunk[1] = last != first ? last : -1;
	int i;
	for (i = 0; i < 2; ++i) {
		if (block->chunk[i] < 0) {
			continue;
		}
		block->next[i] = jit->chunks[block->chunk[i]];
		jit->chunks[block->chunk[i]] = block;
	}
#ifdef JIT_FASTMEM
	ARMJITFastmemProtect(jit, block->pc, block->end);
#endif
}

static void _hashInsert(struct ARMJIT* jit, const struct JITBlock* block) {
	struct GBA* gba = (struct GBA*) jit->cpu->master;
	/* Entering the idle loop has to go through mGBA, which is where it's
	 * noticed; keep it out of reach of indirect branches. */
	if (gba->idleOptimization >= IDLE_LOOP_REMOVE && block->pc == gba->idleLoop) {
		return;
	}
	uint32_t key = JIT_KEY(block->pc, block->thumb);
	uint32_t* entry = jit->hash[(key >> 1) & (JIT_HASH_SIZE - 1)];
	entry[0] = key;
	entry[1] = (uint32_t) (uintptr_t) block->code;
}

static void _hashRemove(struct ARMJIT* jit, const struct JITBlock* block) {
	uint32_t key = JIT_KEY(block->pc, block->thumb);
	uint32_t* entry = jit->hash[(key >> 1) & (JIT_HASH_SIZE - 1)];
	if (entry[0] == key && entry[1] == (uint32_t) (uintptr_t) block->code) {
		entry[0] = 0;
		entry[1] = 0;
	}
}

static struct JITBlock* _compile(struct ARMJIT* jit, uint32_t pc, bool thumb, struct JITBlock** slot) {
	uint32_t bytes;
	const uint8_t* src = _source(jit, pc, &bytes);
	if (!src) {
		return NULL;
	}
	if (jit->nBlocks == jit->maxBlocks || jit->codeSize - jit->codeUsed < JIT_BLOCK_RESERVE) {
		ARMJITFlush(jit->cpu);
		slot = _slot(jit, pc, true);
		if (!slot) {
			return NULL;
		}
	}
	struct JITBlock* block = &jit->blocks[jit->nBlocks];
	memset(block, 0, sizeof(*block));
	if (!ARMJITCompile(jit, block, pc, thumb, src, bytes)) {
		return NULL;
	}
	++jit->nBlocks;
	jit->codeUsed += block->codeSize;
	struct JITBlock* old = *slot;
	if (old && old->dead && old->pc == pc && old->thumb == thumb) {
		/* Links into the old copy still arrive at its entry, which now
		 * always takes the prologue's jump: send that here. */
		*(uint32_t*) (old->code - JIT_PRE_BYTES + JIT_PRE_TARGET) = (uint32_t) (uintptr_t) block->code;
	}
	block->slot = slot;
	*slot = block;
	_link(jit, block);
	_hashInsert(jit, block);
#ifdef __sh__
	icache_sync_range((uintptr_t) block->code - JIT_PRE_BYTES, block->codeSize);
#endif
	++jit->stats.blocksCompiled;
	jit->stats.guestInsnsCompiled += block->nInsns;
	if (!thumb) {
		jit->stats.armInsnsCompiled += block->nInsns;
	}
	return block;
}

/* The block for key, compiled if need be. */
static struct JITBlock* _find(struct ARMJIT* jit, uint32_t key) {
	uint32_t pc = JIT_KEY_PC(key);
	bool thumb = JIT_KEY_THUMB(key);
	struct JITBlock** slot = _slot(jit, pc, true);
	if (!slot) {
		return NULL;
	}
	struct JITBlock* block = *slot;
	if (block && !block->dead && block->pc == pc && block->thumb == thumb) {
		return block;
	}
	return _compile(jit, pc, thumb, slot);
}

/* A dead block's entry is turned into an unconditional exit through its
 * prologue, so anything still linked to it gets redirected. */
static void _kill(struct ARMJIT* jit, struct JITBlock* block) {
	block->dead = 1;
	_hashRemove(jit, block);
	uint8_t* entry = block->code;
	entry[0] = 0x18; /* sett */
	entry[1] = 0x00;
#ifdef __sh__
	icache_sync_range((uintptr_t) entry, 2);
#endif
	++jit->stats.invalidations;
}

/* A store landed in [address, address + size): drop every block that covers
 * any of it. Blocks spanning two chunks sit on both lists; a dead one is
 * unlinked lazily when the other list is next walked. */
static void _invalidate(struct ARMJIT* jit, int chunk, uint32_t address, uint32_t size) {
	uint32_t mask = chunk < JIT_EWRAM_CHUNKS ? SIZE_WORKING_RAM - 1 : SIZE_WORKING_IRAM - 1;
	uint32_t start = address & mask;
	struct JITBlock** link = &jit->chunks[chunk];
	struct JITBlock* block;
	while ((block = *link)) {
		int i = block->chunk[0] == chunk ? 0 : 1;
		uint32_t bstart = block->pc & mask;
		uint32_t bend = bstart + (block->end - block->pc);
		if (!block->dead && start < bend && start + size > bstart) {
			_kill(jit, block);
		}
		if (block->dead) {
			*link = block->next[i];
		} else {
			link = &block->next[i];
		}
	}
}

static inline void _checkStore(struct ARMJIT* jit, uint32_t address, uint32_t size) {
	int chunk = _chunk(address);
	if (chunk >= 0 && jit->chunks[chunk]) {
		_invalidate(jit, chunk, address, size);
	}
}

static void _store32(struct ARMCore* cpu, uint32_t address, int32_t value, int* cycleCounter) {
	struct ARMJIT* jit = cpu->jit;
	jit->store32(cpu, address, value, cycleCounter);
	_checkStore(jit, address & ~3, 4);
}

static void _store16(struct ARMCore* cpu, uint32_t address, int16_t value, int* cycleCounter) {
	struct ARMJIT* jit = cpu->jit;
	jit->store16(cpu, address, value, cycleCounter);
	_checkStore(jit, address & ~1, 2);
}

static void _store8(struct ARMCore* cpu, uint32_t address, int8_t value, int* cycleCounter) {
	struct ARMJIT* jit = cpu->jit;
	jit->store8(cpu, address, value, cycleCounter);
	_checkStore(jit, address, 1);
}

static uint32_t _storeMultiple(struct ARMCore* cpu, uint32_t baseAddress, int mask, enum LSMDirection direction,
                               int* cycleCounter) {
	struct ARMJIT* jit = cpu->jit;
	uint32_t result = jit->storeMultiple(cpu, baseAddress, mask, direction, cycleCounter);
	uint32_t size = popcount32(mask & 0xFFFF) * 4;
	uint32_t start = baseAddress & ~3;
	if (direction & LSM_D) {
		start -= size;
	}
	/* Covers both the IA/IB and DA/DB spans; the extra word either side is
	 * cheaper than decoding the exact one. */
	_checkStore(jit, start, size + 4);
	if (_chunk(start) != _chunk(start + size + 3)) {
		_checkStore(jit, start + size + 3, 1);
	}
	return result;
}

static uint32_t _execute(struct ARMJIT* jit, const struct JITBlock* block) {
#ifdef __sh__
	return ((uint32_t (*)(struct ARMCore*, const void*)) jit->enter)(jit->cpu, block->code);
#else
	return ARMJITHostRun(jit, block->code);
#endif
}

/* The cpu between instructions, about to run the one at key: what
 * ThumbWritePC/ARMWritePC leave, minus their side effects. */
static void _arrive(struct ARMCore* cpu, uint32_t key) {
	uint32_t pc = JIT_KEY_PC(key);
	if (JIT_KEY_THUMB(key)) {
		LOAD_16(cpu->prefetch[0], pc & cpu->memory.activeMask, cpu->memory.activeRegion);
		pc += WORD_SIZE_THUMB;
		LOAD_16(cpu->prefetch[1], pc & cpu->memory.activeMask, cpu->memory.activeRegion);
	} else {
		LOAD_32(cpu->prefetch[0], pc & cpu->memory.activeMask, cpu->memory.activeRegion);
		pc += WORD_SIZE_ARM;
		LOAD_32(cpu->prefetch[1], pc & cpu->memory.activeMask, cpu->memory.activeRegion);
	}
	cpu->gprs[ARM_PC] = pc;
}

uint32_t ARMJITRun(struct ARMCore* cpu) {
	struct ARMJIT* jit = cpu->jit;
	if (ARMJITTimingKey(cpu) != jit->timingKey) {
		ARMJITTimingChanged(jit);
	} else {
		/* Cheats and AGB print can move the ROM. */
		ARMJITUpdateMemory(jit);
	}
	bool thumb = cpu->executionMode == MODE_THUMB;
	uint32_t pc = cpu->gprs[ARM_PC] - (thumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM);
	struct JITBlock* block = _find(jit, JIT_KEY(pc, thumb));
	/* N and Z both set: generated code can't hold that (jit-emit.c). */
	if (!block || (cpu->cpsr.packed >> 30) == 3) {
		++jit->stats.fallbackSteps;
		ARMRunInstruction(cpu);
		return 1;
	}
	++jit->stats.blockRuns;
	uint32_t executed = _execute(jit, block);
	uint32_t arg = jit->exit.arg;
	uint32_t* site;
	uint32_t flushes;
	++jit->stats.exits[jit->exit.type];
	switch (jit->exit.type) {
	case JIT_EXIT_EVENT:
		_arrive(cpu, arg);
		break;
	case JIT_EXIT_SITE_BRANCH:
	case JIT_EXIT_SITE_FALL:
		site = (uint32_t*) (uintptr_t) arg;
		if (jit->exit.type == JIT_EXIT_SITE_BRANCH) {
			/* Same region, not the idle loop: only the bookkeeping. */
			cpu->memory.setActiveRegion(cpu, JIT_KEY_PC(site[1]));
		}
		_arrive(cpu, site[1]);
		flushes = jit->stats.flushes;
		block = _find(jit, site[1]);
		if (block && jit->stats.flushes == flushes) {
			site[0] = (uint32_t) (uintptr_t) block->code;
		}
		break;
	case JIT_EXIT_BRANCH:
		cpu->gprs[ARM_PC] = JIT_KEY_PC(arg);
		if (JIT_KEY_THUMB(arg)) {
			cpu->cycles += ThumbWritePC(cpu);
		} else {
			cpu->cycles += ARMWritePC(cpu);
		}
		break;
	case JIT_EXIT_LOOKUP:
		cpu->memory.setActiveRegion(cpu, JIT_KEY_PC(arg));
		_arrive(cpu, arg);
		break;
	case JIT_EXIT_SYNC:
		break;
	}
	return executed;
}

void ARMJITRunLoop(struct ARMCore* cpu) {
	while (cpu->cycles < cpu->nextEvent) {
		ARMJITRun(cpu);
	}
	cpu->irqh.processEvents(cpu);
}
