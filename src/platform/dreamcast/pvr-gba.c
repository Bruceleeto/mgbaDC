/* PVR renderer for GBA video (no OBJ window, mosaic or mid-frame
 * palette/OAM changes yet).
 *
 * The software renderer still runs the register side, but hands each line's
 * state to captureLine instead of drawing it. At the end of the frame:
 *  - every BG map is expanded into an 8bpp twiddled texture, one 64-byte
 *    block per map entry, redone only for entries (or tiles) that changed;
 *  - BGs are drawn as strips: bands of lines for text BGs, a strip per
 *    line for affine ones, with the per-line UVs;
 *  - sprites come from an atlas that caches them across frames, drawn as
 *    quads;
 *  - alpha blending goes in the translucent list, brighten/darken use the
 *    vertex and offset colours.
 * 4bpp tiles get their palette bank baked into the texel, so each layer
 * needs one palette: PVR banks 0/1 are the BG palette with every 16th / only
 * the first colour transparent, banks 2/3 the same for OBJ. Priority is
 * depth; colour 0 is dropped by the punch-through alpha test. */
#include "pvr-gba.h"

#include <mgba/core/profile.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/video.h>
#include <mgba/internal/gba/renderers/video-software.h>

#include <kos.h>
#include <dc/pvr.h>
#include <dc/sq.h>

extern void (*GBAVideoSoftwareLineHook)(struct GBAVideoSoftwareRenderer* renderer, int y);
extern uint32_t GBAVideoSoftwareVRAMDirty[96];
extern uint8_t GBAVideoSoftwareVRAMUnits[3072];
extern struct GBAVideoSoftwareRenderer* GBAVideoSoftwareLineHookTarget;

#define LINES 160
#define UNITS 3072 /* VRAM in 32-byte units */
#define ATLAS 1024

const pvr_init_params_t PVRGBAInitParams = {
	.opb_sizes = { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16 },
	.vertex_buf_size = 512 * 1024,
	.opb_overflow_count = 3,
};

struct LineBG {
	uint8_t on;
	uint8_t priority;
	uint8_t size;
	uint8_t pal256;
	uint8_t wrap;
	uint16_t hofs;
	uint16_t vofs;
	uint32_t charBase;
	uint32_t screenBase;
	int32_t sx;
	int32_t sy;
	int16_t dx;
	int16_t dy;
};

/* Window spans: the line cut where WIN0/WIN1 start and end, each part with
 * its WININ/WINOUT bits (0-3 BGs, 4 OBJ, 5 blending) */
#define MAX_SPANS 8

struct Line {
	uint16_t dispcnt;
	uint16_t bldcnt;
	uint16_t bldalpha;
	uint16_t bldy;
	struct LineBG bg[4];
	uint8_t nSpans;
	uint8_t spanEnd[MAX_SPANS];
	uint8_t spanCtl[MAX_SPANS];
};

static struct Line lines[LINES];
/* Some line this frame has a window */
static bool windowed;

static inline bool sameSpans(const struct Line* a, const struct Line* b) {
	return a->nSpans == b->nSpans && !memcmp(a->spanEnd, b->spanEnd, a->nSpans) &&
	       !memcmp(a->spanCtl, b->spanCtl, a->nSpans);
}

static void paintWindow(uint8_t* ctl, int start, int end, uint8_t value) {
	if (start < 0) start = 0;
	if (end > 240) end = 240;
	if (end > start) memset(&ctl[start], value, end - start);
}

/* As the software renderer's _breakWindow; the OBJ window counts as outside */
static void captureWindows(const struct GBAVideoSoftwareRenderer* renderer, struct Line* line, int y) {
	uint16_t dispcnt = renderer->dispcnt;
	if (!(dispcnt & 0xE000)) {
		line->nSpans = 1;
		line->spanEnd[0] = 240;
		line->spanCtl[0] = 0x3F;
		return;
	}
	uint8_t ctl[240];
	memset(ctl, renderer->winout.packed & 0x3F, sizeof(ctl));
	int w;
	for (w = 1; w >= 0; --w) {
		if (!(dispcnt & (0x2000 << w))) continue;
		const struct WindowN* win = &renderer->winN[w];
		int vs = win->v.start + win->offsetY, ve = win->v.end + win->offsetY;
		if (win->v.end >= win->v.start ? (y < vs || y >= ve) : (y >= ve && y < vs)) continue;
		uint8_t value = win->control.packed & 0x3F;
		if (win->h.end > 240 || win->h.end < win->h.start) {
			paintWindow(ctl, 0, win->h.end, value);
			paintWindow(ctl, win->h.start, 240, value);
		} else {
			paintWindow(ctl, win->h.start, win->h.end, value);
		}
	}
	unsigned n = 0;
	int x;
	for (x = 1; x <= 240; ++x) {
		if (x < 240 && ctl[x] == ctl[x - 1]) continue;
		if (n == MAX_SPANS - 1 && x < 240) continue;
		line->spanEnd[n] = x;
		line->spanCtl[n] = ctl[x - 1];
		++n;
	}
	line->nSpans = n;
	windowed = true;
}

struct Layer {
	pvr_ptr_t texture;
	uint32_t capacity;
	uint32_t key;
	bool valid;
	unsigned lastFrame;
	/* The frame it was last brought up to date in: dirty bits only cover
	 * one frame, so a layer that skipped one is rebuilt */
	unsigned builtFrame;
	uint16_t shadow[128 * 128];
	/* Bitmap layers: VRAM units changed since the last build */
	uint32_t pending[UNITS / 32];
};

/* Textures by BG configuration: a BG can change mode or BGCNT partway down
 * the screen (the sky above a mode 7 ground), and each part needs its own */
#define POOL 6
static struct Layer pool[POOL];
static unsigned frameCount;

static struct Layer* findLayer(uint32_t key) {
	struct Layer* victim = NULL;
	int i;
	for (i = 0; i < POOL; ++i) {
		if (pool[i].valid && pool[i].key == key) {
			pool[i].lastFrame = frameCount;
			return &pool[i];
		}
	}
	for (i = 0; i < POOL; ++i) {
		if (pool[i].lastFrame == frameCount && pool[i].valid) continue;
		if (!victim || !pool[i].valid || (victim->valid && pool[i].lastFrame < victim->lastFrame)) {
			victim = &pool[i];
		}
	}
	if (!victim) return NULL;
	victim->valid = false;
	victim->lastFrame = frameCount;
	return victim;
}

#ifdef PVR_GBA_DEBUG
static struct {
	unsigned frames, mapTiles, spriteTiles, layerRebuilds, layerScans;
} stats;
#define STAT(X, N) (stats.X += (N))
/* PVRGBA_NOCACHE=1: resend everything every frame, to check the caching */
static bool noCache;
#else
#define noCache false
#define STAT(X, N) ((void) 0)
#endif
static pvr_ptr_t atlas;

static uint8_t tiles4[UNITS][64] __attribute__((aligned(32)));
static uint8_t tiles8[UNITS / 2][64] __attribute__((aligned(32)));
static uint32_t dirty4[UNITS / 32];
static uint32_t dirty8[UNITS / 64];
static uint32_t frameDirty[UNITS / 32];
static uint8_t twiddle[64];
static uint16_t paletteShadow[512];
static bool paletteValid;

static void captureLine(struct GBAVideoSoftwareRenderer* renderer, int y) {
	struct Line* line = &lines[y];
	line->dispcnt = renderer->dispcnt;
	line->bldcnt = renderer->nextIo[REG_BLDCNT >> 1];
	line->bldalpha = renderer->nextIo[REG_BLDALPHA >> 1];
	line->bldy = renderer->nextIo[REG_BLDY >> 1];
	int i;
	for (i = 0; i < 4; ++i) {
		const struct GBAVideoSoftwareBackground* bg = &renderer->bg[i];
		struct LineBG* out = &line->bg[i];
		out->on = bg->enabled == 4;
		out->priority = bg->priority;
		out->size = bg->size;
		out->pal256 = bg->multipalette;
		out->wrap = bg->overflow;
		out->hofs = bg->x;
		out->vofs = bg->y;
		out->charBase = bg->charBase;
		out->screenBase = bg->screenBase;
		out->sx = bg->sx;
		out->sy = bg->sy;
		out->dx = bg->dx;
		out->dy = bg->dy;
	}
	captureWindows(renderer, line, y);
}

/* Twiddled index: y in bit 0, x in bit 1, and so on up */
static inline uint32_t spread(uint32_t v) {
	v = (v | (v << 8)) & 0x00FF00FF;
	v = (v | (v << 4)) & 0x0F0F0F0F;
	v = (v | (v << 2)) & 0x33333333;
	v = (v | (v << 1)) & 0x55555555;
	return v;
}

static inline uint32_t compact(uint32_t v) {
	v &= 0x55555555;
	v = (v | (v >> 1)) & 0x33333333;
	v = (v | (v >> 2)) & 0x0F0F0F0F;
	v = (v | (v >> 4)) & 0x00FF00FF;
	v = (v | (v >> 8)) & 0x0000FFFF;
	return v;
}

static inline uint32_t morton(uint32_t x, uint32_t y) {
	return spread(y) | (spread(x) << 1);
}

/* Byte offset of tile (tx, ty) in a twiddled 8bpp texture of wT x hT tiles:
 * rectangular textures are squares of the smaller side, laid end to end. */
static inline uint32_t tileOffset(unsigned tx, unsigned ty, unsigned wT, unsigned hT) {
	unsigned m = wT < hT ? wT : hT;
	unsigned block = tx / m + ty / m;
	return (block * m * m + morton(tx & (m - 1), ty & (m - 1))) * 64;
}

static const uint8_t* tile4(const uint8_t* vram, unsigned unit) {
	uint8_t* out = tiles4[unit];
	if (dirty4[unit >> 5] & (1U << (unit & 31))) {
		dirty4[unit >> 5] &= ~(1U << (unit & 31));
		mPROFILE_ADD(pConv4, "pvr: tiles converted 4bpp", 1);
		const uint8_t* src = &vram[unit * 32];
		int i;
		for (i = 0; i < 64; i += 2) {
			uint8_t b = src[i >> 1];
			out[twiddle[i]] = b & 0xF;
			out[twiddle[i + 1]] = b >> 4;
		}
	}
	return out;
}

static const uint8_t* tile8(const uint8_t* vram, unsigned index) {
	uint8_t* out = tiles8[index];
	if (dirty8[index >> 5] & (1U << (index & 31))) {
		dirty8[index >> 5] &= ~(1U << (index & 31));
		mPROFILE_ADD(pConv8, "pvr: tiles converted 8bpp", 1);
		const uint8_t* src = &vram[index * 64];
		int i;
		for (i = 0; i < 64; ++i) {
			out[twiddle[i]] = src[i];
		}
	}
	return out;
}

/* With the MMU on (fastmem), the store queues only reach the 1MB page
 * sq_lock mapped, so the lock follows the page being written. */
static uint32_t sqPage = ~0U;
static uint32_t* sqBase;

static uint32_t* sqAddress(pvr_ptr_t texture, uint32_t offset) {
	uint32_t address = PVR_TA_TEX_MEM | (((uintptr_t) texture & 0xFFFFFF) + offset);
	uint32_t page = address & ~0xFFFFFU;
	if (page != sqPage) {
		if (sqPage != ~0U) sq_unlock();
		sqBase = sq_lock((void*) page);
		sqPage = page;
	}
	return (uint32_t*) ((uintptr_t) sqBase + (address & 0xFFFE0));
}

static void sqRelease(void) {
	if (sqPage != ~0U) sq_unlock();
	sqPage = ~0U;
}

/* One 8x8 tile, 64 bytes, through both store queues. A flip is an XOR of the
 * twiddled index: x bits are the odd ones, y bits the even ones. */
static void emitTile(uint32_t* sq, const uint8_t* src, unsigned flip, uint32_t bank) {
	uint32_t flipped[16];
	const uint32_t* words = (const uint32_t*) src;
	if (flip) {
		uint8_t* bytes = (uint8_t*) flipped;
		int i;
		for (i = 0; i < 64; ++i) {
			bytes[i] = src[i ^ flip];
		}
		words = flipped;
	}
	int half;
	for (half = 0; half < 2; ++half) {
		sq[0] = words[0] | bank;
		sq[1] = words[1] | bank;
		sq[2] = words[2] | bank;
		sq[3] = words[3] | bank;
		sq[4] = words[4] | bank;
		sq[5] = words[5] | bank;
		sq[6] = words[6] | bank;
		sq[7] = words[7] | bank;
		sq_flush(sq);
		sq += 8;
		words += 8;
	}
}

static bool rangeDirty(uint32_t base, uint32_t length) {
	unsigned unit = base >> 5;
	unsigned end = (base + length) >> 5;
	if (end > UNITS) end = UNITS;
	for (; unit < end; unit += 32) {
		if (frameDirty[unit >> 5]) return true;
	}
	return false;
}

static bool reserve(struct Layer* layer, uint32_t bytes) {
	if (layer->capacity >= bytes) return true;
	if (layer->texture) pvr_mem_free(layer->texture);
	layer->texture = pvr_mem_malloc(bytes);
	layer->capacity = layer->texture ? bytes : 0;
	layer->valid = false;
	return layer->texture != NULL;
}

static bool lineHas(const struct Line* line, int bg, bool* affine) {
	if (line->dispcnt & 0x80) return false;
	if (!(line->dispcnt & (0x100 << bg)) || !line->bg[bg].on) return false;
	switch (line->dispcnt & 7) {
	case 3:
	case 4:
	case 5:
		*affine = true;
		return bg == 2;
	case 0:
		*affine = false;
		return true;
	case 1:
		*affine = bg == 2;
		return bg < 3;
	case 2:
		*affine = true;
		return bg >= 2;
	default:
		return false;
	}
}

static uint32_t layerKey(const struct LineBG* bg, bool affine) {
	return (bg->charBase >> 14) | ((bg->screenBase >> 11) << 2) | (bg->size << 7) | (bg->pal256 << 9) | (affine << 10);
}

/* Bitmap modes: BG2 is the frame buffer, keyed by mode and page */
#define BITMAP_KEY 0x800

static inline bool isBitmap(const struct Line* line) {
	return (line->dispcnt & 7) >= 3;
}

static uint32_t lineKey(const struct Line* line, int bg, bool affine) {
	if (isBitmap(line)) {
		unsigned mode = line->dispcnt & 7;
		return BITMAP_KEY | (mode << 12) | ((mode != 3 && (line->dispcnt & 0x10)) << 15);
	}
	return layerKey(&line->bg[bg], affine);
}

static inline bool unitDirty(unsigned unit) {
	return frameDirty[unit >> 5] & (1U << (unit & 31));
}

/* Brings the layer's texture up to date; returns its size in pixels. Only
 * map entries that changed, or whose tile did, are sent again. */
static void buildLayer(struct Layer* layer, const uint8_t* vram, const struct LineBG* bg, bool affine,
                       unsigned* width, unsigned* height) {
	uint32_t key = layerKey(bg, affine);
	unsigned wT, hT;
	if (affine) {
		wT = hT = 16 << bg->size;
	} else {
		wT = (bg->size & 1) ? 64 : 32;
		hT = (bg->size & 2) ? 64 : 32;
	}
	*width = wT * 8;
	*height = hT * 8;
	if (!reserve(layer, wT * hT * 64)) return;
	bool full = !layer->valid || layer->key != key || noCache;
	if (!full && layer->builtFrame == frameCount) return;
	full = full || layer->builtFrame != frameCount - 1;
	uint32_t mapBytes = affine ? wT * hT : wT * hT * 2;
	uint32_t charBytes = affine ? 0x4000 : bg->pal256 ? 0x10000 : 0x8000;
	if (bg->charBase + charBytes > 0x10000) charBytes = 0x10000 - bg->charBase;
	bool charDirty = rangeDirty(bg->charBase, charBytes);
	layer->key = key;
	layer->valid = true;
	layer->builtFrame = frameCount;
	if (!full && !charDirty && !rangeDirty(bg->screenBase, mapBytes)) return;
	STAT(layerScans, 1);
	if (full) STAT(layerRebuilds, 1);
	/* Without tile changes, only the map's dirty 32-byte units need a look */
	bool allUnits = full || charDirty;
	unsigned emitted = 0;
	if (affine) {
		const uint8_t* map = &vram[bg->screenBase];
		unsigned index;
		for (index = 0; index < wT * hT; index += 32) {
			if (!allUnits && !unitDirty(((bg->screenBase + index) >> 5) % UNITS)) continue;
			unsigned i;
			for (i = index; i < index + 32; ++i) {
				uint8_t entry = map[i];
				unsigned tile = ((bg->charBase >> 6) + entry) & 1023;
				unsigned unit = tile * 2;
				if (!full && layer->shadow[i] == entry && !(charDirty && (unitDirty(unit) || unitDirty(unit + 1)))) {
					continue;
				}
				layer->shadow[i] = entry;
				emitTile(sqAddress(layer->texture, tileOffset(i % wT, i / wT, wT, hT)),
				         tile8(vram, tile), 0, 0);
				++emitted;
			}
		}
	} else {
		const uint16_t* map = (const uint16_t*) &vram[bg->screenBase];
		unsigned tx, ty;
		for (ty = 0; ty < hT; ++ty) {
			for (tx = 0; tx < wT; tx += 16) {
				unsigned block = (tx >> 5) + (ty >> 5) * (wT >> 5);
				unsigned mapIndex = (block * 0x400 + (ty & 31) * 32 + (tx & 31)) & 0x7FFF;
				if (!allUnits && !unitDirty(((bg->screenBase >> 5) + (mapIndex >> 4)) % UNITS)) continue;
				unsigned i;
				for (i = 0; i < 16; ++i) {
					uint16_t entry = map[mapIndex + i];
					unsigned index = ty * wT + tx + i;
					unsigned unit;
					bool tileDirty;
					if (bg->pal256) {
						unit = ((((bg->charBase >> 6) + (entry & 0x3FF)) & 1023) * 2);
						tileDirty = charDirty && (unitDirty(unit) || unitDirty(unit + 1));
					} else {
						unit = ((bg->charBase >> 5) + (entry & 0x3FF)) & 2047;
						tileDirty = charDirty && unitDirty(unit);
					}
					if (!full && layer->shadow[index] == entry && !tileDirty) continue;
					layer->shadow[index] = entry;
					unsigned flip = ((entry & 0x400) ? 0x2A : 0) | ((entry & 0x800) ? 0x15 : 0);
					uint32_t* sq = sqAddress(layer->texture, tileOffset(tx + i, ty, wT, hT));
					if (bg->pal256) {
						emitTile(sq, tile8(vram, unit >> 1), flip, 0);
					} else {
						emitTile(sq, tile4(vram, unit), flip, (entry >> 12) * 0x10101010U);
					}
					++emitted;
				}
			}
		}
	}
	STAT(mapTiles, emitted);
	mPROFILE_ADD(pEmit, "pvr: map tiles emitted", emitted);
	if (full) mPROFILE_ADD(pRebuild, "pvr: layer rebuilds", 1);
}

/* A bitmap's size and texture: modes 3 and 5 are ARGB1555 with a 512-byte
 * stride, so each 32-byte VRAM unit is one store queue; mode 4 is twiddled
 * PAL8 made of 8x8 tiles */
struct BitmapFormat {
	unsigned width, height, texW, texH;
	uint32_t format;
};

static struct BitmapFormat bitmapFormat(uint32_t key) {
	switch ((key >> 12) & 7) {
	case 3:
		return (struct BitmapFormat) { 240, 160, 256, 256, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED };
	case 4:
		return (struct BitmapFormat) { 240, 160, 256, 256, 0 };
	default:
		return (struct BitmapFormat) { 160, 128, 256, 128, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED };
	}
}

static inline bool pendingUnit(const struct Layer* layer, unsigned unit) {
	return layer->pending[unit >> 5] & (1U << (unit & 31));
}

static void buildBitmap(struct Layer* layer, const uint8_t* vram, uint32_t key) {
	struct BitmapFormat f = bitmapFormat(key);
	uint32_t base = (key & 0x8000) ? 0xA000 : 0;
	if (!reserve(layer, f.texW * f.texH * (f.format ? 2 : 1))) return;
	bool full = !layer->valid || layer->key != key || noCache;
	if (full) memset(layer->pending, 0, sizeof(layer->pending));
	layer->key = key;
	layer->valid = true;
	if (layer->builtFrame == frameCount && !full) return;
	layer->builtFrame = frameCount;
	unsigned emitted = 0;
	if (f.format) {
		/* A unit is 16 pixels of one row */
		unsigned perRow = f.width / 16, units = perRow * f.height, u;
		unsigned first = base >> 5;
		for (u = 0; u < units; ++u) {
			if (!full && !pendingUnit(layer, first + u)) continue;
			const uint16_t* src = (const uint16_t*) &vram[base + u * 32];
			uint32_t* sq = sqAddress(layer->texture, (u / perRow) * 512 + (u % perRow) * 32);
			int i;
			for (i = 0; i < 8; ++i) {
				uint32_t c = src[i * 2] | (src[i * 2 + 1] << 16);
				/* GBA is BGR, the PVR RGB: swap the 5-bit R and B fields */
				c = 0x80008000U | ((c & 0x001F001FU) << 10) | (c & 0x03E003E0U) | ((c >> 10) & 0x001F001FU);
				sq[i] = c;
			}
			sq_flush(sq);
			++emitted;
		}
	} else {
		uint8_t tile[64] __attribute__((aligned(4)));
		unsigned tx, ty;
		for (ty = 0; ty < 20; ++ty) {
			uint32_t rowBase = base + ty * 8 * 240;
			if (!full) {
				unsigned u, dirty = 0;
				for (u = rowBase >> 5; u <= (rowBase + 1919) >> 5 && !dirty; ++u) dirty = pendingUnit(layer, u);
				if (!dirty) continue;
			}
			for (tx = 0; tx < 30; ++tx) {
				uint32_t o = rowBase + tx * 8;
				int r;
				if (!full) {
					bool dirty = false;
					for (r = 0; r < 8 && !dirty; ++r) {
						uint32_t a = o + r * 240;
						dirty = pendingUnit(layer, a >> 5) || pendingUnit(layer, (a + 7) >> 5);
					}
					if (!dirty) continue;
				}
				for (r = 0; r < 8; ++r) {
					const uint8_t* src = &vram[o + r * 240];
					int c;
					for (c = 0; c < 8; ++c) tile[twiddle[r * 8 + c]] = src[c];
				}
				emitTile(sqAddress(layer->texture, tileOffset(tx, ty, 32, 32)), tile, 0, 0);
				++emitted;
			}
		}
	}
	memset(layer->pending, 0, sizeof(layer->pending));
	STAT(mapTiles, emitted);
	mPROFILE_ADD(pBitmap, "pvr: bitmap units emitted", emitted);
}

static void updatePalette(const uint16_t* palette) {
	int i;
	for (i = 0; i < 512; ++i) {
		uint16_t c = palette[i];
		if (paletteValid && paletteShadow[i] == c) continue;
		paletteShadow[i] = c;
		uint32_t argb = 0x8000 | ((c & 0x1F) << 10) | (c & 0x3E0) | ((c >> 10) & 0x1F);
		unsigned index = (i < 256 ? 0 : 512) + (i & 255);
		pvr_set_pal_entry(index, (i & 15) ? argb : 0);
		pvr_set_pal_entry(index + 256, (i & 255) ? argb : 0);
	}
	paletteValid = true;
}

/* Screen transform: 240x160 scaled to the width of the PVR_GBA_WIDTH x
 * PVR_GBA_HEIGHT framebuffer, centred. Fill rate is the PVR's limit, so
 * the framebuffer is 320x240 and the video output line-doubles it. */
#define SCALE ((float) PVR_GBA_WIDTH / 240.0f)
#define ORIGIN_Y (((float) PVR_GBA_HEIGHT - 160.0f * SCALE) / 2)
/* Where in a GBA pixel the PVR's first sample lands: its centre at 1:1,
 * 1/8 in at 320 wide (samples at 1/8, 3/8, 5/8, 7/8). Affine texcoords
 * are phased from just below it so every sample reads the GBA's texel */
#if PVR_GBA_WIDTH == 240
#define AFFINE_PHASE 0.5f
#else
#define AFFINE_PHASE (0.125f - 1.0f / 32)
#endif

static inline void submitHeader(const pvr_poly_hdr_t* header) {
	uint32_t* target = pvr_dr_target();
	const uint32_t* words = (const uint32_t*) header;
	target[0] = words[0];
	target[1] = words[1];
	target[2] = words[2];
	target[3] = words[3];
	target[4] = words[4];
	target[5] = words[5];
	target[6] = words[6];
	target[7] = words[7];
	pvr_dr_commit(target);
}

/* How a layer's line is blended. Alpha blending (translucent list): the
 * vertex colour is EVA and its alpha 16 - EVB, blended ONE, INVSRCALPHA,
 * which gives src * EVA + dst * EVB and leaves dst alone where the texel is
 * transparent (its colour and alpha are 0). Brightness (punch-through):
 * the texel times 16 - EVY, plus EVY of white for brighten. */
enum {
	STYLE_PLAIN,
	STYLE_ALPHA,
	STYLE_BRIGHTEN,
	STYLE_DARKEN,
};

struct Style {
	uint8_t kind;
	uint32_t argb;
	uint32_t oargb;
};

static inline uint32_t level(unsigned coefficient) {
	if (coefficient > 16) coefficient = 16;
	return coefficient >= 16 ? 255 : coefficient * 16;
}

static struct Style makeStyle(unsigned kind, const struct Line* line) {
	struct Style style = { kind, 0xFFFFFFFF, 0 };
	unsigned eva = line->bldalpha & 0x1F, evb = (line->bldalpha >> 8) & 0x1F, evy = line->bldy & 0x1F;
	if (evy > 16) evy = 16;
	uint32_t gray;
	switch (kind) {
	case STYLE_ALPHA:
		gray = level(eva);
		style.argb = (level(16 - (evb > 16 ? 16 : evb)) << 24) | (gray << 16) | (gray << 8) | gray;
		break;
	case STYLE_BRIGHTEN:
		gray = level(16 - evy);
		style.argb = 0xFF000000 | (gray << 16) | (gray << 8) | gray;
		gray = level(evy);
		style.oargb = (gray << 16) | (gray << 8) | gray;
		break;
	case STYLE_DARKEN:
		gray = level(16 - evy);
		style.argb = 0xFF000000 | (gray << 16) | (gray << 8) | gray;
		break;
	}
	return style;
}

/* layer: 0-3 BGs, 4 OBJ, 5 backdrop */
static struct Style lineStyle(const struct Line* line, int layer) {
	unsigned mode = (line->bldcnt >> 6) & 3;
	if (!mode || !(line->bldcnt & (1 << layer))) return makeStyle(STYLE_PLAIN, line);
	if (mode == 1) {
		/* Blends with whatever is below; only target 2 should count */
		return makeStyle(line->bldcnt & 0x3F00 ? STYLE_ALPHA : STYLE_PLAIN, line);
	}
	if (!(line->bldy & 0x1F)) return makeStyle(STYLE_PLAIN, line);
	return makeStyle(mode == 2 ? STYLE_BRIGHTEN : STYLE_DARKEN, line);
}

/* A window span with blending off */
static inline struct Style spanStyle(const struct Line* line, int layer, uint8_t ctl) {
	return (ctl & 0x20) ? lineStyle(line, layer) : makeStyle(STYLE_PLAIN, line);
}

static inline bool sameStyle(const struct Style* a, const struct Style* b) {
	return a->kind == b->kind && a->argb == b->argb && a->oargb == b->oargb;
}

static struct Style currentStyle;

static inline void submitVertex(bool last, float x, float y, float z, float u, float v, uint32_t argb) {
	pvr_vertex_t* vertex = pvr_dr_target();
	vertex->flags = last ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
	vertex->x = x * SCALE;
	vertex->y = ORIGIN_Y + y * SCALE;
	vertex->z = z;
	vertex->u = u;
	vertex->v = v;
	vertex->argb = argb;
	vertex->oargb = currentStyle.oargb;
	pvr_dr_commit(vertex);
}

/* Corners in strip order: top left, top right, bottom left, bottom right */
static void submitQuad(const float* x, const float* y, float z, const float* u, const float* v) {
	int i;
	for (i = 0; i < 4; ++i) {
		submitVertex(i == 3, x[i], y[i], z, u[i], v[i], currentStyle.argb);
	}
}

static void rect(float x0, float y0, float x1, float y1, float z,
                 float u0, float v0, float u1, float v1) {
	const float x[4] = { x0, x1, x0, x1 };
	const float y[4] = { y0, y0, y1, y1 };
	const float u[4] = { u0, u1, u0, u1 };
	const float v[4] = { v0, v0, v1, v1 };
	submitQuad(x, y, z, u, v);
}

static void compileHeader(pvr_poly_hdr_t* header, pvr_ptr_t texture, unsigned bank, unsigned width, unsigned height,
                          unsigned kind, uint32_t format) {
	pvr_poly_cxt_t context;
	pvr_poly_cxt_txr(&context, kind == STYLE_ALPHA ? PVR_LIST_TR_POLY : PVR_LIST_PT_POLY,
	                 format ? format : PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(bank), width, height, texture,
	                 PVR_FILTER_NONE);
	context.txr.alpha = PVR_TXRALPHA_ENABLE;
	context.gen.culling = PVR_CULLING_NONE;
	switch (kind) {
	case STYLE_PLAIN:
		context.txr.env = PVR_TXRENV_REPLACE;
		break;
	case STYLE_ALPHA:
		context.txr.env = PVR_TXRENV_MODULATEALPHA;
		context.blend.src = PVR_BLEND_ONE;
		context.blend.dst = PVR_BLEND_INVSRCALPHA;
		break;
	default:
		context.txr.env = PVR_TXRENV_MODULATE;
		context.gen.specular = PVR_SPECULAR_ENABLE;
		break;
	}
	pvr_poly_compile(header, &context);
}

/* Sends a header when the style of what follows needs a different one */
struct HeaderState {
	pvr_ptr_t texture;
	unsigned bank, width, height;
	int kind;
	/* 0: PAL8 in bank */
	uint32_t format;
};

static void useStyle(struct HeaderState* state, const struct Style* style) {
	currentStyle = *style;
	if (state->kind == style->kind) return;
	pvr_poly_hdr_t header;
	compileHeader(&header, state->texture, state->bank, state->width, state->height, style->kind, state->format);
	submitHeader(&header);
	state->kind = style->kind;
}

/* Lines drawn in this pass: the translucent pass takes the alpha blended ones */
static inline bool inPass(const struct Style* style, bool translucent) {
	return (style->kind == STYLE_ALPHA) == translucent;
}

static inline float layerDepth(unsigned priority, unsigned bg) {
	return (4 - priority) * 8.0f + (4 - bg);
}

/* Text BG: bands of lines with the same scroll and blending are one quad */
static void drawText(int bg, pvr_ptr_t texture, unsigned width, unsigned height, uint32_t key, bool translucent) {
	struct HeaderState state = { texture, 0, width, height, -1 };
	float invW = 1.0f / width, invH = 1.0f / height;
	unsigned strips = 0;
	int y = 0;
	while (y < LINES) {
		bool affine;
		const struct LineBG* line = &lines[y].bg[bg];
		if (!lineHas(&lines[y], bg, &affine) || affine || layerKey(line, false) != key) {
			++y;
			continue;
		}
		struct Style style = lineStyle(&lines[y], bg);
		int end = y + 1;
		while (end < LINES) {
			const struct LineBG* next = &lines[end].bg[bg];
			bool nextAffine;
			if (!lineHas(&lines[end], bg, &nextAffine) || nextAffine || next->hofs != line->hofs ||
			    next->vofs != line->vofs || next->priority != line->priority || layerKey(next, false) != key ||
			    !sameSpans(&lines[y], &lines[end])) {
				break;
			}
			struct Style nextStyle = lineStyle(&lines[end], bg);
			if (!sameStyle(&style, &nextStyle)) break;
			++end;
		}
		state.bank = line->pal256 ? 1 : 0;
		float u0 = (line->hofs & 0x1FF) * invW;
		float v0 = ((line->vofs & 0x1FF) + y) * invH;
		unsigned s, x0 = 0;
		for (s = 0; s < lines[y].nSpans; x0 = lines[y].spanEnd[s++]) {
			uint8_t ctl = lines[y].spanCtl[s];
			if (!(ctl & (1 << bg))) continue;
			struct Style spanned = spanStyle(&lines[y], bg, ctl);
			if (!inPass(&spanned, translucent)) continue;
			unsigned x1 = lines[y].spanEnd[s];
			useStyle(&state, &spanned);
			rect(x0, y, x1, end, layerDepth(line->priority, bg),
			     u0 + x0 * invW, v0, u0 + x1 * invW, v0 + (end - y) * invH);
			++strips;
		}
		y = end;
	}
	mPROFILE_ADD(pText, "pvr: text strips", strips);
}

/* Affine BG: a strip per line; without wrapping, cut to the part inside the map */
static void drawAffine(int bg, pvr_ptr_t texture, unsigned width, unsigned height, uint32_t key, bool translucent) {
	struct HeaderState state = { texture, 1, width, height, -1, 0 };
	float invU = 1.0f / (width * 256.0f), invV = 1.0f / (height * 256.0f);
	float limits[2] = { width * 256.0f, height * 256.0f };
	bool bitmap = key & BITMAP_KEY;
	if (bitmap) {
		struct BitmapFormat f = bitmapFormat(key);
		state.format = f.format;
		limits[0] = f.width * 256.0f;
		limits[1] = f.height * 256.0f;
	}
	unsigned strips = 0;
	int y;
	for (y = 0; y < LINES; ++y) {
		bool affine;
		const struct LineBG* line = &lines[y].bg[bg];
		if (!lineHas(&lines[y], bg, &affine) || !affine || lineKey(&lines[y], bg, true) != key) continue;
		float sx = line->sx, sy = line->sy, dx = line->dx, dy = line->dy;
		float lo = 0, hi = 240;
		if (!line->wrap || bitmap) {
			/* 0 <= s + x * d < limit on both axes */
			float s[2] = { sx, sy }, d[2] = { dx, dy };
			int a;
			for (a = 0; a < 2; ++a) {
				float limit = limits[a];
				if (d[a] == 0) {
					if (s[a] < 0 || s[a] >= limit) hi = 0;
				} else if (d[a] > 0) {
					float l = ceilf(-s[a] / d[a]), h = ceilf((limit - s[a]) / d[a]);
					if (l > lo) lo = l;
					if (h < hi) hi = h;
				} else {
					float l = floorf((limit - s[a]) / d[a]) + 1, h = floorf(-s[a] / d[a]) + 1;
					if (l > lo) lo = l;
					if (h < hi) hi = h;
				}
			}
			if (lo >= hi) {
				mPROFILE_ADD(pCulled, "pvr: affine lines outside map", 1);
				continue;
			}
		}
		unsigned s;
		float x0 = 0;
		for (s = 0; s < lines[y].nSpans; x0 = lines[y].spanEnd[s++]) {
			uint8_t ctl = lines[y].spanCtl[s];
			float a = x0 > lo ? x0 : lo, b = lines[y].spanEnd[s] < hi ? lines[y].spanEnd[s] : hi;
			if (!(ctl & (1 << bg)) || a >= b) continue;
			struct Style style = spanStyle(&lines[y], bg, ctl);
			if (!inPass(&style, translucent)) continue;
			useStyle(&state, &style);
			/* The GBA samples a pixel's left edge; half a 1/256 step in
			 * keeps texel edges off the sample points */
			float u0 = (sx + 0.5f + (a - AFFINE_PHASE) * dx) * invU, v0 = (sy + 0.5f + (a - AFFINE_PHASE) * dy) * invV;
			float u1 = (sx + 0.5f + (b - AFFINE_PHASE) * dx) * invU, v1 = (sy + 0.5f + (b - AFFINE_PHASE) * dy) * invV;
			/* The PVR misses texel edges by up to 1/8 texel: when an axis
			 * doesn't step along the line, sample the texel's centre */
			if (dx == 0) u0 = u1 = (floorf(sx / 256) + 0.5f) * 256 * invU;
			if (dy == 0) v0 = v1 = (floorf(sy / 256) + 0.5f) * 256 * invV;
			const float xs[4] = { a, b, a, b };
			const float ys[4] = { y, y, y + 1, y + 1 };
			const float us[4] = { u0, u1, u0, u1 };
			const float vs[4] = { v0, v1, v0, v1 };
			submitQuad(xs, ys, layerDepth(line->priority, bg), us, vs);
			++strips;
		}
	}
	mPROFILE_ADD(pAffine, "pvr: affine strips", strips);
}

struct Sprite {
	float x[4];
	float y[4];
	float u[4];
	float v[4];
	float z;
	bool bpp8;
	bool semi;
	int top, bottom;
	struct Style style;
};

static struct Sprite sprites[128];

static const uint8_t objWidth[3][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 } };
static const uint8_t objHeight[3][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 } };

/* Sprite cache: the atlas is 128x128 tiles in Morton order, a quarter for
 * each block size (1, 2, 4 or 8 tiles square). A sprite's pixels stay in
 * their block while its tiles don't change; a block the PVR may still be
 * drawing from (used last frame) is never written, a changed sprite gets a
 * new one instead. */
#define CACHE_BLOCKS (4096 + 1024 + 256 + 64)
#define CACHE_BUCKETS 1024
static const uint16_t classBase[4] = { 0, 4096, 5120, 5376 };
static const uint16_t classCount[4] = { 4096, 1024, 256, 64 };
static struct CacheBlock {
	uint32_t key;
	int32_t lastUsed;
	uint16_t next; /* id + 1 */
	bool live;
} cache[CACHE_BLOCKS];
static uint16_t buckets[CACHE_BUCKETS];
static uint16_t clockHand[4];

static inline unsigned cacheHash(uint32_t key) {
	return (key * 0x9E3779B1U) >> 22;
}

static int cacheFind(uint32_t key) {
	unsigned id = buckets[cacheHash(key)];
	while (id) {
		if (cache[id - 1].key == key) return id - 1;
		id = cache[id - 1].next;
	}
	return -1;
}

static void cacheRemove(int id) {
	uint16_t* link = &buckets[cacheHash(cache[id].key)];
	while (*link != id + 1) link = &cache[*link - 1].next;
	*link = cache[id].next;
	cache[id].live = false;
}

static int cacheAllocate(unsigned sizeClass, uint32_t key, int now) {
	unsigned n;
	for (n = 0; n < classCount[sizeClass]; ++n) {
		int id = classBase[sizeClass] + clockHand[sizeClass];
		if (++clockHand[sizeClass] == classCount[sizeClass]) clockHand[sizeClass] = 0;
		if (cache[id].lastUsed >= now - 1) continue;
		if (cache[id].live) cacheRemove(id);
		unsigned h = cacheHash(key);
		cache[id].key = key;
		cache[id].live = true;
		cache[id].next = buckets[h];
		buckets[h] = id + 1;
		return id;
	}
	return -1;
}

/* First Morton slot of a block */
static inline unsigned cacheSlot(int id) {
	unsigned sizeClass = id >= 5376 ? 3 : id >= 5120 ? 2 : id >= 4096 ? 1 : 0;
	return sizeClass * 4096 + ((id - classBase[sizeClass]) << (sizeClass * 2));
}

/* Finds each visible sprite's pixels in the cache, copying the ones that
 * aren't there, and works out their quads. Returns how many. */
static unsigned buildSprites(const uint8_t* vram, const uint16_t* oam, bool map1D, bool bitmap) {
	unsigned count = 0, tilesCopied = 0;
	int now = frameCount;
	int i;
	for (i = 0; i < 128; ++i) {
		uint16_t a = oam[i * 4], b = oam[i * 4 + 1], c = oam[i * 4 + 2];
		bool affine = a & 0x100;
		if (!affine && (a & 0x200)) continue;
		unsigned mode = (a >> 10) & 3;
		if (mode >= 2) continue;
		unsigned shape = a >> 14;
		if (shape == 3) continue;
		unsigned w = objWidth[shape][b >> 14], h = objHeight[shape][b >> 14];
		bool doubled = affine && (a & 0x200);
		int bw = doubled ? w * 2 : w, bh = doubled ? h * 2 : h;
		int x = b & 0x1FF, y = a & 0xFF;
		if (x >= 256) x -= 512;
		if (y + bh > 256) y -= 256;
		if (x >= 240 || x + bw <= 0 || y >= 160 || y + bh <= 0) continue;
		bool bpp8 = a & 0x2000;
		unsigned tile = c & 0x3FF, priority = (c >> 10) & 3;
		/* Bitmap modes own the first half of OBJ VRAM; those tiles don't draw */
		if (bitmap && tile < 512) continue;
		unsigned bank = bpp8 ? 0 : c >> 12;
		unsigned wT = w >> 3, hT = h >> 3, tx, ty;
		unsigned side = wT > hT ? wT : hT;
		unsigned sizeClass = side == 8 ? 3 : side == 4 ? 2 : side == 2 ? 1 : 0;
		uint32_t key = tile | (shape << 10) | ((b >> 14) << 12) | (bpp8 << 14) | (bank << 15) | (map1D << 19);

		int id = cacheFind(key);
		bool copy = true;
		if (noCache && id >= 0 && cache[id].lastUsed != now) {
			cacheRemove(id);
			id = -1;
		}
		if (id >= 0) {
			if (cache[id].lastUsed == now) {
				copy = false;
			} else if (cache[id].lastUsed == now - 1) {
				/* Still valid if none of its tiles changed this frame */
				bool dirty = false;
				for (ty = 0; ty < hT && !dirty; ++ty) {
					for (tx = 0; tx < wT; ++tx) {
						unsigned n = map1D ? tile + (ty * wT + tx) * (bpp8 ? 2 : 1) : tile + ty * 32 + tx * (bpp8 ? 2 : 1);
						unsigned unit = 2048 + (n & 0x3FF);
						if (bpp8) unit &= ~1U;
						if (unitDirty(unit) || (bpp8 && unitDirty(unit + 1))) {
							dirty = true;
							break;
						}
					}
				}
				if (dirty) {
					/* The PVR may be drawing from this block: use another */
					cacheRemove(id);
					id = cacheAllocate(sizeClass, key, now);
				} else {
					copy = false;
				}
			}
		} else {
			id = cacheAllocate(sizeClass, key, now);
		}
		if (id < 0) continue;
		cache[id].lastUsed = now;
		unsigned slot = cacheSlot(id);
		if (copy) {
			uint32_t bankBits = bank * 0x10101010U;
			for (ty = 0; ty < hT; ++ty) {
				for (tx = 0; tx < wT; ++tx) {
					unsigned n = map1D ? tile + (ty * wT + tx) * (bpp8 ? 2 : 1) : tile + ty * 32 + tx * (bpp8 ? 2 : 1);
					unsigned unit = 2048 + (n & 0x3FF);
					uint32_t* sq = sqAddress(atlas, (slot + morton(tx, ty)) * 64);
					emitTile(sq, bpp8 ? tile8(vram, unit >> 1) : tile4(vram, unit), 0, bankBits);
				}
			}
			tilesCopied += wT * hT;
		}
		float u0 = compact(slot >> 1) * 8.0f / ATLAS, v0 = compact(slot) * 8.0f / ATLAS;
		float u1 = u0 + (float) w / ATLAS, v1 = v0 + (float) h / ATLAS;
		struct Sprite* sprite = &sprites[count++];
		sprite->z = (4 - priority) * 8.0f + 6 + (127 - i) / 128.0f;
		sprite->bpp8 = bpp8;
		sprite->semi = mode == 1;
		sprite->top = y;
		sprite->bottom = y + bh;
		{
			/* Semi-transparent sprites always alpha blend */
			const struct Line* line = &lines[y < 0 ? 0 : y > LINES - 1 ? LINES - 1 : y];
			sprite->style = mode == 1 ? makeStyle(STYLE_ALPHA, line) : lineStyle(line, 4);
		}
		sprite->u[0] = sprite->u[2] = u0;
		sprite->u[1] = sprite->u[3] = u1;
		sprite->v[0] = sprite->v[1] = v0;
		sprite->v[2] = sprite->v[3] = v1;
		if (!affine) {
			if (b & 0x1000) {
				sprite->u[0] = sprite->u[2] = u1;
				sprite->u[1] = sprite->u[3] = u0;
			}
			if (b & 0x2000) {
				sprite->v[0] = sprite->v[1] = v1;
				sprite->v[2] = sprite->v[3] = v0;
			}
			sprite->x[0] = sprite->x[2] = x;
			sprite->x[1] = sprite->x[3] = x + w;
			sprite->y[0] = sprite->y[1] = y;
			sprite->y[2] = sprite->y[3] = y + h;
		} else {
			unsigned m = ((b >> 9) & 31) * 16;
			float pa = (int16_t) oam[m + 3] / 256.0f, pb = (int16_t) oam[m + 7] / 256.0f;
			float pc = (int16_t) oam[m + 11] / 256.0f, pd = (int16_t) oam[m + 15] / 256.0f;
			float det = pa * pd - pb * pc;
			if (det > -1e-6f && det < 1e-6f) {
				--count;
				continue;
			}
			float cx = x + bw * 0.5f, cy = y + bh * 0.5f;
			int k;
			for (k = 0; k < 4; ++k) {
				float rx = ((k & 1) ? w : 0) - w * 0.5f;
				float ry = ((k & 2) ? h : 0) - h * 0.5f;
				sprite->x[k] = cx + (pd * rx - pb * ry) / det;
				sprite->y[k] = cy + (pa * ry - pc * rx) / det;
			}
		}
	}
	STAT(spriteTiles, tilesCopied);
	mPROFILE_ADD(pSprites, "pvr: sprites", count);
	mPROFILE_ADD(pSpriteTiles, "pvr: sprite tiles copied", tilesCopied);
	return count;
}

struct ClipVertex {
	float x, y, u, v;
};

/* Sutherland-Hodgman against one edge: keeps side * (p.axis - edge) >= 0 */
static unsigned clipEdge(const struct ClipVertex* in, unsigned n, struct ClipVertex* out, int axis, float edge, float side) {
	unsigned m = 0, i;
	for (i = 0; i < n; ++i) {
		const struct ClipVertex* a = &in[i];
		const struct ClipVertex* b = &in[(i + 1) % n];
		float da = side * ((axis ? a->y : a->x) - edge), db = side * ((axis ? b->y : b->x) - edge);
		if (da >= 0) out[m++] = *a;
		if ((da >= 0) != (db >= 0)) {
			float t = da / (da - db);
			out[m].x = a->x + (b->x - a->x) * t;
			out[m].y = a->y + (b->y - a->y) * t;
			out[m].u = a->u + (b->u - a->u) * t;
			out[m].v = a->v + (b->v - a->v) * t;
			++m;
		}
	}
	return m;
}

/* The part of a sprite inside a rectangle, as one strip */
static void submitClipped(const struct Sprite* sprite, float x0, float y0, float x1, float y1) {
	static const int order[4] = { 0, 1, 3, 2 };
	struct ClipVertex a[12], b[12];
	unsigned n = 4, i;
	for (i = 0; i < 4; ++i) {
		int k = order[i];
		a[i] = (struct ClipVertex) { sprite->x[k], sprite->y[k], sprite->u[k], sprite->v[k] };
	}
	n = clipEdge(a, n, b, 0, x0, 1);
	n = clipEdge(b, n, a, 0, x1, -1);
	n = clipEdge(a, n, b, 1, y0, 1);
	n = clipEdge(b, n, a, 1, y1, -1);
	if (n < 3) return;
	/* A convex polygon as a strip: zigzag in from both ends */
	unsigned lo = 1, hi = n - 1, k;
	for (k = 0; k < n; ++k) {
		const struct ClipVertex* p = !k ? &a[0] : (k & 1) ? &a[lo++] : &a[hi--];
		submitVertex(k == n - 1, p->x, p->y, sprite->z, p->u, p->v, currentStyle.argb);
	}
}

static void drawSprites(unsigned count, bool translucent) {
	struct HeaderState state = { atlas, 0, ATLAS, ATLAS, -1 };
	unsigned i;
	for (i = 0; i < count; ++i) {
		const struct Sprite* sprite = &sprites[i];
		unsigned bank = sprite->bpp8 ? 3 : 2;
		if (bank != state.bank) {
			state.bank = bank;
			state.kind = -1;
		}
		if (!windowed) {
			if (!inPass(&sprite->style, translucent)) continue;
			useStyle(&state, &sprite->style);
			submitQuad(sprite->x, sprite->y, sprite->z, sprite->u, sprite->v);
			continue;
		}
		/* Bands of lines with the same windows, each cut to where OBJ shows */
		int y = sprite->top < 0 ? 0 : sprite->top;
		int bottom = sprite->bottom > LINES ? LINES : sprite->bottom;
		while (y < bottom) {
			int end = y + 1;
			while (end < bottom && sameSpans(&lines[y], &lines[end])) ++end;
			unsigned s, x0 = 0;
			for (s = 0; s < lines[y].nSpans; x0 = lines[y].spanEnd[s++]) {
				uint8_t ctl = lines[y].spanCtl[s];
				if (!(ctl & 0x10)) continue;
				struct Style style = (ctl & 0x20) ? sprite->style : makeStyle(STYLE_PLAIN, &lines[y]);
				if (!inPass(&style, translucent)) continue;
				useStyle(&state, &style);
				submitClipped(sprite, x0, y, lines[y].spanEnd[s], end);
			}
			y = end;
		}
	}
}

#ifdef PVR_GBA_DEBUG
void PVRGBADebugStats(void) {
	unsigned f = stats.frames ? stats.frames : 1;
	printf("pvr per frame: map tiles %.1f, sprite tiles %.1f, layer scans %.2f, full rebuilds %.2f\n",
	       (double) stats.mapTiles / f, (double) stats.spriteTiles / f, (double) stats.layerScans / f,
	       (double) stats.layerRebuilds / f);
}

void PVRGBADebugLines(void) {
	int y;
	for (y = 0; y < LINES; ++y) {
		const struct Line* line = &lines[y];
		printf("%3d dispcnt %04X", y, line->dispcnt);
		int bg;
		for (bg = 0; bg < 4; ++bg) {
			const struct LineBG* b = &line->bg[bg];
			bool affine = false;
			if (!lineHas(line, bg, &affine)) continue;
			if (affine) {
				printf(" | bg%d A p%d size%d wrap%d c%05X s%05X sx %d sy %d dx %d dy %d", bg, b->priority, b->size, b->wrap,
				       (unsigned) b->charBase, (unsigned) b->screenBase, (int) b->sx, (int) b->sy, b->dx, b->dy);
			} else {
				printf(" | bg%d T p%d size%d %s c%05X s%05X x%d y%d", bg, b->priority, b->size, b->pal256 ? "8bpp" : "4bpp",
				       (unsigned) b->charBase, (unsigned) b->screenBase, b->hofs, b->vofs);
			}
		}
		printf("\n");
	}
}
#endif

bool PVRGBAInit(struct GBA* gba) {
	int i;
	for (i = 0; i < 64; ++i) {
		twiddle[i] = morton(i & 7, i >> 3);
	}
	memset(dirty4, 0xFF, sizeof(dirty4));
	memset(dirty8, 0xFF, sizeof(dirty8));
	memset(frameDirty, 0xFF, sizeof(frameDirty));
#ifdef PVR_GBA_DEBUG
	noCache = getenv("PVRGBA_NOCACHE") != NULL;
#endif
	atlas = pvr_mem_malloc(ATLAS * ATLAS);
	if (!atlas) return false;
	for (i = 0; i < CACHE_BLOCKS; ++i) {
		cache[i].lastUsed = -2;
	}
	pvr_set_pal_format(PVR_PAL_ARGB1555);
	PVR_SET(PVR_PT_ALPHA_REF, 0x80);
	/* Every changed VRAM word has to reach the dirty bitmap */
	gba->video.renderer->coarseVRAM = false;
	GBAVideoSoftwareLineHookTarget = (struct GBAVideoSoftwareRenderer*) gba->video.renderer;
	GBAVideoSoftwareLineHook = captureLine;
	return true;
}

static uint32_t backdropColor(const struct Line* line, uint16_t c, bool blend) {
	unsigned r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
	if (line->dispcnt & 0x80) {
		r = g = b = 31;
	} else if (blend) {
		struct Style style = lineStyle(line, 5);
		unsigned evy = line->bldy & 0x1F;
		if (evy > 16) evy = 16;
		if (style.kind == STYLE_BRIGHTEN) {
			r += ((31 - r) * evy) >> 4;
			g += ((31 - g) * evy) >> 4;
			b += ((31 - b) * evy) >> 4;
		} else if (style.kind == STYLE_DARKEN) {
			r -= (r * evy) >> 4;
			g -= (g * evy) >> 4;
			b -= (b * evy) >> 4;
		}
	}
	return 0xFF000000 | (r << 19) | (g << 11) | (b << 3);
}

uint64_t PVRGBAUploadSplit[3];

void PVRGBAFrame(struct GBA* gba, uint64_t* uploadTime, uint64_t* submitTime) {
	uint64_t start = timer_us_gettime64();
	const uint8_t* vram = (const uint8_t*) gba->video.vram;
	const uint16_t* palette = gba->video.palette;
	int i;
	/* The JIT's VRAM stores mark bytes: into bits */
	const uint32_t* units = (const uint32_t*) GBAVideoSoftwareVRAMUnits;
	for (i = 0; i < UNITS / 4; ++i) {
		if (!units[i]) continue;
		uint32_t w = units[i], bits = 0;
		if (w & 0x000000FF) bits |= 1;
		if (w & 0x0000FF00) bits |= 2;
		if (w & 0x00FF0000) bits |= 4;
		if (w & 0xFF000000) bits |= 8;
		/* Little-endian: byte 0 is the lowest unit */
		GBAVideoSoftwareVRAMDirty[i >> 3] |= bits << ((i & 7) * 4);
		((uint32_t*) GBAVideoSoftwareVRAMUnits)[i] = 0;
	}
	for (i = 0; i < UNITS / 32; ++i) {
		uint32_t bits = GBAVideoSoftwareVRAMDirty[i];
		GBAVideoSoftwareVRAMDirty[i] = 0;
		frameDirty[i] |= bits;
		dirty4[i] |= bits;
	}
	for (i = 0; i < UNITS / 64; ++i) {
		/* Two 32-byte units per 8bpp tile */
		uint32_t lo = frameDirty[i * 2], hi = frameDirty[i * 2 + 1], bits = 0;
		int b;
		for (b = 0; b < 16; ++b) {
			if (lo & (3U << (b * 2))) bits |= 1U << b;
			if (hi & (3U << (b * 2))) bits |= 1U << (b + 16);
		}
		dirty8[i] |= bits;
	}
	for (i = 0; i < POOL; ++i) {
		if (!pool[i].valid || !(pool[i].key & BITMAP_KEY)) continue;
		int w;
		for (w = 0; w < UNITS / 32; ++w) pool[i].pending[w] |= frameDirty[w];
	}
	updatePalette(palette);
	uint64_t prepared = timer_us_gettime64();

	/* Each BG's lines, grouped by configuration; each group has a texture */
	struct Group {
		int bg;
		uint32_t key;
		bool affine;
		struct Layer* layer;
		unsigned width, height;
	} groups[16];
	unsigned nGroups = 0;
#ifdef M_PROFILE
	static const char* const lineNames[4][2] = {
		{ "pvr: bg0 text lines", "pvr: bg0 affine lines" }, { "pvr: bg1 text lines", "pvr: bg1 affine lines" },
		{ "pvr: bg2 text lines", "pvr: bg2 affine lines" }, { "pvr: bg3 text lines", "pvr: bg3 affine lines" },
	};
#endif
	int bg;
	for (bg = 0; bg < 4; ++bg) {
		int y;
		for (y = 0; y < LINES; ++y) {
			bool affine;
			if (!lineHas(&lines[y], bg, &affine)) continue;
#ifdef M_PROFILE
			++*mProfileCounterLookup(lineNames[bg][affine]);
#endif
			uint32_t key = lineKey(&lines[y], bg, affine);
			unsigned g;
			for (g = 0; g < nGroups; ++g) {
				if (groups[g].bg == bg && groups[g].key == key) break;
			}
			if (g < nGroups || nGroups == 16) continue;
			struct Layer* layer = findLayer(key);
			if (!layer) continue;
			if (key & BITMAP_KEY) {
				struct BitmapFormat f = bitmapFormat(key);
				buildBitmap(layer, vram, key);
				groups[g].width = f.texW;
				groups[g].height = f.texH;
			} else {
				buildLayer(layer, vram, &lines[y].bg[bg], affine, &groups[g].width, &groups[g].height);
			}
			if (!layer->texture) continue;
			groups[g].bg = bg;
			groups[g].key = key;
			groups[g].affine = affine;
			groups[g].layer = layer;
			++nGroups;
		}
	}
	mPROFILE_ADD(pGroups, "pvr: bg configs", nGroups);
	uint64_t layered = timer_us_gettime64();
	bool objects = false;
	for (i = 0; i < LINES; ++i) {
		if ((lines[i].dispcnt & 0x1080) == 0x1000) objects = true;
	}
	unsigned spriteCount = objects ? buildSprites(vram, gba->video.oam.raw, lines[0].dispcnt & 0x40, (lines[0].dispcnt & 7) >= 3) : 0;
	sqRelease();
	memset(frameDirty, 0, sizeof(frameDirty));
	uint64_t built = timer_us_gettime64();
	*uploadTime += built - start;
	PVRGBAUploadSplit[0] += prepared - start;
	PVRGBAUploadSplit[1] += layered - prepared;
	PVRGBAUploadSplit[2] += built - layered;

	pvr_scene_begin();
	pvr_list_begin(PVR_LIST_OP_POLY);
	{
		/* The backdrop, in bands of lines with the same colour: brightness
		 * applies to it too, and forced blank is white */
		pvr_poly_cxt_t context;
		pvr_poly_hdr_t header;
		pvr_poly_cxt_col(&context, PVR_LIST_OP_POLY);
		context.gen.culling = PVR_CULLING_NONE;
		pvr_poly_compile(&header, &context);
		submitHeader(&header);
		currentStyle.oargb = 0;
		int y = 0;
		while (y < LINES) {
			uint32_t argb = backdropColor(&lines[y], palette[0], true);
			uint32_t plain = backdropColor(&lines[y], palette[0], false);
			int end = y + 1;
			while (end < LINES && backdropColor(&lines[end], palette[0], true) == argb &&
			       (argb == plain || sameSpans(&lines[y], &lines[end]))) {
				++end;
			}
			unsigned s, x0 = 0;
			for (s = 0; s < lines[y].nSpans; x0 = lines[y].spanEnd[s++]) {
				unsigned x1 = lines[y].spanEnd[s];
				uint32_t c = (lines[y].spanCtl[s] & 0x20) ? argb : plain;
				/* Merge spans of one colour */
				while (s + 1 < lines[y].nSpans &&
				       ((lines[y].spanCtl[s + 1] & 0x20) ? argb : plain) == c) {
					x1 = lines[y].spanEnd[++s];
				}
				submitVertex(false, x0, y, 0.5f, 0, 0, c);
				submitVertex(false, x1, y, 0.5f, 0, 0, c);
				submitVertex(false, x0, end, 0.5f, 0, 0, c);
				submitVertex(true, x1, end, 0.5f, 0, 0, c);
			}
			y = end;
		}
	}
	pvr_list_finish();
	int pass;
	for (pass = 0; pass < 2; ++pass) {
		pvr_list_begin(pass ? PVR_LIST_TR_POLY : PVR_LIST_PT_POLY);
		unsigned g;
		for (g = 0; g < nGroups; ++g) {
			const struct Group* group = &groups[g];
			if (group->affine) {
				drawAffine(group->bg, group->layer->texture, group->width, group->height, group->key, pass);
			} else {
				drawText(group->bg, group->layer->texture, group->width, group->height, group->key, pass);
			}
		}
		drawSprites(spriteCount, pass);
		pvr_list_finish();
	}
	windowed = false;
	pvr_scene_finish();
	++frameCount;
#ifdef PVR_GBA_DEBUG
	++stats.frames;
#endif
	*submitTime += timer_us_gettime64() - built;
}
