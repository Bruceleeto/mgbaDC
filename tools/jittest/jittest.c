/* JIT vs interpreter, in lockstep.
 *
 * Two cores run the same ROM and input: A on mGBA's interpreter, B on the
 * SH-4 recompiler (under sh4-interp.h on this host), one block at a time:
 * B runs a block, A interprets as many instructions as the block reported,
 * without looking at events (the JIT only does between blocks), and when an
 * event is due both process it. After every step the CPU state must match
 * exactly; every frame the picture must, and every 60 frames RAM must. The
 * first divergence stops the run and is printed.
 *
 * Build: tools/jittest/build.sh   (links build-linux32/libmgba.a, -m32)
 */
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/serialize.h>
#include <mgba/internal/arm/jit.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/input.h>
#include <mgba-util/vfs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W GBA_VIDEO_HORIZONTAL_PIXELS
#define H GBA_VIDEO_VERTICAL_PIXELS

struct Harness {
	struct mCore* core;
	struct GBA* gba;
	struct ARMCore* cpu;
	color_t buffer[W * H];
};

static void _log(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(logger);
	if (!getenv("JITTEST_LOG") || !(level & (mLOG_ERROR | mLOG_WARN | mLOG_FATAL))) {
		return;
	}
	fprintf(stderr, "[%s] ", mLogCategoryName(category));
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
}

static struct mLogger _logger = { .log = _log };

static bool _init(struct Harness* h, const char* rom, const char* state, bool jit) {
	h->core = mCoreFind(rom);
	if (!h->core || !h->core->init(h->core)) {
		fprintf(stderr, "Can't init core for %s\n", rom);
		return false;
	}
	mCoreInitConfig(h->core, NULL);
	mCoreLoadConfig(h->core);
	h->core->setVideoBuffer(h->core, h->buffer, W);
	if (!mCoreLoadFile(h->core, rom)) {
		fprintf(stderr, "Can't load %s\n", rom);
		return false;
	}
	h->gba = h->core->board;
	h->cpu = h->core->cpu;
	if (jit && !ARMJITInit(h->cpu)) {
		fprintf(stderr, "Can't init JIT\n");
		return false;
	}
	h->core->reset(h->core);
	if (state) {
		struct VFile* vf = VFileOpen(state, O_RDONLY);
		if (!vf || !mCoreLoadStateNamed(h->core, vf, 0)) {
			fprintf(stderr, "Can't load state %s\n", state);
			return false;
		}
		vf->close(vf);
	}
	return true;
}

/* Same script language as tools/rendertest. */
static uint32_t _keysFor(const char* script, int frame) {
	uint32_t keys = 0;
	if (!script) {
		return 0;
	}
	if (!strcmp(script, "auto")) {
		script = "0-99999:*";
	}
	const char* p = script;
	while (*p) {
		int start, end, n = 0, pulse = 0;
		if (sscanf(p, "%d-%d/%d:%n", &start, &end, &pulse, &n) != 3 || !n) {
			pulse = 0;
			n = 0;
			if (sscanf(p, "%d-%d:%n", &start, &end, &n) != 2 || !n) {
				break;
			}
		}
		p += n;
		for (; *p && *p != ','; ++p) {
			if (frame < start || frame > end) {
				continue;
			}
			if (pulse && (frame - start) % pulse >= 4) {
				continue;
			}
			switch (*p) {
			case 'A': keys |= 1 << GBA_KEY_A; break;
			case 'B': keys |= 1 << GBA_KEY_B; break;
			case 's': keys |= 1 << GBA_KEY_SELECT; break;
			case 'S': keys |= 1 << GBA_KEY_START; break;
			case 'R': keys |= 1 << GBA_KEY_RIGHT; break;
			case 'L': keys |= 1 << GBA_KEY_LEFT; break;
			case 'U': keys |= 1 << GBA_KEY_UP; break;
			case 'D': keys |= 1 << GBA_KEY_DOWN; break;
			case 'r': keys |= 1 << GBA_KEY_R; break;
			case 'l': keys |= 1 << GBA_KEY_L; break;
			case '*':
				if (frame % 40 < 4) {
					keys |= (frame / 40) & 1 ? 1 << GBA_KEY_START : 1 << GBA_KEY_A;
				}
				break;
			}
		}
		if (*p == ',') {
			++p;
		}
	}
	return keys;
}

static void _printCPU(const char* name, const struct ARMCore* cpu) {
	int i;
	printf("%s:", name);
	for (i = 0; i < 16; ++i) {
		printf("%s r%d=%08X", i == 8 ? "\n   " : "", i, (uint32_t) cpu->gprs[i]);
	}
	printf("\n    cpsr=%08X spsr=%08X cycles=%d next=%d halted=%d mode=%s priv=%02X pf=%08X,%08X\n",
	       cpu->cpsr.packed, cpu->spsr.packed, cpu->cycles, cpu->nextEvent, cpu->halted,
	       cpu->executionMode ? "thumb" : "arm", cpu->privilegeMode, cpu->prefetch[0], cpu->prefetch[1]);
}

static bool _sameCPU(const struct ARMCore* a, const struct ARMCore* b) {
	return !memcmp(a->gprs, b->gprs, sizeof(a->gprs)) && a->cpsr.packed == b->cpsr.packed &&
	       a->spsr.packed == b->spsr.packed && a->cycles == b->cycles &&
	       /* Past an event, its exact value doesn't matter: processEvents only
	        * looks at whether one is due. */
	       (a->nextEvent == b->nextEvent || (a->cycles >= a->nextEvent && b->cycles >= b->nextEvent)) &&
	       a->halted == b->halted && a->executionMode == b->executionMode &&
	       a->privilegeMode == b->privilegeMode && !memcmp(a->prefetch, b->prefetch, sizeof(a->prefetch)) &&
	       !memcmp(a->bankedRegisters, b->bankedRegisters, sizeof(a->bankedRegisters)) &&
	       !memcmp(a->bankedSPSRs, b->bankedSPSRs, sizeof(a->bankedSPSRs));
}

static int _firstDiff(const void* va, const void* vb, size_t size) {
	const uint8_t* a = va;
	const uint8_t* b = vb;
	size_t i;
	for (i = 0; i < size; ++i) {
		if (a[i] != b[i]) {
			return (int) i;
		}
	}
	return -1;
}

static void _printStats(struct ARMCore* cpu) {
	struct ARMJITStats s;
	ARMJITGetStats(cpu, &s);
	printf("jit: %u blocks, %u insns (%u ARM, %.1f/block), %u code bytes (%.1f/insn), %u flushes, %u invalidations, "
	       "%u fallback steps, %llu block runs\n",
	       s.blocksCompiled, s.guestInsnsCompiled, s.armInsnsCompiled,
	       s.blocksCompiled ? (double) s.guestInsnsCompiled / s.blocksCompiled : 0.0, s.codeBytes,
	       s.guestInsnsCompiled ? (double) s.codeBytes / s.guestInsnsCompiled : 0.0, s.flushes, s.invalidations,
	       s.fallbackSteps, (unsigned long long) s.blockRuns);
	{
		extern uint32_t jitLoadHist[2][16];
		{
			extern uint32_t jitFaultHist[2][16][2];
			extern uint32_t jitFaultKind[4];
			printf("would fault too: misaligned %u, RAM mirror %u, store to ROM %u, past ROM end %u\n", jitFaultKind[0], jitFaultKind[1], jitFaultKind[2], jitFaultKind[3]);
			int st, rg;
			for (st = 0; st < 2; ++st) {
				printf("would fault, %s:", st ? "stores" : "loads");
				for (rg = 0; rg < 16; ++rg) {
					if (jitFaultHist[st][rg][0] || jitFaultHist[st][rg][1]) {
						printf(" %X:%u", rg, jitFaultHist[st][rg][0]);
						if (rg == 2 || rg == 3) {
							printf("(+%u code page)", jitFaultHist[st][rg][1]);
						}
					}
				}
				printf("\n");
			}
		}
		int k, r;
		for (k = 0; k < 2; ++k) {
			printf("loads %s:", k ? "from ROM+prefetch code" : "other code");
			for (r = 0; r < 16; ++r) {
				if (jitLoadHist[k][r]) {
					printf(" %X:%u", r, jitLoadHist[k][r]);
				}
			}
			printf("\n");
		}
	}
	{
		extern uint32_t jitHandlerHist[2][256];
		int k, j, best;
		for (k = 1; k >= 0; --k) {
			uint32_t h[256];
			memcpy(h, jitHandlerHist[k], sizeof(h));
			printf("handlers %s:", k ? "thumb op>>8" : "arm op>>20");
			for (j = 0; j < 12; ++j) {
				int b;
				best = 0;
				for (b = 1; b < 256; ++b) {
					if (h[b] > h[best]) {
						best = b;
					}
				}
				if (!h[best]) {
					break;
				}
				printf(" %02X:%u", best, h[best]);
				h[best] = 0;
			}
			printf("\n");
		}
	}
	printf("exits: event %u, fall %u, branch site %u, branch %u, sync %u, lookup %u\n", s.exits[0], s.exits[1],
	       s.exits[2], s.exits[3], s.exits[4], s.exits[5]);
}

/* JITTEST_HIST: what kinds of instruction the interpreter side executes. */
enum { H_ARM, H_ALU, H_BRANCH, H_BL, H_BX, H_LDRPC, H_LOAD, H_STORE, H_PUSHPOP, H_LDMSTM, H_OTHER, H_MAX };
static const char* const _histNames[H_MAX] = { "ARM", "alu", "b/bcc", "bl", "bx/hi-pc", "ldr pc", "load", "store",
	                                           "push/pop", "ldm/stm", "other" };
static uint64_t _hist[H_MAX];
static uint64_t _histRegion[2][16];
static bool hist;
static uint64_t _armHist[16];
static uint64_t _armCond;
static const char* const _armNames[16] = { "other", "mul", "ldrh/sb", "bx", "swp", "mrs/msr", "dp-imm", "dp-rsr",
	                                       "dp-shimm", "ldr", "str", "ldm", "stm", "bl", "b", "swi" };

static void _histCount(struct ARMCore* cpu) {
	uint32_t pc = cpu->gprs[ARM_PC];
	if (cpu->executionMode != MODE_THUMB) {
		++_hist[H_ARM];
		uint32_t a = cpu->prefetch[0];
		int c;
		if ((a & 0x0FC000F0) == 0x00000090 || (a & 0x0F8000F0) == 0x00800090) {
			c = 1; /* mul */
		} else if ((a & 0x0E000090) == 0x00000090 && (a & 0x60)) {
			c = 2; /* ldrh etc */
		} else if ((a & 0x0FFFFFF0) == 0x012FFF10) {
			c = 3; /* bx */
		} else if ((a & 0x0FB00FF0) == 0x01000090) {
			c = 4; /* swp */
		} else if ((a & 0x0D900000) == 0x01000000) {
			c = 5; /* mrs/msr */
		} else if ((a & 0x0C000000) == 0) {
			c = (a & 0x02000000) ? 6 : (a & 0x10) ? 7 : 8; /* dp imm, dp reg-shift-reg, dp shift-imm */
		} else if ((a & 0x0C000000) == 0x04000000) {
			c = (a & 0x00100000) ? 9 : 10; /* ldr, str */
		} else if ((a & 0x0E000000) == 0x08000000) {
			c = (a & 0x00100000) ? 11 : 12; /* ldm, stm */
		} else if ((a & 0x0E000000) == 0x0A000000) {
			c = (a & 0x01000000) ? 13 : 14; /* bl, b */
		} else if ((a & 0x0F000000) == 0x0F000000) {
			c = 15; /* swi */
		} else {
			c = 0;
		}
		++_armHist[c];
		if ((a >> 28) != 0xE) {
			++_armCond;
		}
		return;
	}
	uint32_t op = cpu->prefetch[0];
	(void) pc;
	int k;
	switch (op >> 11) {
	case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
		k = H_ALU;
		break;
	case 0x08:
		if ((op & 0xFF00) == 0x4700) {
			k = H_BX;
		} else if ((op & 0x0400) && ((op & 7) | ((op >> 4) & 8)) == 15 && ((op >> 8) & 3) != 1) {
			k = H_BX;
		} else {
			k = H_ALU;
		}
		break;
	case 0x09:
		k = H_LDRPC;
		break;
	case 0x0A: case 0x0B:
		k = ((op >> 9) & 7) < 3 ? H_STORE : H_LOAD;
		break;
	case 0x0C: case 0x0E: case 0x10: case 0x12:
		k = H_STORE;
		break;
	case 0x0D: case 0x0F: case 0x11: case 0x13:
		k = H_LOAD;
		break;
	case 0x14: case 0x15:
		k = H_ALU;
		break;
	case 0x16: case 0x17:
		k = (op & 0x0600) == 0x0400 ? H_PUSHPOP : (op & 0xFF00) == 0xB000 ? H_ALU : H_OTHER;
		break;
	case 0x18: case 0x19:
		k = H_LDMSTM;
		break;
	case 0x1A: case 0x1B: case 0x1C:
		k = (op & 0xFF00) == 0xDF00 ? H_OTHER : H_BRANCH;
		break;
	case 0x1E: case 0x1F:
		k = H_BL;
		break;
	default:
		k = H_OTHER;
		break;
	}
	++_hist[k];
	if (k == H_LOAD || k == H_STORE) {
		/* the address isn't decoded here; count by code region instead */
		++_histRegion[k == H_STORE][(pc >> 24) & 15];
	}
}

static void _histPrint(void) {
	uint64_t total = 0;
	int k;
	for (k = 0; k < H_MAX; ++k) {
		total += _hist[k];
	}
	printf("mix (%llu insns):", (unsigned long long) total);
	for (k = 0; k < H_MAX; ++k) {
		printf(" %s %.1f%%", _histNames[k], total ? 100.0 * _hist[k] / total : 0.0);
	}
	printf("\n");
	uint64_t arm = _hist[H_ARM];
	printf("arm (%llu, %.1f%% conditional):", (unsigned long long) arm, arm ? 100.0 * _armCond / arm : 0.0);
	for (k = 0; k < 16; ++k) {
		printf(" %s %.1f%%", _armNames[k], arm ? 100.0 * _armHist[k] / arm : 0.0);
	}
	printf("\n");
}

static void _usage(void) {
	fprintf(stderr,
	        "usage: jittest ROM [options]\n"
	        "  -n FRAMES   frames to run (default 600)\n"
	        "  -s STATE    load savestate first\n"
	        "  -i SCRIPT   input script (\"auto\" or \"start-end:KEYS,...\", see rendertest)\n"
	        "  -m MODE     compare (default), jit, interp\n");
}

int main(int argc, char** argv) {
	if (argc < 2) {
		_usage();
		return 1;
	}
	mLogSetDefaultLogger(&_logger);
	const char* rom = argv[1];
	int frames = 600;
	const char* state = NULL;
	const char* script = NULL;
	const char* mode = "compare";
	int i;
	for (i = 2; i + 1 < argc; i += 2) {
		const char* arg = argv[i];
		const char* val = argv[i + 1];
		if (!strcmp(arg, "-n")) {
			frames = atoi(val);
		} else if (!strcmp(arg, "-s")) {
			state = val;
		} else if (!strcmp(arg, "-i")) {
			script = val;
		} else if (!strcmp(arg, "-m")) {
			mode = val;
		} else {
			_usage();
			return 1;
		}
	}
	if (i != argc) {
		_usage();
		return 1;
	}

	hist = getenv("JITTEST_HIST") != NULL;
	bool compare = !strcmp(mode, "compare");
	static struct Harness a, b;
	if (compare) {
		if (!_init(&a, rom, state, false) || !_init(&b, rom, state, true)) {
			return 1;
		}
	} else if (!_init(&a, rom, state, !strcmp(mode, "jit"))) {
		return 1;
	}

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	uint64_t slices = 0;
	int frame;
	for (frame = 0; frame < frames; ++frame) {
		uint32_t keys = _keysFor(script, frame);
		a.core->setKeys(a.core, keys);
		if (!compare && getenv("JITTEST_STEP")) {
			uint32_t fc = a.gba->video.frameCounter;
			while (a.gba->video.frameCounter == fc) {
				if (a.cpu->cycles >= a.cpu->nextEvent) {
					a.cpu->irqh.processEvents(a.cpu);
				} else {
					ARMRunInstruction(a.cpu);
				}
			}
			continue;
		}
		if (!compare) {
			a.core->runFrame(a.core);
			continue;
		}
		b.core->setKeys(b.core, keys);
		uint32_t frameCounter = a.gba->video.frameCounter;
		while (a.gba->video.frameCounter == frameCounter) {
			uint32_t before = a.cpu->gprs[ARM_PC] - (a.cpu->executionMode == MODE_THUMB ? 2 : 4);
			const char* what;
			uint32_t n = 0;
			if (a.cpu->cycles >= a.cpu->nextEvent) {
				what = "events";
				a.cpu->irqh.processEvents(a.cpu);
				b.cpu->irqh.processEvents(b.cpu);
			} else {
				what = "block";
				n = ARMJITRun(b.cpu);
				uint32_t i;
				for (i = 0; i < n; ++i) {
					if (getenv("JITTEST_TRACE")) {
						printf("  %08X c=%d ne=%d\n", a.cpu->gprs[ARM_PC], a.cpu->cycles, a.cpu->nextEvent);
					}
					if (hist) {
						_histCount(a.cpu);
					}
					ARMRunInstruction(a.cpu);
				}
				/* Flags nothing reads before writing may be left stale */
				if (a.cpu->gprs[ARM_PC] == b.cpu->gprs[ARM_PC] && a.cpu->executionMode == b.cpu->executionMode) {
					uint32_t stale = ARMJITStaleFlags(b.cpu);
					b.cpu->cpsr.packed = (b.cpu->cpsr.packed & ~stale) | (a.cpu->cpsr.packed & stale);
				}
			}
			++slices;
			if (getenv("JITTEST_MEM") && frame >= atoi(getenv("JITTEST_MEM"))) {
				int d = _firstDiff(a.gba->memory.iwram, b.gba->memory.iwram, SIZE_WORKING_IRAM);
				int d2 = _firstDiff(a.gba->memory.wram, b.gba->memory.wram, SIZE_WORKING_RAM);
				int d3 = _firstDiff(a.gba->memory.io, b.gba->memory.io, sizeof(a.gba->memory.io));
				if (d >= 0 || d2 >= 0 || d3 >= 0) {
					printf("memory differs at frame %d, step %llu (%s of %u at %08X): iwram %x wram %x io %x\n", frame,
					       (unsigned long long) slices, what, n, before, d, d2, d3);
					if (d >= 0) printf("iwram interp %02x jit %02x\n", ((uint8_t*) a.gba->memory.iwram)[d], ((uint8_t*) b.gba->memory.iwram)[d]);
					if (d3 >= 0) printf("io interp %02x jit %02x\n", ((uint8_t*) a.gba->memory.io)[d3], ((uint8_t*) b.gba->memory.io)[d3]);
					_printCPU("interp", a.cpu);
					_printCPU("jit   ", b.cpu);
					return 1;
				}
			}
			if (!_sameCPU(a.cpu, b.cpu)) {
				printf("CPU diverged at frame %d, step %llu (%s of %u at %08X)\n", frame,
				       (unsigned long long) slices, what, n, before);
				_printCPU("interp", a.cpu);
				_printCPU("jit   ", b.cpu);
				_printStats(b.cpu);
				return 1;
			}
		}
		int p = _firstDiff(a.buffer, b.buffer, sizeof(a.buffer));
		if (p >= 0) {
			printf("picture differs at frame %d, pixel %d\n", frame, p / (int) sizeof(color_t));
			return 1;
		}
		if (frame % 60 == 59) {
			int d = _firstDiff(a.gba->memory.wram, b.gba->memory.wram, SIZE_WORKING_RAM);
			if (d < 0) {
				d = _firstDiff(a.gba->memory.iwram, b.gba->memory.iwram, SIZE_WORKING_IRAM);
				if (d >= 0) {
					d += 0x03000000;
				}
			} else {
				d += 0x02000000;
			}
			if (d >= 0) {
				printf("RAM differs at frame %d, address %08X\n", frame, d);
				return 1;
			}
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	if (getenv("JITTEST_DUMP")) {
		FILE* f = fopen(getenv("JITTEST_DUMP"), "wb");
		if (f) {
			fprintf(f, "P6\n%d %d\n255\n", W, H);
			int k;
			for (k = 0; k < W * H; ++k) {
				color_t c = a.buffer[k];
				uint8_t rgb[3] = { (c >> 11) << 3, ((c >> 5) & 0x3F) << 2, (c & 0x1F) << 3 };
				fwrite(rgb, 1, 3, f);
			}
			fclose(f);
		}
	}
	if (getenv("JITTEST_IWRAM")) {
		FILE* f = fopen(getenv("JITTEST_IWRAM"), "wb");
		if (f) {
			fwrite(a.gba->memory.iwram, 1, SIZE_WORKING_IRAM, f);
			fclose(f);
		}
	}
	double seconds = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

	if (compare) {
		printf("compare: %d frames, %llu steps, all identical (%.2fs)\n", frames, (unsigned long long) slices,
		       seconds);
		_printStats(b.cpu);
		if (hist) {
			_histPrint();
		}
	} else {
		printf("%s: %d frames in %.2fs (%.1f fps)\n", mode, frames, seconds, frames / seconds);
		printf("audio: sampleInterval %d, SOUNDBIAS %04X\n", a.gba->audio.sampleInterval, a.gba->audio.soundbias);
		if (a.cpu->jit) {
			_printStats(a.cpu);
		}
	}
	a.core->deinit(a.core);
	if (compare) {
		b.core->deinit(b.core);
	}
	return 0;
}
