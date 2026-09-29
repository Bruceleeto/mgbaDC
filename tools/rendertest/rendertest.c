/* Headless renderer test harness for the Linux build.
 *
 * Runs a ROM with a scripted input and either
 *  - compares the fast renderer path against the reference path frame by frame
 *    (two cores in lockstep, same input), or
 *  - benchmarks the time spent in drawScanline for one path.
 *
 * Build: tools/rendertest/build.sh   (links build-linux/libmgba.a)
 */
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/profile.h>
#include <mgba/core/serialize.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/input.h>
#include <mgba/internal/gba/renderers/video-software.h>
#include <mgba-util/vfs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <x86intrin.h>

#define W GBA_VIDEO_HORIZONTAL_PIXELS
#define H GBA_VIDEO_VERTICAL_PIXELS

struct Harness {
	struct mCore* core;
	color_t buffer[W * H];
	void (*drawScanline)(struct GBAVideoRenderer*, int y);
	uint64_t ticks;
	uint64_t lines;
};

static struct Harness* _active[2];

#ifdef M_PROFILE
static uint32_t _rdtsc32(void) {
	return (uint32_t) __rdtsc();
}
#endif

static void _log(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(logger);
	if (!getenv("RENDERTEST_LOG") || !(level & (mLOG_ERROR | mLOG_WARN | mLOG_FATAL))) {
		return;
	}
	fprintf(stderr, "[%s] ", mLogCategoryName(category));
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
}

static struct mLogger _logger = { .log = _log };

static void _timedScanline0(struct GBAVideoRenderer* renderer, int y) {
	uint64_t t = __rdtsc();
	_active[0]->drawScanline(renderer, y);
	_active[0]->ticks += __rdtsc() - t;
	++_active[0]->lines;
}

static void _timedScanline1(struct GBAVideoRenderer* renderer, int y) {
	uint64_t t = __rdtsc();
	_active[1]->drawScanline(renderer, y);
	_active[1]->ticks += __rdtsc() - t;
	++_active[1]->lines;
}

static struct GBAVideoSoftwareRenderer* _renderer(struct Harness* h) {
	struct GBA* gba = h->core->board;
	return (struct GBAVideoSoftwareRenderer*) gba->video.renderer;
}

static bool _init(struct Harness* h, int index, const char* rom, const char* state, bool fast) {
	h->core = mCoreFind(rom);
	if (!h->core || !h->core->init(h->core)) {
		fprintf(stderr, "Can't init core for %s\n", rom);
		return false;
	}
	mCoreInitConfig(h->core, NULL);
	mCoreConfigSetDefaultValue(&h->core->config, "idleOptimization", "detect");
	mCoreLoadConfig(h->core);
	h->core->setVideoBuffer(h->core, h->buffer, W);
	if (!mCoreLoadFile(h->core, rom)) {
		fprintf(stderr, "Can't load %s\n", rom);
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
	struct GBAVideoSoftwareRenderer* sw = _renderer(h);
	sw->fastPath = fast;
	_active[index] = h;
	h->drawScanline = sw->d.drawScanline;
	sw->d.drawScanline = index ? _timedScanline1 : _timedScanline0;
	return true;
}

static void _writePPM(const char* path, const color_t* a, const color_t* b) {
	FILE* f = fopen(path, "wb");
	if (!f) {
		return;
	}
	int width = b ? W * 3 : W;
	fprintf(f, "P6\n%d %d\n255\n", width, H);
	int x, y;
	for (y = 0; y < H; ++y) {
		for (x = 0; x < width; ++x) {
			unsigned c;
			if (x < W) {
				c = a[y * W + x];
			} else if (x < W * 2) {
				c = b[y * W + x - W];
			} else {
				c = a[y * W + x - W * 2] != b[y * W + x - W * 2] ? 0xF800 : 0;
			}
			uint8_t rgb[3] = { (c >> 11) << 3, ((c >> 5) & 0x3F) << 2, (c & 0x1F) << 3 };
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
}

/* Input script: comma separated "start-end:KEYS" (KEYS from ABsSRLUDrl for
 * A B select Start Right Left Up Down R L, or * to press Start/A in turn to
 * get through menus), frames inclusive. "start-end/N:KEYS" only presses the
 * keys for 4 frames out of every N. "auto" is "0-99999:*". */
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

static void _usage(void) {
	fprintf(stderr,
	        "usage: rendertest ROM [options]\n"
	        "  -n FRAMES      frames to run (default 600)\n"
	        "  -s STATE       load savestate first\n"
	        "  -S FRAME:FILE  save a state at FRAME\n"
	        "  -i SCRIPT      input script (\"auto\" or \"start-end:KEYS,...\")\n"
	        "  -d DIR         dump frames to DIR\n"
	        "  -e N           dump every N frames (default 60)\n"
	        "  -m MODE        compare (default), fast, slow\n"
	        "  -x N           stop comparing after N mismatched frames (default 5)\n"
	        "  -r FRAME       print per-line video registers at FRAME\n");
}

int main(int argc, char** argv) {
	if (argc < 2) {
		_usage();
		return 1;
	}
	mLogSetDefaultLogger(&_logger);
#ifdef M_PROFILE
	mProfileClock = _rdtsc32;
#endif
	const char* rom = argv[1];
	int frames = 600;
	const char* state = NULL;
	int saveFrame = -1;
	const char* saveFile = NULL;
	const char* script = NULL;
	const char* dumpDir = NULL;
	int dumpEvery = 60;
	const char* mode = "compare";
	int maxMismatch = 5;
	int regFrame = -1;
	int i;
	for (i = 2; i < argc; ++i) {
		const char* arg = argv[i];
		const char* val = i + 1 < argc ? argv[i + 1] : NULL;
		if (!val) {
			_usage();
			return 1;
		}
		++i;
		if (!strcmp(arg, "-n")) {
			frames = atoi(val);
		} else if (!strcmp(arg, "-s")) {
			state = val;
		} else if (!strcmp(arg, "-S")) {
			saveFrame = atoi(val);
			saveFile = strchr(val, ':');
			if (saveFile) {
				++saveFile;
			}
		} else if (!strcmp(arg, "-i")) {
			script = val;
		} else if (!strcmp(arg, "-d")) {
			dumpDir = val;
		} else if (!strcmp(arg, "-e")) {
			dumpEvery = atoi(val);
		} else if (!strcmp(arg, "-m")) {
			mode = val;
		} else if (!strcmp(arg, "-r")) {
			regFrame = atoi(val);
		} else if (!strcmp(arg, "-x")) {
			maxMismatch = atoi(val);
		} else {
			_usage();
			return 1;
		}
	}

	bool compare = !strcmp(mode, "compare");
	static struct Harness a, b;
	if (!_init(&a, 0, rom, state, !strcmp(mode, "fast"))) {
		return 1;
	}
	if (compare && !_init(&b, 1, rom, state, true)) {
		return 1;
	}

	int mismatchedFrames = 0;
	uint64_t mismatchedPixels = 0;
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int frame;
	for (frame = 0; frame < frames; ++frame) {
		uint32_t keys = _keysFor(script, frame);
		a.core->setKeys(a.core, keys);
		a.core->runFrame(a.core);
		if (compare) {
			b.core->setKeys(b.core, keys);
			b.core->runFrame(b.core);
			int diff = 0;
			int p;
			for (p = 0; p < W * H; ++p) {
				diff += a.buffer[p] != b.buffer[p];
			}
			if (diff) {
				mismatchedPixels += diff;
				if (mismatchedFrames < maxMismatch) {
					printf("frame %d: %d pixels differ\n", frame, diff);
					if (dumpDir) {
						char path[512];
						snprintf(path, sizeof(path), "%s/diff-%05d.ppm", dumpDir, frame);
						_writePPM(path, a.buffer, b.buffer);
					}
				}
				++mismatchedFrames;
			}
		}
		if (dumpDir && dumpEvery > 0 && frame % dumpEvery == 0) {
			char path[512];
			snprintf(path, sizeof(path), "%s/frame-%05d.ppm", dumpDir, frame);
			_writePPM(path, compare ? b.buffer : a.buffer, NULL);
		}
		if (frame == regFrame) {
			struct GBAVideoSoftwareRenderer* sw = _renderer(&a);
			int y;
			printf(" y  DISPCNT BG0CNT BG1CNT BG2CNT BG3CNT WININ WINOUT MOSAIC BLDCNT BLDALPHA BLDY\n");
			for (y = 0; y < H; ++y) {
				const uint16_t* io = sw->cache[y].io;
				printf("%3d  %04X    %04X   %04X   %04X   %04X   %04X  %04X   %04X   %04X   %04X     %04X\n", y,
				       io[0], io[4], io[5], io[6], io[7], io[0x24], io[0x25], io[0x26], io[0x28], io[0x29], io[0x2A]);
			}
		}
		if (frame == saveFrame && saveFile) {
			struct VFile* vf = VFileOpen(saveFile, O_CREAT | O_TRUNC | O_RDWR);
			if (vf) {
				mCoreSaveStateNamed(a.core, vf, 0);
				vf->close(vf);
				printf("saved state at frame %d to %s\n", frame, saveFile);
			}
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double seconds = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

	if (compare) {
		printf("compare: %d/%d frames differ, %llu pixels total\n", mismatchedFrames, frames,
		       (unsigned long long) mismatchedPixels);
		printf("drawScanline ticks/line: slow %.0f  fast %.0f  (%.2fx)\n",
		       (double) a.ticks / (a.lines ? a.lines : 1), (double) b.ticks / (b.lines ? b.lines : 1),
		       (double) a.ticks / (b.ticks ? b.ticks : 1));
	} else {
		printf("%s: %d frames in %.2fs (%.0f fps), drawScanline %.0f ticks/line, %.1f%% of run time\n", mode, frames,
		       seconds, frames / seconds, (double) a.ticks / (a.lines ? a.lines : 1),
		       100.0 * a.ticks / (seconds * 1e9 * 3.0));
	}
#ifdef M_PROFILE
	if (!compare) {
		// rdtsc runs at about 3GHz on the test machine
		mProfilePrint(seconds * 1000.0 / frames, frames, 3e6, NULL);
	}
#endif
	struct GBAVideoSoftwareRenderer* sw = _renderer(compare ? &b : &a);
	if (sw->fastLines + sw->slowLines) {
		printf("fast path lines: %.1f%%\n", 100.0 * sw->fastLines / (sw->fastLines + sw->slowLines));
	}
	return compare && mismatchedFrames ? 2 : 0;
}
