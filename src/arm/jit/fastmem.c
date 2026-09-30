/* Guest memory at its own addresses, through the SH-4's MMU.
 *
 * ROM, EWRAM and IWRAM are mapped where the GBA has them with fixed UTLB
 * entries, so generated code reaches them with one instruction. The rest
 * faults into the memory stubs. RAM pages with compiled code in them are read
 * only, which sends stores there to the stubs' check.
 *
 * The mapping needs wram on a 64 KiB and the ROM on a 1 MiB boundary
 * (anonymousMemoryMap and GBALoadROM see to that on the Dreamcast); a region
 * that isn't is left unmapped and every access to it faults.
 *
 * Optional (M_ARM_JIT_FASTMEM, on by default). It relies on the MMU raising
 * TLB miss, protection and address error exceptions. Flycast doesn't raise
 * them, so there an access outside the mapped regions reads garbage instead
 * of reaching its stub: build with it off to run there. */
#include "jit-private.h"

#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/memory.h>

#include <arch/irq.h>
#include <arch/mmu.h>
#include <kos/thread.h>
#include <stdio.h>

/* The vectors: exception entry for the direct accesses.
 * A fault at one of the six access instructions (address r4, value r5, offset
 * r6) becomes a call of the matching stub that returns after the access;
 * anything else is KOS's.
 *
 * Each vector starts with a nop and its branch has a nop in the delay slot,
 * as KOS's do. With "bra common; mov #1,r4" there the write reached the r4 of
 * the code that faulted (the address) on hardware: wrong values and hangs.
 * docs/asid/fmtest is the test for any change to this. */
__asm__(
	"\t.section .text.fastmem,\"ax\"\n"
	"\t.balign\t32\n"
	"\t.globl\t_fastmem_vbr\n"
	"\t.globl\t_fastmem_lo\n"
	"\t.globl\t_fastmem_hi\n"
	"\t.globl\t_fastmem_count\n"
	"\t.globl\t_fastmem_table\n"
	"_fastmem_vbr:\n"
	"\t.org\t0x100\n"
	"\tnop\n"
	"\tbra\tcommon\n"
	"\tnop\n"
	"\t.org\t0x400\n"
	"\tnop\n"
	"\tbra\tcommon\n"
	"\tnop\n"
	"\t.org\t0x600\n"
	"\tnop\n"
	"\tmov.l\tkos,r0\n"
	"\tjmp\t@r0\n"
	"\tmov\t#3,r4\n"
	"common:\n"
	"\tstc\tspc,r0\n"
	"\tmov.l\t_fastmem_lo,r1\n"
	"\tmov.l\t_fastmem_hi,r2\n"
	"\tcmp/hs\tr1,r0\n"
	"\tbf\tpass\n"
	"\tcmp/hs\tr2,r0\n"
	"\tbt\tpass\n"
	"\tmov\tr0,r3\n"
	"\tmov.w\t@r0,r1\n"
	"\tadd\t#2,r0\n"
	"\tlds\tr0,pr\n"
	"\tmov\tr1,r0\n"
	"\tand\t#3,r0\n"
	"\tshlr8\tr1\n"
	"\tshlr2\tr1\n"
	"\tmov\t#16,r2\n"
	"\tand\tr2,r1\n"
	"\tshll2\tr0\n"
	"\tadd\tr0,r1\n"
	"\tmova\t_fastmem_table,r0\n"
	"\tmov.l\t@(r0,r1),r1\n"
	"\tldc\tr1,spc\n"
	"\tmova\t_fastmem_dbg,r0\n"
	"\tmov.l\tr3,@r0\n"
	"\tstc\tr4_bank,r2\n"
	"\tmov.l\tr2,@(4,r0)\n"
	"\tmov.l\tr1,@(8,r0)\n"
	"\tmov.l\t_expevt,r2\n"
	"\tmov.l\t@r2,r2\n"
	"\tmov.l\tr2,@(12,r0)\n"
	"\tmova\t_fastmem_count,r0\n"
	"\tmov.l\t@r0,r1\n"
	"\tadd\t#1,r1\n"
	"\tmov.l\tr1,@r0\n"
	"\trte\n"
	"\tnop\n"
	"pass:\n"
	"\tmov.l\t_expevt,r2\n"
	"\tmov.l\t@r2,r2\n"
	"\tmov.w\ttrapa,r1\n"
	"\tcmp/eq\tr1,r2\n"
	"\tbt\ttokos\n"
	"\tmova\t_fastmem_crash,r0\n"
	"\tmov.l\tr2,@(4,r0)\n"
	"\tstc\tspc,r1\n"
	"\tmov.l\tr1,@r0\n"
	"\tmov.l\ttea,r1\n"
	"\tmov.l\t@r1,r1\n"
	"\tmov.l\tr1,@(8,r0)\n"
	"\tsts\tpr,r1\n"
	"\tmov.l\tr1,@(12,r0)\n"
	"\tstc\tssr,r1\n"
	"\tmov.l\tr1,@(16,r0)\n"
	"\tmov.l\tr15,@(20,r0)\n"
	"\tstc\tr4_bank,r1\n"
	"\tmov.l\tr1,@(24,r0)\n"
	"\tstc\tr0_bank,r1\n"
	"\tmov.l\tr1,@(28,r0)\n"
	"\tmov.l\tcrashfn,r1\n"
	"\tldc\tr1,spc\n"
	"\tmov.l\tcrashsr,r1\n"
	"\tldc\tr1,ssr\n"
	"\trte\n"
	"\tnop\n"
	"tokos:\n"
	"\tmov.l\tkos,r0\n"
	"\tjmp\t@r0\n"
	"\tmov\t#1,r4\n"
	"trapa:\n"
	"\t.word\t0x160\n"
	"\t.balign\t4\n"
	"tea:\n"
	"\t.long\t0xFF00000C\n"
	"crashfn:\n"
	"\t.long\t_ARMJITFastmemCrash\n"
	"crashsr:\n"
	"\t.long\t0x40000000\n"
	"\t.globl\t_fastmem_crash\n"
	"_fastmem_crash:\n"
	"\t.long\t0, 0, 0, 0, 0, 0, 0, 0\n"
	"\t.balign\t4\n"
	"kos:\n"
	"\t.long\t_irq_save_regs\n"
	"_fastmem_lo:\n"
	"\t.long\t0\n"
	"_fastmem_hi:\n"
	"\t.long\t0\n"
	"_fastmem_count:\n"
	"\t.long\t0\n"
	"_expevt:\n"
	"\t.long\t0xFF000024\n"
	"\t.globl\t_fastmem_dbg\n"
	"_fastmem_dbg:\n"
	"\t.long\t0, 0, 0, 0\n"
	"_fastmem_table:\n"
	"\t.long\t0, 0, 0, 0, 0, 0, 0, 0\n"
	".text\n");

extern uint8_t fastmem_vbr[];
extern uint32_t fastmem_lo;
extern uint32_t fastmem_hi;
extern uint32_t fastmem_count;
extern uint32_t fastmem_table[8];

#define UTLB_ADDR(E) (*(volatile uint32_t*) (0xF6000000 | ((E) << 8)))
#define UTLB_DATA(E) (*(volatile uint32_t*) (0xF7000000 | ((E) << 8)))

#define PTE_V 0x100
#define PTE_RW 0x020
#define PTE_C 0x008
#define PTE_D 0x004
#define PTE_SH 0x002
#define SZ_1K 0x00
#define SZ_64K 0x80
#define SZ_1M 0x90

/* 62 and 63 are KOS's, for the store queues */
#define FIRST_ENTRY 61
#define IWRAM_PAGES (SIZE_WORKING_IRAM >> 10)
#define EWRAM_PAGES (SIZE_WORKING_RAM >> 16)
#define MAX_ROM_PAGES (FIRST_ENTRY + 1 - IWRAM_PAGES - EWRAM_PAGES)

static struct {
	bool on;
	uint32_t vbr;
	const void* wram;
	const void* rom;
	uint32_t romSize;
	bool ram;
	int next;
} fm;

static void _entry(int entry, uint32_t virt, uint32_t phys, uint32_t size) {
	UTLB_DATA(entry) = (phys & 0x1FFFFC00) | PTE_V | size | PTE_RW | PTE_C | PTE_D | PTE_SH;
	UTLB_ADDR(entry) = (virt & 0xFFFFFC00) | 0x200 | PTE_V;
}

static void _map(struct ARMJIT* jit) {
	struct GBA* gba = (struct GBA*) jit->cpu->master;
	struct GBAMemory* memory = &gba->memory;
	int old = irq_disable();
	int i;
	for (i = 0; i <= FIRST_ENTRY; ++i) {
		UTLB_ADDR(i) = 0;
		UTLB_DATA(i) = 0;
	}
	fm.wram = memory->wram;
	fm.rom = memory->rom;
	fm.romSize = memory->rom ? memory->romSize : 0;
	uint32_t wram = (uint32_t) (uintptr_t) memory->wram;
	uint32_t rom = (uint32_t) (uintptr_t) memory->rom;
	/* Aligned as the guest's, or the two views of a page land in
	 * different cache lines. */
	fm.ram = wram && !(wram & 0xFFFF);
	int entry = FIRST_ENTRY;
	for (i = 0; i < IWRAM_PAGES; ++i, --entry) {
		if (fm.ram) {
			_entry(entry, BASE_WORKING_IRAM + (i << 10), wram + SIZE_WORKING_RAM + (i << 10), SZ_1K);
		}
	}
	for (i = 0; i < EWRAM_PAGES; ++i, --entry) {
		if (fm.ram) {
			_entry(entry, BASE_WORKING_RAM + (i << 16), wram + (i << 16), SZ_64K);
		}
	}
	if (rom && !(rom & 0xFFFFF)) {
		int pages = (fm.romSize + 0xFFFFF) >> 20;
		if (pages > MAX_ROM_PAGES) {
			pages = MAX_ROM_PAGES;
		}
		for (i = 0; i < pages; ++i, --entry) {
			_entry(entry, BASE_CART0 + (i << 20), rom + (i << 20), SZ_1M);
			UTLB_DATA(entry) &= ~PTE_RW;
		}
	}
	irq_restore(old);
}

extern uint32_t fastmem_dbg[4];
extern uint32_t fastmem_crash[8];

__attribute__((used, externally_visible)) void ARMJITFastmemCrash(void) {
	printf("fastmem CRASH: expevt %03x spc %08x tea %08x pr %08x ssr %08x r15 %08x r4 %08x r0 %08x\n",
	       (unsigned) fastmem_crash[1], (unsigned) fastmem_crash[0], (unsigned) fastmem_crash[2],
	       (unsigned) fastmem_crash[3], (unsigned) fastmem_crash[4], (unsigned) fastmem_crash[5],
	       (unsigned) fastmem_crash[6], (unsigned) fastmem_crash[7]);
	printf("  faults %u, last spc %08x addr %08x stub %08x expevt %03x, code %08x-%08x, vbr block %p\n",
	       (unsigned) fastmem_count, (unsigned) fastmem_dbg[0], (unsigned) fastmem_dbg[1], (unsigned) fastmem_dbg[2],
	       (unsigned) fastmem_dbg[3], (unsigned) fastmem_lo, (unsigned) fastmem_hi, fastmem_vbr);
	uint32_t spc = fastmem_crash[0] & ~1;
	if (spc >= 0x8C010000 && spc < 0x8D000000) {
		const uint16_t* p = (const uint16_t*) (spc - 16);
		int i;
		printf("  code at spc-16:");
		for (i = 0; i < 16; ++i) {
			printf(" %04x", p[i]);
		}
		printf("\n");
	}
	fflush(stdout);
	for (;;) {
		thd_sleep(1000);
	}
}

bool ARMJITFastmemInit(struct ARMJIT* jit) {
	UNUSED(jit);
	mmu_init_basic();
	return true;
}

void ARMJITFastmemInstall(struct ARMJIT* jit) {
	fastmem_lo = (uint32_t) (uintptr_t) jit->code + jit->codeBase;
	fastmem_hi = (uint32_t) (uintptr_t) jit->code + jit->codeSize;
	fastmem_table[0] = (uint32_t) (uintptr_t) jit->memStubs[0][JIT_MEM_STORE8];
	fastmem_table[1] = (uint32_t) (uintptr_t) jit->memStubs[0][JIT_MEM_STORE16];
	fastmem_table[2] = (uint32_t) (uintptr_t) jit->memStubs[0][JIT_MEM_STORE32];
	fastmem_table[4] = (uint32_t) (uintptr_t) jit->memStubs[0][JIT_MEM_LOADS8];
	fastmem_table[5] = (uint32_t) (uintptr_t) jit->memStubs[0][JIT_MEM_LOADS16];
	fastmem_table[6] = (uint32_t) (uintptr_t) jit->memStubs[0][JIT_MEM_LOAD32];
	_map(jit);
	int old = irq_disable();
	uint32_t vbr;
	__asm__ volatile("stc vbr,%0" : "=r"(vbr));
	fm.vbr = vbr;
	vbr = (uint32_t) (uintptr_t) fastmem_vbr;
	__asm__ volatile("ldc %0,vbr" : : "r"(vbr));
	fm.on = true;
	irq_restore(old);
}

void ARMJITFastmemDeinit(struct ARMJIT* jit) {
	UNUSED(jit);
	if (!fm.on) {
		return;
	}
	int old = irq_disable();
	__asm__ volatile("ldc %0,vbr" : : "r"(fm.vbr));
	fm.on = false;
	irq_restore(old);
	mmu_shutdown_basic();
}

void ARMJITFastmemUpdate(struct ARMJIT* jit) {
	struct GBA* gba = (struct GBA*) jit->cpu->master;
	struct GBAMemory* memory = &gba->memory;
	if (!fm.on) {
		return;
	}
	if (memory->wram == fm.wram && memory->rom == fm.rom && (memory->rom ? memory->romSize : 0) == fm.romSize) {
		return;
	}
	_map(jit);
	ARMJITFlush(jit->cpu);
}

static int _page(uint32_t address) {
	switch (address >> BASE_OFFSET) {
	case REGION_WORKING_IRAM:
		return FIRST_ENTRY - ((address & (SIZE_WORKING_IRAM - 1)) >> 10);
	case REGION_WORKING_RAM:
		return FIRST_ENTRY - IWRAM_PAGES - ((address & (SIZE_WORKING_RAM - 1)) >> 16);
	default:
		return -1;
	}
}

void ARMJITFastmemProtect(struct ARMJIT* jit, uint32_t start, uint32_t end) {
	UNUSED(jit);
	if (!fm.on || !fm.ram) {
		return;
	}
	int first = _page(start);
	int last = _page(end - 1);
	if (first >= 0) {
		UTLB_DATA(first) &= ~PTE_RW;
	}
	if (last >= 0 && last != first) {
		UTLB_DATA(last) &= ~PTE_RW;
	}
}

void ARMJITFastmemUnprotectPage(struct ARMJIT* jit, uint32_t address) {
	UNUSED(jit);
	if (!fm.on || !fm.ram) {
		return;
	}
	int page = _page(address);
	if (page >= 0) {
		UTLB_DATA(page) |= PTE_RW;
	}
}

void ARMJITFastmemUnprotect(struct ARMJIT* jit) {
	UNUSED(jit);
	if (!fm.on || !fm.ram) {
		return;
	}
	int i;
	for (i = 0; i < IWRAM_PAGES + EWRAM_PAGES; ++i) {
		UTLB_DATA(FIRST_ENTRY - i) |= PTE_RW;
	}
}

uint32_t ARMJITFastmemFaults(void) {
	return fastmem_count;
}
