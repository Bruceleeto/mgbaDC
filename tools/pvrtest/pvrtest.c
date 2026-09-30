/* Offline check of the Dreamcast PVR renderer (src/platform/dreamcast/pvr-gba.c).
 *
 * Two cores run in lockstep with the same input: A draws with the software
 * renderer's reference path, B hands its lines to pvr-gba.c, which renders
 * through bloom's KOS shim onto libpvr (a software PVR). The PVR draws 1:1
 * at the top left, so each frame is compared pixel for pixel at 5 bits per
 * channel.
 *
 * Build: tools/pvrtest/build.sh   (links build-linux32/libmgba.a, 32-bit
 * like the shim).
 */
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/serialize.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/input.h>
#include <mgba/internal/gba/renderers/video-software.h>
#include <mgba-util/vfs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pvr-gba.h"

void PVRGBADebugLines(void);
void PVRGBADebugStats(void);

int kos_pvr_init(void);
int kos_pvr_read_front(uint8_t* rgb, int stride, int w, int h);
void vid_set_dithering(bool enable);

/* Called by the shim */
void kos_pvr_present(void) {
}

void gpu_census_sq(void) {
}

#define W GBA_VIDEO_HORIZONTAL_PIXELS
#define H GBA_VIDEO_VERTICAL_PIXELS
#define FB_W 640
#define FB_H 480

struct Harness {
	struct mCore* core;
	color_t buffer[W * H];
};

static uint8_t front[FB_W * FB_H * 3];

static void _log(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	(void) logger;
	if (!getenv("PVRTEST_LOG") || !(level & (mLOG_ERROR | mLOG_WARN | mLOG_FATAL))) {
		return;
	}
	fprintf(stderr, "[%s] ", mLogCategoryName(category));
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
}

static struct mLogger _logger = { .log = _log };

static bool _init(struct Harness* h, const char* rom, const char* state) {
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
	struct GBA* gba = h->core->board;
	((struct GBAVideoSoftwareRenderer*) gba->video.renderer)->fastPath = false;
	return true;
}

/* rendertest's input script: "start-end:KEYS,..." with KEYS from ABsSRLUDrl
 * (A B select Start Right Left Up Down R L), "start-end/N:KEYS" pulses the
 * keys 4 frames in every N, "*" presses Start/A in turn. */
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

static inline void _ref(color_t c, uint8_t* rgb) {
	rgb[0] = c >> 11;
	rgb[1] = (c >> 6) & 0x1F;
	rgb[2] = c & 0x1F;
}

static inline void _pvr(int x, int y, uint8_t* rgb) {
	const uint8_t* p = &front[(y * FB_W + x) * 3];
	rgb[0] = p[0] >> 3;
	rgb[1] = p[1] >> 3;
	rgb[2] = p[2] >> 3;
}

static int _diff(const struct Harness* a, uint8_t* mask) {
	int x, y, n = 0;
	for (y = 0; y < H; ++y) {
		for (x = 0; x < W; ++x) {
			uint8_t r[3], p[3];
			_ref(a->buffer[y * W + x], r);
			_pvr(x, y, p);
			bool d = r[0] != p[0] || r[1] != p[1] || r[2] != p[2];
			mask[y * W + x] = d;
			n += d;
		}
	}
	return n;
}

/* reference | PVR | differing pixels in red */
static void _writePPM(const char* path, const struct Harness* a, const uint8_t* mask) {
	FILE* f = fopen(path, "wb");
	if (!f) {
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", W * 3, H);
	int x, y;
	for (y = 0; y < H; ++y) {
		for (x = 0; x < W * 3; ++x) {
			uint8_t c[3];
			if (x < W) {
				_ref(a->buffer[y * W + x], c);
			} else if (x < W * 2) {
				_pvr(x - W, y, c);
			} else {
				c[0] = mask[y * W + x - W * 2] ? 31 : 0;
				c[1] = c[2] = 0;
				if (!mask[y * W + x - W * 2]) {
					uint8_t r[3];
					_ref(a->buffer[y * W + x - W * 2], r);
					c[0] = c[1] = c[2] = (r[0] + r[1] + r[2]) / 6;
				}
			}
			uint8_t rgb[3] = { c[0] << 3, c[1] << 3, c[2] << 3 };
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
}

static void _usage(void) {
	fprintf(stderr,
	        "usage: pvrtest ROM [options]\n"
	        "  -n FRAMES      frames to run (default 600)\n"
	        "  -s STATE       load savestate first\n"
	        "  -S FRAME:FILE  save a state at FRAME\n"
	        "  -i SCRIPT      input script (\"auto\" or \"start-end:KEYS,...\")\n"
	        "  -d DIR         dump ref|pvr|diff frames to DIR\n"
	        "  -e N           dump every N frames (default 60)\n"
	        "  -q             only print the summary\n"
	        "  -r FRAME       print the PVR renderer's per-line BG state at FRAME\n");
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
	int saveFrame = -1;
	const char* saveFile = NULL;
	const char* script = NULL;
	const char* dumpDir = NULL;
	int dumpEvery = 60;
	bool quiet = false;
	int regFrame = -1;
	int i;
	for (i = 2; i < argc; ++i) {
		const char* arg = argv[i];
		if (!strcmp(arg, "-q")) {
			quiet = true;
			continue;
		}
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
		} else if (!strcmp(arg, "-r")) {
			regFrame = atoi(val);
		} else if (!strcmp(arg, "-e")) {
			dumpEvery = atoi(val);
		} else {
			_usage();
			return 1;
		}
	}

	static struct Harness a, b;
	if (!_init(&a, rom, state) || !_init(&b, rom, state)) {
		return 1;
	}
	if (kos_pvr_init()) {
		return 1;
	}
	vid_set_dithering(false);
	if (!PVRGBAInit(b.core->board)) {
		fprintf(stderr, "PVRGBAInit failed\n");
		return 1;
	}

	static uint8_t mask[W * H];
	uint64_t upload = 0, submit = 0, totalDiff = 0;
	int differing = 0;
	int frame;
	for (frame = 0; frame < frames; ++frame) {
		uint32_t keys = _keysFor(script, frame);
		a.core->setKeys(a.core, keys);
		b.core->setKeys(b.core, keys);
		a.core->runFrame(a.core);
		b.core->runFrame(b.core);
		PVRGBAFrame(b.core->board, &upload, &submit);
		kos_pvr_read_front(front, FB_W * 3, FB_W, FB_H);
		int n = _diff(&a, mask);
		totalDiff += n;
		if (n) {
			++differing;
		}
		if (!quiet && (n || frame % 60 == 0)) {
			printf("frame %d: %d pixels differ (%.1f%%)\n", frame, n, 100.0 * n / (W * H));
		}
		if (dumpDir && dumpEvery > 0 && frame % dumpEvery == 0) {
			char path[512];
			snprintf(path, sizeof(path), "%s/pvr-%05d.ppm", dumpDir, frame);
			_writePPM(path, &a, mask);
		}
		if (frame == regFrame) {
			PVRGBADebugLines();
			struct GBA* gba = a.core->board;
			const struct GBAVideoSoftwareRenderer* sw = (const struct GBAVideoSoftwareRenderer*) gba->video.renderer;
			int y;
			printf("reference:  y DISPCNT BLDCNT BLDALPHA BLDY WININ WINOUT WIN0H WIN0V WIN1H WIN1V MOSAIC\n");
			for (y = 0; y < H; ++y) {
				const uint16_t* io = sw->cache[y].io;
				printf("reference %3d  %04X   %04X   %04X    %04X  %04X  %04X  %04X  %04X  %04X  %04X  %04X\n", y, io[0],
				       io[0x28], io[0x29], io[0x2A], io[0x24], io[0x25], io[0x20], io[0x22], io[0x21], io[0x23], io[0x26]);
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
	PVRGBADebugStats();
	printf("pvrtest: %d/%d frames differ, %.2f%% of pixels\n", differing, frames,
	       100.0 * totalDiff / ((double) frames * W * H));
	return 0;
}
