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

// Keeps GCC from turning a shift-as-you-go walk into eight independent
// shifts of the original (which SH-4 then spills to the stack).
#ifdef __GNUC__
#define KEEP(X) __asm__("" : "+r"(X))
#else
#define KEEP(X)
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

// How a kernel paints a pixel. The text kernels know the layer's attr up
// front, so they're specialized on it; affine BGs and sprites check it per
// pixel.
enum PutKind {
	PUT_PLAIN, // no blending on this line
	PUT_ATTR, // blending, but this layer isn't target 1: just record attr
	PUT_MIX, // this layer is target 1: mix over target 2
	PUT_ANY, // attr decided per pixel
};

// mColorMix5Bit, with its clamps behind one test: a channel can only overflow
// when EVA + EVB > 16, so the usual case pays a single branch. Same result
// bit for bit; green keeps mGBA's 5 bits.
FAST_INLINE color_t _mix(int blda, unsigned a, int bldb, unsigned b) {
#if defined(COLOR_16_BIT) && defined(COLOR_5_6_5)
	a = (a | (a << 16)) & 0x07C0F81F;
	b = (b | (b << 16)) & 0x07C0F81F;
	unsigned c = (a * blda + b * bldb) >> 4;
	if (UNLIKELY(c & 0x08010020)) {
		if (c & 0x08000000) {
			c = (c & ~0x0FC00000) | 0x07C00000;
		}
		if (c & 0x0020) {
			c = (c & ~0x003F) | 0x001F;
		}
		if (c & 0x10000) {
			c = (c & ~0x1F800) | 0xF800;
		}
	}
	c &= 0x07C0F81F;
	return c | (c >> 16);
#else
	return mColorMix5Bit(blda, a, bldb, b);
#endif
}

FAST_INLINE void _put(color_t* top, uint8_t* attrs, int x, color_t color, unsigned attr, enum PutKind kind, int blda, int bldb) {
	if (kind == PUT_MIX || (kind == PUT_ANY && (attr & ATTR_T1))) {
		if (attrs[x] & ATTR_T2) {
			color = _mix(blda, color, bldb, top[x]);
		}
	}
	if (kind != PUT_PLAIN) {
		attrs[x] = attr;
	}
	top[x] = color;
}

#define PUT_4BPP(X, SHIFT) \
	do { \
		unsigned p = (tileData >> (SHIFT)) & 0xF; \
		if (p) { \
			_put(top, attrs, (X), palette[p], attr, kind, blda, bldb); \
		} \
	} while (0)

#define PUT_8BPP(X, SHIFT, WORD) \
	do { \
		unsigned p = ((WORD) >> (SHIFT)) & 0xFF; \
		if (p) { \
			_put(top, attrs, (X), mainPalette[p], attr, kind, blda, bldb); \
		} \
	} while (0)

// A 4bpp row with no transparent pixel: nibble-wise "has zero" test.
FAST_INLINE bool _opaque4(uint32_t t) {
	return !((t - 0x11111111) & ~t & 0x88888888);
}

FAST_INLINE bool _opaque8(uint32_t t) {
	return !((t - 0x01010101) & ~t & 0x80808080);
}

FAST_INLINE void _drawText(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y, struct FastRow* row, color_t* mainPalette, unsigned attr, enum PutKind kind) {
	int inX = (background->x - background->offsetX) & 0x1FF;
	int inY = y + background->y - background->offsetY;
	uint16_t* vram = renderer->d.vram;
	color_t* top = row->top;
	uint8_t* attrs = row->attr;
	int blda = row->blda;
	int bldb = row->bldb;

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
	const uint16_t* mapCache = background->mapCache;
	if (!background->multipalette) {
		for (; outX < GBA_VIDEO_HORIZONTAL_PIXELS; outX += 8, ++tile) {
			uint16_t mapData = mapCache[tile & 0x3F];
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
				color_t* t = &top[outX];
				uint8_t* a = &attrs[outX];
				if (kind != PUT_MIX && _opaque4(tileData)) {
					// Walk the row down one register; byte offsets into the palette
					int i;
					_Pragma("GCC unroll 8")
					for (i = 0; i < 8; ++i, tileData >>= 4) {
						KEEP(tileData);
						t[i] = *(color_t*) ((uintptr_t) palette + ((tileData << 1) & 0x1E));
						if (kind == PUT_ATTR) {
							a[i] = attr;
						}
					}
					continue;
				}
				int i;
				_Pragma("GCC unroll 8")
				for (i = 0; i < 8; ++i, tileData >>= 4) {
					KEEP(tileData);
					unsigned p = tileData & 0xF;
					if (p) {
						_put(t, a, i, palette[p], attr, kind, blda, bldb);
					}
				}
			} else {
				// Edge tiles
				int i = outX < 0 ? -outX : 0;
				int end = GBA_VIDEO_HORIZONTAL_PIXELS - outX < 8 ? GBA_VIDEO_HORIZONTAL_PIXELS - outX : 8;
				_Pragma("GCC unroll 1")
				for (tileData >>= i * 4; i < end; ++i, tileData >>= 4) {
					unsigned p = tileData & 0xF;
					if (p) {
						_put(top, attrs, outX + i, palette[p], attr, kind, blda, bldb);
					}
				}
			}
		}
	} else {
		for (; outX < GBA_VIDEO_HORIZONTAL_PIXELS; outX += 8, ++tile) {
			uint16_t mapData = mapCache[tile & 0x3F];
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
			if (kind != PUT_MIX && outX >= 0 && outX <= GBA_VIDEO_HORIZONTAL_PIXELS - 8) {
				if (_opaque8(lo) && _opaque8(hi)) {
					color_t* t = &top[outX];
					t[0] = mainPalette[lo & 0xFF];
					t[1] = mainPalette[(lo >> 8) & 0xFF];
					t[2] = mainPalette[(lo >> 16) & 0xFF];
					t[3] = mainPalette[lo >> 24];
					t[4] = mainPalette[hi & 0xFF];
					t[5] = mainPalette[(hi >> 8) & 0xFF];
					t[6] = mainPalette[(hi >> 16) & 0xFF];
					t[7] = mainPalette[hi >> 24];
					if (kind == PUT_ATTR) {
						uint8_t* a = &attrs[outX];
						a[0] = attr; a[1] = attr; a[2] = attr; a[3] = attr;
						a[4] = attr; a[5] = attr; a[6] = attr; a[7] = attr;
					}
					continue;
				}
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
				int i = outX < 0 ? -outX : 0;
				int end = GBA_VIDEO_HORIZONTAL_PIXELS - outX < 8 ? GBA_VIDEO_HORIZONTAL_PIXELS - outX : 8;
				for (; i < end; ++i) {
					PUT_8BPP(outX + i, (i & 3) * 8, i < 4 ? lo : hi);
				}
			}
		}
	}
}

FAST_INLINE void _drawAffine(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, struct FastRow* row, color_t* mainPalette, unsigned attr, enum PutKind kind) {
	color_t* top = row->top;
	uint8_t* attrs = row->attr;
	int blda = row->blda;
	int bldb = row->bldb;
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
				_put(top, attrs, outX, mainPalette[p], attr, kind, blda, bldb);
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
				_put(top, attrs, outX, mainPalette[p], attr, kind, blda, bldb);
			}
		}
	}
}

FAST_INLINE void _drawSprites(struct GBAVideoSoftwareRenderer* renderer, struct FastRow* row, unsigned priority, enum PutKind kind) {
	color_t* top = row->top;
	uint8_t* attrs = row->attr;
	int blda = row->blda;
	int bldb = row->bldb;
	unsigned t2 = renderer->target2Obj ? ATTR_T2 : 0;
	int x;
	for (x = 0; x < GBA_VIDEO_HORIZONTAL_PIXELS; ++x) {
		uint32_t s = renderer->spriteLayer[x];
		if ((s & FLAG_UNWRITTEN) == FLAG_UNWRITTEN || (s >> OFFSET_PRIORITY) != priority) {
			continue;
		}
		unsigned attr = t2 | ((s & FLAG_TARGET_1) ? ATTR_T1 : 0);
		_put(top, attrs, x, (color_t) (s & 0x00FFFFFF), attr, kind, blda, bldb);
	}
}

static ATTRIBUTE_NOINLINE void _drawTextPlain(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y, struct FastRow* row, color_t* palette, unsigned attr) {
	_drawText(renderer, background, y, row, palette, attr, PUT_PLAIN);
}

static ATTRIBUTE_NOINLINE void _drawTextAttr(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y, struct FastRow* row, color_t* palette, unsigned attr) {
	_drawText(renderer, background, y, row, palette, attr, PUT_ATTR);
}

static ATTRIBUTE_NOINLINE void _drawTextMix(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y, struct FastRow* row, color_t* palette, unsigned attr) {
	_drawText(renderer, background, y, row, palette, attr, PUT_MIX);
}

#define FAST_KERNELS(SUFFIX, KIND) \
	static ATTRIBUTE_NOINLINE void _drawAffine ## SUFFIX(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, struct FastRow* row, color_t* palette, unsigned attr) { \
		_drawAffine(renderer, background, row, palette, attr, KIND); \
	} \
	static ATTRIBUTE_NOINLINE void _drawSprites ## SUFFIX(struct GBAVideoSoftwareRenderer* renderer, struct FastRow* row, unsigned priority) { \
		_drawSprites(renderer, row, priority, KIND); \
	}

FAST_KERNELS(Blend, PUT_ANY)
FAST_KERNELS(NoBlend, PUT_PLAIN)

static void _paint(struct GBAVideoSoftwareRenderer* renderer, int y, int spriteLayers, struct FastRow* row, bool blend) {
	int mode = GBARegisterDISPCNTGetMode(renderer->dispcnt);
	bool alpha = renderer->blendEffect == BLEND_ALPHA;
	bool variantFx = renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN;

	color_t backdrop = (renderer->target1Bd && variantFx) ? renderer->variantPalette[0] : renderer->normalPalette[0];
	int x;
#ifdef COLOR_16_BIT
	if (!((uintptr_t) row->top & 3)) {
		// Two pixels a store
		uint32_t pair = backdrop | ((uint32_t) backdrop << 16);
		uint32_t* out = (uint32_t*) row->top;
		for (x = 0; x < GBA_VIDEO_HORIZONTAL_PIXELS / 2; x += 4) {
			out[x] = pair;
			out[x + 1] = pair;
			out[x + 2] = pair;
			out[x + 3] = pair;
		}
	} else
#endif
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
				if (!blend) {
					_drawTextPlain(renderer, bg, y, row, palette, attr);
				} else if (attr & ATTR_T1) {
					_drawTextMix(renderer, bg, y, row, palette, attr);
				} else {
					_drawTextAttr(renderer, bg, y, row, palette, attr);
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
