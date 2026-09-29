/* Fast scanline compositor for the software renderer.
 *
 * Paints layers back to front straight into the output row instead of the
 * flagged 32-bit front-to-back composite. Handles modes 0-2 without windows or
 * BG mosaic, alpha blending (including semi-transparent sprites) and
 * brighten/darken via the variant palette. Anything else falls back to the
 * regular path.
 */
#include "gba/renderers/software-private.h"

#include <mgba/core/interface.h>
#include <mgba/core/profile.h>
#include <mgba/internal/gba/gba.h>

#ifdef __GNUC__
#define FAST_INLINE static inline __attribute__((always_inline))
#else
#define FAST_INLINE static inline
#endif

// Per-pixel blend attributes: bit 0 = top is target 1, bit 1 = top is target 2.
// Target 1 pixels blend with what's under them as they're painted, which is
// exact as long as no layer is both target 1 and target 2 (checked before use).
#define ATTR_T1 1
#define ATTR_T2 2

struct FastRow {
	color_t* top;
	int blda;
	int bldb;
	uint8_t attr[GBA_VIDEO_HORIZONTAL_PIXELS];
};

FAST_INLINE void _put(struct FastRow* row, int x, color_t color, unsigned attr, bool blend) {
	if (blend) {
		if ((attr & ATTR_T1) && (row->attr[x] & ATTR_T2)) {
			color = mColorMix5Bit(row->blda, color, row->bldb, row->top[x]);
		}
		row->attr[x] = attr;
	}
	row->top[x] = color;
}

#define PUT_4BPP(X, SHIFT) \
	do { \
		unsigned p = (tileData >> (SHIFT)) & 0xF; \
		if (p) { \
			_put(row, (X), palette[p], attr, blend); \
		} \
	} while (0)

#define PUT_8BPP(X, SHIFT, WORD) \
	do { \
		unsigned p = ((WORD) >> (SHIFT)) & 0xFF; \
		if (p) { \
			_put(row, (X), mainPalette[p], attr, blend); \
		} \
	} while (0)

FAST_INLINE void _drawText(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y, struct FastRow* row, color_t* mainPalette, unsigned attr, bool blend) {
	int inX = (background->x - background->offsetX) & 0x1FF;
	int inY = y + background->y - background->offsetY;
	uint16_t* vram = renderer->d.vram;

	unsigned yBase = inY & 0xF8;
	if (background->size == 2) {
		yBase += inY & 0x100;
	} else if (background->size == 3) {
		yBase += (inY & 0x100) << 1;
	}
	yBase = (background->screenBase >> 1) + (yBase << 2);

	if (background->yCache != inY >> 3) {
		int tileX;
		int localX = 0;
		for (tileX = 0; tileX < 64; ++tileX, localX += 8) {
			unsigned xBase = localX & 0xF8;
			if (background->size & 1) {
				xBase += (localX & 0x100) << 5;
			}
			uint32_t screenBase = yBase + (xBase >> 3);
			uint16_t mapData;
			LOAD_16(mapData, screenBase << 1, vram);
			background->mapCache[tileX] = mapData;
		}
		background->yCache = inY >> 3;
	}

	int localY = inY & 7;
	int tile = inX >> 3;
	int outX = -(inX & 7);
	uint32_t charBase0 = background->charBase;
	if (!background->multipalette) {
		for (; outX < GBA_VIDEO_HORIZONTAL_PIXELS; outX += 8, ++tile) {
			uint16_t mapData = background->mapCache[tile & 0x3F];
			int ty = GBA_TEXT_MAP_VFLIP(mapData) ? 7 - localY : localY;
			uint32_t charBase = charBase0 + (GBA_TEXT_MAP_TILE(mapData) << 5) + (ty << 2);
			if (UNLIKELY(charBase >= 0x10000)) {
				continue;
			}
			uint32_t tileData;
			LOAD_32(tileData, charBase, vram);
			if (!tileData) {
				continue;
			}
			color_t* palette = &mainPalette[GBA_TEXT_MAP_PALETTE(mapData) << 4];
			if (GBA_TEXT_MAP_HFLIP(mapData)) {
				// Reverse nibble order so pixel i is always at bits 4*i
				tileData = (tileData >> 16) | (tileData << 16);
				tileData = ((tileData >> 8) & 0x00FF00FF) | ((tileData << 8) & 0xFF00FF00);
				tileData = ((tileData >> 4) & 0x0F0F0F0F) | ((tileData << 4) & 0xF0F0F0F0);
			}
			if (outX >= 0 && outX <= GBA_VIDEO_HORIZONTAL_PIXELS - 8) {
				PUT_4BPP(outX + 0, 0);
				PUT_4BPP(outX + 1, 4);
				PUT_4BPP(outX + 2, 8);
				PUT_4BPP(outX + 3, 12);
				PUT_4BPP(outX + 4, 16);
				PUT_4BPP(outX + 5, 20);
				PUT_4BPP(outX + 6, 24);
				PUT_4BPP(outX + 7, 28);
			} else {
				int i;
				for (i = 0; i < 8; ++i) {
					int x = outX + i;
					if (x >= 0 && x < GBA_VIDEO_HORIZONTAL_PIXELS) {
						PUT_4BPP(x, i * 4);
					}
				}
			}
		}
	} else {
		for (; outX < GBA_VIDEO_HORIZONTAL_PIXELS; outX += 8, ++tile) {
			uint16_t mapData = background->mapCache[tile & 0x3F];
			int ty = GBA_TEXT_MAP_VFLIP(mapData) ? 7 - localY : localY;
			uint32_t charBase = charBase0 + (GBA_TEXT_MAP_TILE(mapData) << 6) + (ty << 3);
			if (UNLIKELY(charBase >= 0x10000)) {
				continue;
			}
			uint32_t lo, hi;
			LOAD_32(lo, charBase, vram);
			LOAD_32(hi, charBase + 4, vram);
			if (!(lo | hi)) {
				continue;
			}
			if (GBA_TEXT_MAP_HFLIP(mapData)) {
				uint32_t t = lo;
				lo = (hi >> 24) | ((hi >> 8) & 0xFF00) | ((hi << 8) & 0xFF0000) | (hi << 24);
				hi = (t >> 24) | ((t >> 8) & 0xFF00) | ((t << 8) & 0xFF0000) | (t << 24);
			}
			if (outX >= 0 && outX <= GBA_VIDEO_HORIZONTAL_PIXELS - 8) {
				if (lo) {
					PUT_8BPP(outX + 0, 0, lo);
					PUT_8BPP(outX + 1, 8, lo);
					PUT_8BPP(outX + 2, 16, lo);
					PUT_8BPP(outX + 3, 24, lo);
				}
				if (hi) {
					PUT_8BPP(outX + 4, 0, hi);
					PUT_8BPP(outX + 5, 8, hi);
					PUT_8BPP(outX + 6, 16, hi);
					PUT_8BPP(outX + 7, 24, hi);
				}
			} else {
				int i;
				for (i = 0; i < 8; ++i) {
					int x = outX + i;
					if (x >= 0 && x < GBA_VIDEO_HORIZONTAL_PIXELS) {
						PUT_8BPP(x, (i & 3) * 8, i < 4 ? lo : hi);
					}
				}
			}
		}
	}
}

FAST_INLINE void _drawAffine(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, struct FastRow* row, color_t* mainPalette, unsigned attr, bool blend) {
	int32_t sizeAdjusted = 0x8000 << background->size;
	int32_t mask = sizeAdjusted - 1;
	int size = background->size;
	const uint8_t* screenBase = &((const uint8_t*) renderer->d.vram)[background->screenBase];
	const uint8_t* charBase = &((const uint8_t*) renderer->d.vram)[background->charBase];
	int32_t x = background->sx;
	int32_t y = background->sy;
	int32_t dx = background->dx;
	int32_t dy = background->dy;
	int outX;
	if (background->overflow) {
		for (outX = 0; outX < GBA_VIDEO_HORIZONTAL_PIXELS; ++outX, x += dx, y += dy) {
			int32_t localX = x & mask;
			int32_t localY = y & mask;
			unsigned mapData = screenBase[(localX >> 11) + (((localY >> 7) & 0x7F0) << size)];
			unsigned p = charBase[(mapData << 6) + ((localY & 0x700) >> 5) + ((localX & 0x700) >> 8)];
			if (p) {
				_put(row, outX, mainPalette[p], attr, blend);
			}
		}
	} else {
		for (outX = 0; outX < GBA_VIDEO_HORIZONTAL_PIXELS; ++outX, x += dx, y += dy) {
			if ((x | y) & ~mask) {
				continue;
			}
			unsigned mapData = screenBase[(x >> 11) + (((y >> 7) & 0x7F0) << size)];
			unsigned p = charBase[(mapData << 6) + ((y & 0x700) >> 5) + ((x & 0x700) >> 8)];
			if (p) {
				_put(row, outX, mainPalette[p], attr, blend);
			}
		}
	}
}

FAST_INLINE void _drawSprites(struct GBAVideoSoftwareRenderer* renderer, struct FastRow* row, unsigned priority, bool blend) {
	unsigned t2 = renderer->target2Obj ? ATTR_T2 : 0;
	int x;
	for (x = 0; x < GBA_VIDEO_HORIZONTAL_PIXELS; ++x) {
		uint32_t s = renderer->spriteLayer[x];
		if ((s & FLAG_UNWRITTEN) == FLAG_UNWRITTEN || (s >> OFFSET_PRIORITY) != priority) {
			continue;
		}
		unsigned attr = t2 | ((s & FLAG_TARGET_1) ? ATTR_T1 : 0);
		_put(row, x, (color_t) (s & 0x00FFFFFF), attr, blend);
	}
}

#define FAST_KERNELS(SUFFIX, BLEND) \
	static ATTRIBUTE_NOINLINE void _drawText ## SUFFIX(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y, struct FastRow* row, color_t* palette, unsigned attr) { \
		_drawText(renderer, background, y, row, palette, attr, BLEND); \
	} \
	static ATTRIBUTE_NOINLINE void _drawAffine ## SUFFIX(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, struct FastRow* row, color_t* palette, unsigned attr) { \
		_drawAffine(renderer, background, row, palette, attr, BLEND); \
	} \
	static ATTRIBUTE_NOINLINE void _drawSprites ## SUFFIX(struct GBAVideoSoftwareRenderer* renderer, struct FastRow* row, unsigned priority) { \
		_drawSprites(renderer, row, priority, BLEND); \
	}

FAST_KERNELS(Blend, true)
FAST_KERNELS(NoBlend, false)

static void _paint(struct GBAVideoSoftwareRenderer* renderer, int y, int spriteLayers, struct FastRow* row, bool blend) {
	int mode = GBARegisterDISPCNTGetMode(renderer->dispcnt);
	bool alpha = renderer->blendEffect == BLEND_ALPHA;
	bool variantFx = renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN;

	color_t backdrop = (renderer->target1Bd && variantFx) ? renderer->variantPalette[0] : renderer->normalPalette[0];
	int x;
	for (x = 0; x < GBA_VIDEO_HORIZONTAL_PIXELS; ++x) {
		row->top[x] = backdrop;
	}
	if (blend) {
		memset(row->attr, renderer->target2Bd ? ATTR_T2 : 0, sizeof(row->attr));
	}

	int priority;
	for (priority = 3; priority >= 0; --priority) {
		int i;
		for (i = 3; i >= 0; --i) {
			struct GBAVideoSoftwareBackground* bg = &renderer->bg[i];
			if (bg->enabled != ENABLED_MAX || renderer->d.disableBG[i] || (int) bg->priority != priority) {
				continue;
			}
			bool affine;
			if (mode == 0) {
				affine = false;
			} else if (mode == 1) {
				if (i == 3) {
					continue;
				}
				affine = i == 2;
			} else {
				if (i < 2) {
					continue;
				}
				affine = true;
			}
			unsigned attr = (bg->target2 ? ATTR_T2 : 0) | ((alpha && bg->target1) ? ATTR_T1 : 0);
			color_t* palette = (bg->target1 && variantFx) ? renderer->variantPalette : renderer->normalPalette;
			if (affine) {
				mPROFILE_START(profileAffine, "fast bg affine");
				if (blend) {
					_drawAffineBlend(renderer, bg, row, palette, attr);
				} else {
					_drawAffineNoBlend(renderer, bg, row, palette, attr);
				}
				mPROFILE_STOP(profileAffine);
			} else {
				mPROFILE_START(profileText, "fast bg text");
				if (blend) {
					_drawTextBlend(renderer, bg, y, row, palette, attr);
				} else {
					_drawTextNoBlend(renderer, bg, y, row, palette, attr);
				}
				mPROFILE_STOP(profileText);
			}
		}
		if (spriteLayers & (1 << priority)) {
			mPROFILE_START(profileSprites, "fast sprites composite");
			if (blend) {
				_drawSpritesBlend(renderer, row, priority);
			} else {
				_drawSpritesNoBlend(renderer, row, priority);
			}
			mPROFILE_STOP(profileSprites);
		}
	}
}

bool GBAVideoSoftwareRendererFastEligible(struct GBAVideoSoftwareRenderer* renderer) {
	GBARegisterDISPCNT dispcnt = renderer->dispcnt;
	if (!renderer->fastPath) {
		return false;
	}
	if (GBARegisterDISPCNTGetMode(dispcnt) > 2) {
		return false;
	}
	if (GBARegisterDISPCNTIsWin0Enable(dispcnt) || GBARegisterDISPCNTIsWin1Enable(dispcnt) || GBARegisterDISPCNTIsObjwinEnable(dispcnt)) {
		return false;
	}
	if (renderer->greenswap || renderer->d.highlightAmount) {
		return false;
	}
	if (renderer->mosaic & 0xFF) {
		int i;
		for (i = 0; i < 4; ++i) {
			if (renderer->bg[i].enabled && renderer->bg[i].mosaic) {
				return false;
			}
		}
	}
	return true;
}

// Called after the sprite layer is built. Returns false if the line needs the
// regular path after all.
bool GBAVideoSoftwareRendererDrawFast(struct GBAVideoSoftwareRenderer* renderer, int y, int spriteLayers, color_t* out) {
	bool variantFx = renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN;
	if (renderer->forceTarget1 && variantFx) {
		return false;
	}
	struct FastRow row;
	row.top = out;
	row.blda = renderer->blda;
	row.bldb = renderer->bldb;
	bool alpha = renderer->blendEffect == BLEND_ALPHA;
	bool blend = renderer->forceTarget1;
	if (alpha || renderer->forceTarget1) {
		// Sprites can be target 1 through alpha or semi-transparency
		bool objT1 = renderer->forceTarget1 || (alpha && renderer->target1Obj);
		if (objT1 && renderer->target2Obj) {
			return false;
		}
		blend = blend || objT1;
		int i;
		for (i = 0; i < 4; ++i) {
			struct GBAVideoSoftwareBackground* bg = &renderer->bg[i];
			if (bg->enabled != ENABLED_MAX) {
				continue;
			}
			if (alpha && bg->target1) {
				if (bg->target2) {
					return false;
				}
				blend = true;
			}
		}
	}
	_paint(renderer, y, spriteLayers, &row, blend);
	return true;
}
