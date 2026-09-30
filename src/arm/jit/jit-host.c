/* Off-target backend: run generated SH-4 under sh4-interp.h.
 *
 * Needs a 32-bit host so that the addresses baked into generated code (the
 * cpu, handler entry points, the code buffer) are the host's own. Calls out
 * of generated code to registered C functions run natively with r4-r7 as the
 * arguments and r0 as the result; anything else the code touches outside
 * its windows is a fault, which aborts with the SH-4 PC. */
#include "jit-private.h"

/* STEPSHACK */
unsigned long long sh4StepsTotal;
unsigned long long sh4CountSeqs;
__attribute__((destructor)) static void _stepsTotal(void) {
	fprintf(stderr, "SH4STEPS %llu countseqs %llu\n", sh4StepsTotal, sh4CountSeqs);
}
#include "sh4-interp.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static_assert(sizeof(void*) == 4, "the SH-4 JIT needs a 32-bit host (-m32)");

#define HOST_STACK_WORDS 1024
#define HOST_CALLS 4096
/* Where the entry stub "returns" to: ends the run. */
#define HOST_RETURN 0xFFFFFFF0u
#define HOST_BUDGET (1u << 30)
#define HOST_MAPS 16

struct ARMJITHost {
	struct sh4_state s;
	uint32_t stack[HOST_STACK_WORDS];
	uint32_t calls[HOST_CALLS];
	uint32_t nCalls;
	struct {
		uint32_t at;
		uint32_t size;
	} maps[HOST_MAPS];
	int nMaps;
	/* JIT_PROFILE=file: SH-4 instructions run per halfword of the buffer */
	uint32_t* prof;
	const char* profPath;
};

static void _profFetch(struct sh4_state* s, uint32_t pc) {
	struct ARMJIT* jit = s->user;
	uint32_t off = pc - s->code_at;
	if (off < s->code_size) {
		++jit->host->prof[off >> 1];
	}
}

static uint64_t _profSum(struct ARMJIT* jit, uint32_t from, uint32_t to) {
	uint64_t n = 0;
	uint32_t i;
	for (i = from >> 1; i < to >> 1; ++i) {
		n += jit->host->prof[i];
	}
	return n;
}

static void _profDump(struct ARMJIT* jit) {
	struct ARMJITHost* host = jit->host;
	FILE* f = fopen(host->profPath, "w");
	if (!f) {
		return;
	}
	uint64_t total = _profSum(jit, 0, jit->codeSize);
	fprintf(f, "total %llu stubs %llu code_at %08X\n", (unsigned long long) total,
	        (unsigned long long) _profSum(jit, 0, jit->codeBase), (uint32_t) (uintptr_t) jit->code);
#define STUB(NAME, P) fprintf(f, "stub %s %u\n", NAME, (uint32_t) ((const uint8_t*) (P) - jit->code))
	STUB("enter", jit->enter);
	STUB("exits", jit->exits[0]);
	STUB("lookup", jit->lookup);
	STUB("dispatchSync", jit->dispatchSync);
	STUB("handlerT", jit->handlers[1]);
	STUB("handlerA", jit->handlers[0]);
	STUB("stall", jit->stall);
	{
		static const char* const ops[8] = { "ld32", "ld16", "lds16", "ld8", "lds8", "st32", "st16", "st8" };
		char name[32];
		int i, j;
		for (i = 0; i < 8; ++i) {
			for (j = 0; j < 2; ++j) {
				snprintf(name, sizeof(name), "%s%s", ops[i], j ? "S" : "");
				STUB(name, jit->cpu->jitStubs[j][i]);
			}
		}
		for (i = 0; i < 8; ++i) {
			snprintf(name, sizeof(name), "multi%s%s%s", i & 1 ? "St" : "Ld", (i >> 1) & 1 ? "S" : "", i >> 2 ? "T" : "A");
			STUB(name, jit->multipleStubs[(i >> 1) & 1][i & 1][i >> 2]);
		}
	}
#undef STUB
	uint32_t i;
	for (i = 0; i < jit->nBlocks; ++i) {
		const struct JITBlock* b = &jit->blocks[i];
		if (b->dead || !b->code) {
			continue;
		}
		uint32_t off = b->code - jit->code;
		uint64_t n = _profSum(jit, off, off + b->codeSize);
		if (n) {
			fprintf(f, "block %08X %c insns %u bytes %u off %u run %llu entry %u\n", b->pc, b->thumb ? 'T' : 'A',
			        b->nInsns, b->codeSize, off, (unsigned long long) n, host->prof[off >> 1]);
		}
	}
	fclose(f);
	char path[512];
	snprintf(path, sizeof(path), "%s.bin", host->profPath);
	f = fopen(path, "wb");
	if (f) {
		fwrite(jit->code, 1, jit->codeUsed, f);
		fclose(f);
	}
	snprintf(path, sizeof(path), "%s.cnt", host->profPath);
	f = fopen(path, "wb");
	if (f) {
		fwrite(host->prof, 4, jit->codeUsed >> 1, f);
		fclose(f);
	}
}

void ARMJITHostMap(struct ARMJIT* jit, const void* p, uint32_t size) {
	struct ARMJITHost* host = jit->host;
	if (host->nMaps == HOST_MAPS) {
		fprintf(stderr, "JIT host: too many mappings\n");
		abort();
	}
	host->maps[host->nMaps].at = (uint32_t) (uintptr_t) p;
	host->maps[host->nMaps].size = size;
	++host->nMaps;
}

static uint8_t* _xlat(struct sh4_state* s, uint32_t a, uint32_t n) {
	struct ARMJIT* jit = s->user;
	struct ARMJITHost* host = jit->host;
	int i;
	for (i = 0; i < host->nMaps; ++i) {
		if (a - host->maps[i].at < host->maps[i].size && a + n - host->maps[i].at <= host->maps[i].size) {
			return (uint8_t*) (uintptr_t) a;
		}
	}
	return NULL;
}

static uint32_t _hash(uint32_t fn) {
	return (fn * 0x9E3779B1u) >> 20;
}

static bool _known(struct ARMJITHost* host, uint32_t fn) {
	uint32_t i;
	for (i = _hash(fn);; i = (i + 1) & (HOST_CALLS - 1)) {
		if (host->calls[i] == fn) {
			return true;
		}
		if (!host->calls[i]) {
			return false;
		}
	}
}

void ARMJITHostRegister(struct ARMJIT* jit, uint32_t fn) {
	struct ARMJITHost* host = jit->host;
	uint32_t i;
	for (i = _hash(fn);; i = (i + 1) & (HOST_CALLS - 1)) {
		if (host->calls[i] == fn) {
			return;
		}
		if (!host->calls[i]) {
			break;
		}
	}
	if (host->nCalls >= HOST_CALLS / 2) {
		fprintf(stderr, "JIT host: call table full\n");
		abort();
	}
	host->calls[i] = fn;
	++host->nCalls;
}

static int _callOut(struct sh4_state* s, uint32_t tgt) {
	struct ARMJIT* jit = s->user;
	if (tgt - s->code_at < s->code_size) {
		return 0;
	}
	if (!_known(jit->host, tgt)) {
		sh4_fault_at(s, "call to an unregistered function", tgt);
		return 1;
	}
	/* cdecl: extra arguments are harmless, a void function leaves r0
	 * undefined, which is what the SH-4 ABI says too. */
	typedef uint32_t (*Fn)(uint32_t, uint32_t, uint32_t, uint32_t);
	s->r[0] = ((Fn) (uintptr_t) tgt)(s->r[4], s->r[5], s->r[6], s->r[7]);
	/* Whatever the ABI lets a callee clobber, clobber, so generated code
	 * that relies on it breaks here and not on the Dreamcast. */
	int r;
	for (r = 1; r <= 7; ++r) {
		s->r[r] = 0xDEAD0000u | r;
	}
	s->t = 1;
	s->macl = 0xDEADDEADu;
	s->mach = 0xDEADDEADu;
	return 1;
}

bool ARMJITHostInit(struct ARMJIT* jit) {
	struct ARMJITHost* host = calloc(1, sizeof(*host));
	if (!host) {
		return false;
	}
	struct sh4_state* s = &host->s;
	s->mem = (uint8_t*) jit->cpu;
	s->mem_at = (uint32_t) (uintptr_t) jit->cpu;
	s->mem_size = sizeof(*jit->cpu);
	s->mem2 = (uint8_t*) host->stack;
	s->mem2_at = (uint32_t) (uintptr_t) host->stack;
	s->mem2_size = sizeof(host->stack);
	s->code = jit->code;
	s->code_at = (uint32_t) (uintptr_t) jit->code;
	s->code_size = jit->codeSize;
	s->call_out = _callOut;
	s->xlat = _xlat;
	s->user = jit;
	jit->host = host;
	host->profPath = getenv("JIT_PROFILE");
	if (host->profPath) {
		host->prof = calloc(jit->codeSize >> 1, sizeof(*host->prof));
		s->on_fetch = _profFetch;
	}
	return true;
}

void ARMJITHostDeinit(struct ARMJIT* jit) {
	if (jit->host->prof) {
		_profDump(jit);
		free(jit->host->prof);
	}
	free(jit->host);
	jit->host = NULL;
}

uint32_t jitFaultHist[2][16][2];
uint32_t jitFaultKind[4];

uint32_t ARMJITHostRun(struct ARMJIT* jit, const void* code) {
	struct sh4_state* s = &jit->host->s;
	s->stopped = 0;
	s->fault = 0;
	s->in_slot = 0;
	s->depth = 0;
	s->r[4] = (uint32_t) (uintptr_t) jit->cpu;
	s->r[5] = (uint32_t) (uintptr_t) code;
	s->r[15] = s->mem2_at + s->mem2_size;
	s->pr = HOST_RETURN;
	s->ret_at = HOST_RETURN;
	s->pc = (uint32_t) (uintptr_t) jit->enter;
	if (jit->fastmem) {
		/* What fastmem.c's exception entry does, with nothing mapped:
		 * every access in a block becomes a call of its stub. */
		static const int8_t ops[8] = {
			JIT_MEM_STORE8, JIT_MEM_STORE16, JIT_MEM_STORE32, -1,
			JIT_MEM_LOADS8, JIT_MEM_LOADS16, JIT_MEM_LOAD32, -1
		};
		uint32_t lo = s->code_at + jit->codeBase;
		s->steps = 0;
		while (!s->stopped && !s->fault) {
			if (s->steps >= (1u << 24)) {
				sh4_fault(s, "step budget exhausted", 0);
				break;
			}
			if (s->pc >= lo && s->pc - s->code_at < s->code_size) {
				uint16_t insn = sh4_fetch(s, s->pc);
				if ((insn >= 0x6040 && insn <= 0x6042) || (insn >= 0x2450 && insn <= 0x2452)) {
					int op = ops[(insn & 3) | ((insn >> 12) & 4)];
					if (op < 0) {
						sh4_fault(s, "fastmem: not an access", insn);
						break;
					}
					{
						/* What would fault on the Dreamcast, by kind */
						extern uint32_t jitFaultHist[2][16][2];
						uint32_t addr = s->r[4] & 0x0FFFFFFF;
						int region = addr >> 24;
						int st = op >= JIT_MEM_STORE32;
						int prot = 0;
						if (region == 2 || region == 3) {
							/* page with code in it: 64K pages in EWRAM, 1K in IWRAM */
							uint32_t size = region == 2 ? 0x10000 : 0x400;
							uint32_t base = region == 2 ? 0 : JIT_EWRAM_CHUNKS;
							uint32_t off = addr & (region == 2 ? 0x3FFFF : 0x7FFF) & ~(size - 1);
							extern uint8_t jitProtected[JIT_CHUNKS];
							(void) size;
							prot = jitProtected[base + (off >> 8)];
							if (!st) {
								prot = -1;
							}
						}
						{
							extern uint32_t jitFaultKind[4];
							int size = (op == JIT_MEM_LOAD32 || op == JIT_MEM_STORE32) ? 4
							         : (op == JIT_MEM_LOADS8 || op == JIT_MEM_STORE8) ? 1 : 2;
							if (addr & (size - 1)) {
								++jitFaultKind[0];
							} else if ((region == 3 && (addr & 0xFFFFFF) >= 0x8000) ||
							           (region == 2 && (addr & 0xFFFFFF) >= 0x40000)) {
								++jitFaultKind[1];
							} else if (region >= 8 && region < 14 && st) {
								++jitFaultKind[2];
							} else if (region >= 8 && (addr & 0x1FFFFFF) >= 0x400000) {
								++jitFaultKind[3];
							}
						}
						if (prot >= 0 && !(region >= 8 && !st)) {
							++jitFaultHist[st][region][prot];
						}
					}
					++s->depth;
					s->pr = s->pc + 2;
					s->pc = (uint32_t) (uintptr_t) jit->cpu->jitStubs[0][op];
					continue;
				}
			}
			sh4_step(s);
		}
	} else {
		sh4_run(s, HOST_BUDGET);
	}
	sh4StepsTotal += s->steps; /* STEPSHACK */
	if (s->fault) {
		fprintf(stderr, "JIT host: %s\n", s->fault_msg);
		abort();
	}
	return s->r[0];
}
