/* Off-target backend: run generated SH-4 under sh4-interp.h.
 *
 * Needs a 32-bit host so that the addresses baked into generated code (the
 * cpu, handler entry points, the code buffer) are the host's own. Calls out
 * of generated code to registered C functions run natively with r4-r7 as the
 * arguments and r0 as the result; anything else the code touches outside
 * its windows is a fault, which aborts with the SH-4 PC. */
#include "jit-private.h"

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
};

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
	return true;
}

void ARMJITHostDeinit(struct ARMJIT* jit) {
	free(jit->host);
	jit->host = NULL;
}

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
	sh4_run(s, HOST_BUDGET);
	if (s->fault) {
		fprintf(stderr, "JIT host: %s\n", s->fault_msg);
		abort();
	}
	return s->r[0];
}
