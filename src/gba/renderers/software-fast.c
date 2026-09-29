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
	uint8_t* attr;
	int blda;
	int bldb;
	// 8 pixel blocks that layers in front already cover completely, a bit
	// each. The two past the end of the line are always set.
	uint32_t skip;
};
#define SKIP_NONE 0xC0000000

// The blocks a tile at x lies in, which can start 7 pixels left of the line
FAST_INLINE uint32_t _tileBlocks(int x) {
	return ((x & 7 ? 3U : 1U) << ((x + 8) >> 3)) >> 1;
}

// Target 1 BG layers are drawn plain into a scratch row first and then mixed
// into the output in one tight loop (_mixRow), which keeps the blend weights
// and masks in registers. Green's low bit is never set in a converted color,
// so it marks the pixels the layer left transparent.
#define MIX_EMPTY 0x0020

// How a kernel paints a pixel. The text kernels know the layer's attr up
// front, so they're specialized on it; affine BGs and sprites check it per
// pixel.
enum PutKind {
	PUT_PLAIN, // no blending on this line
	PUT_ATTR, // blending, but this layer isn't target 1: just record attr
	PUT_MIX, // this pixel is target 1: mix over target 2
	PUT_ANY, // attr decided per pixel
	PUT_UNDER, // no blending, and only where nothing's been painted yet
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
	if (kind == PUT_UNDER) {
		if (top[x] & MIX_EMPTY) {
			top[x] = color;
		}
		return;
	}
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
	uint32_t skip = row->skip;
	if (!background->multipalette) {
		for (; outX < GBA_VIDEO_HORIZONTAL_PIXELS; outX += 8, ++tile) {
			if (skip != SKIP_NONE) {
				uint32_t blocks = _tileBlocks(outX);
				if ((skip & blocks) == blocks) {
					continue;
				}
			}
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
				if (kind != PUT_UNDER && _opaque4(tileData)) {
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
			if (skip != SKIP_NONE) {
				uint32_t blocks = _tileBlocks(outX);
				if ((skip & blocks) == blocks) {
					continue;
				}
			}
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
			if (outX >= 0 && outX <= GBA_VIDEO_HORIZONTAL_PIXELS - 8) {
				if (kind != PUT_UNDER && _opaque8(lo) && _opaque8(hi)) {
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
	uint32_t skip = row->skip;
	if (skip != SKIP_NONE) {
		// Same as below, a block at a time
		bool overflow = background->overflow;
		for (outX = 0; outX < GBA_VIDEO_HORIZONTAL_PIXELS; skip >>= 1) {
			if (skip & 1) {
				outX += 8;
				x += (uint32_t) dx * 8;
				y += (uint32_t) dy * 8;
				continue;
			}
			int end = outX + 8;
			for (; outX < end; ++outX, x += dx, y += dy) {
				if (kind == PUT_UNDER && !(top[outX] & MIX_EMPTY)) {
					continue;
				}
				int32_t localX = x;
				int32_t localY = y;
				if (overflow) {
					localX &= mask;
					localY &= mask;
				} else if ((x | y) & ~mask) {
					continue;
				}
				unsigned mapData = screenBase[(localX >> 11) + (((localY >> 7) & 0x7F0) << size)];
				unsigned p = charBase[(mapData << 6) + ((localY & 0x700) >> 5) + ((localX & 0x700) >> 8)];
				if (p) {
					_put(top, attrs, outX, mainPalette[p], attr, kind, blda, bldb);
				}
			}
		}
		return;
	}
	if (background->overflow) {
		for (outX = 0; outX < GBA_VIDEO_HORIZONTAL_PIXELS; ++outX, x += dx, y += dy) {
			if (kind == PUT_UNDER && !(top[outX] & MIX_EMPTY)) {
				continue;
			}
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
			if (kind == PUT_UNDER && !(top[outX] & MIX_EMPTY)) {
				continue;
			}
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
	const uint32_t* layer = renderer->spriteLayer;
	uint32_t blocks = renderer->spritePriorityBlocks[priority] & ~row->skip;
	int x;
	for (x = 0; blocks; blocks >>= 1, x += 8) {
		if (!(blocks & 1)) {
			continue;
		}
		int end = x + 8;
		int i;
		for (i = x; i < end; ++i) {
			uint32_t s = layer[i];
			if ((s & FLAG_UNWRITTEN) == FLAG_UNWRITTEN || (s >> OFFSET_PRIORITY) != priority) {
				continue;
			}
			unsigned attr = t2 | ((s & FLAG_TARGET_1) ? ATTR_T1 : 0);
			_put(top, attrs, i, (color_t) (s & 0x00FFFFFF), attr, kind, blda, bldb);
		}
	}
}

static ATTRIBUTE_NOINLINE void _drawTextPlain(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y, struct FastRow* row, color_t* palette, unsigned attr) {
	_drawText(renderer, background, y, row, palette, attr, PUT_PLAIN);
}

static ATTRIBUTE_NOINLINE void _drawTextAttr(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y, struct FastRow* row, color_t* palette, unsigned attr) {
	_drawText(renderer, background, y, row, palette, attr, PUT_ATTR);
}

static ATTRIBUTE_NOINLINE void _drawAffinePlain(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, struct FastRow* row, color_t* palette, unsigned attr) {
	_drawAffine(renderer, background, row, palette, attr, PUT_PLAIN);
}

static ATTRIBUTE_NOINLINE void _drawAffineAttr(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, struct FastRow* row, color_t* palette, unsigned attr) {
	_drawAffine(renderer, background, row, palette, attr, PUT_ATTR);
}

static ATTRIBUTE_NOINLINE void _drawTextUnder(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y, struct FastRow* row, color_t* palette, unsigned attr) {
	_drawText(renderer, background, y, row, palette, attr, PUT_UNDER);
}

static ATTRIBUTE_NOINLINE void _drawAffineUnder(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, struct FastRow* row, color_t* palette, unsigned attr) {
	_drawAffine(renderer, background, row, palette, attr, PUT_UNDER);
}

static ATTRIBUTE_NOINLINE void _drawSpritesUnder(struct GBAVideoSoftwareRenderer* renderer, struct FastRow* row, unsigned priority) {
	_drawSprites(renderer, row, priority, PUT_UNDER);
}

static ATTRIBUTE_NOINLINE void _drawSpritesBlend(struct GBAVideoSoftwareRenderer* renderer, struct FastRow* row, unsigned priority) {
	_drawSprites(renderer, row, priority, PUT_ANY);
}

static ATTRIBUTE_NOINLINE void _drawSpritesNoBlend(struct GBAVideoSoftwareRenderer* renderer, struct FastRow* row, unsigned priority) {
	_drawSprites(renderer, row, priority, PUT_PLAIN);
}

// Sprite kernels. They leave the sprite layer exactly as
// GBAVideoSoftwareRendererPreprocessSprite does, for the sprites the fast
// path sees (no windows, OBJ window, mosaic or highlight), but read a tile row
// at a time and skip the blank ones, and don't look at what's in the layer
// where nothing has been drawn yet.
#define SPRITE_REFLAG (FLAG_ORDER_MASK | FLAG_REBLEND | FLAG_TARGET_1)

#define FAST_SPRITE_8BPP 0x01
#define FAST_SPRITE_AFFINE 0x02
#define FAST_SPRITE_HFLIP 0x04
#define FAST_SPRITE_VFLIP 0x08
#define FAST_SPRITE_FORCE 0x10
// Counts as drawn, but has nothing on screen
#define FAST_SPRITE_EMPTY 0x20
// Not drawn at all
#define FAST_SPRITE_NONE 0x40
// One for GBAVideoSoftwareRendererPreprocessSprite
#define FAST_SPRITE_SLOW 0x80

FAST_INLINE uint32_t _spriteBlocks(int x, int end) {
	return (2U << ((end - 1) >> 3)) - (1U << (x >> 3));
}

FAST_INLINE void _spritePixel(uint32_t* pixel, unsigned p, const color_t* palette, uint32_t flags) {
	uint32_t current = *pixel;
	if ((current & FLAG_ORDER_MASK) > flags) {
		if (p) {
			*pixel = palette[p] | flags;
		} else if (current != FLAG_UNWRITTEN) {
			*pixel = (current & ~SPRITE_REFLAG) | (flags & SPRITE_REFLAG);
		}
	}
}

// 8 pixels of a 16 color tile row or 4 of a 256 color one, first pixel lowest
FAST_INLINE uint32_t _spriteWord(const uint16_t* vramBase, unsigned yBase, unsigned charBase, unsigned maskLo, int localX, bool bpp8) {
	unsigned xBase = bpp8 ? (localX & ~0x7) * 8 + (localX & 4) : (localX & ~0x7) * 4;
	uint32_t lo;
	uint32_t hi;
	LOAD_16(lo, (yBase + ((xBase + charBase) & maskLo)) & 0x7FFE, vramBase);
	LOAD_16(hi, (yBase + ((xBase + 2 + charBase) & maskLo)) & 0x7FFE, vramBase);
	return lo | (hi << 16);
}

// Returns the blocks it may have changed. Flipped sprites get their tile rows
// reversed, so there's only the one direction to walk.
FAST_INLINE uint32_t _spriteNormal(struct GBAVideoSoftwareRenderer* renderer, const struct GBAVideoSoftwareFastSprite* sprite, unsigned yBase, uint32_t blocks, bool bpp8) {
	const int group = bpp8 ? 4 : 8;
	const int bits = bpp8 ? 8 : 4;
	uint32_t* layer = renderer->spriteLayer;
	const uint16_t* vramBase = &renderer->d.vram[BASE_TILE >> 1];
	const color_t* palette = sprite->palette;
	uint32_t flags = sprite->flags;
	unsigned charBase = sprite->charBase;
	unsigned maskLo = sprite->maskLo;
	int outX = sprite->outX;
	int condition = sprite->condition;
	int inX = sprite->inX;
	bool flip = sprite->kind & FAST_SPRITE_HFLIP;
	uint32_t touched = 0;
	while (outX < condition) {
		uint32_t word = _spriteWord(vramBase, yBase, charBase, maskLo, inX, bpp8);
		int sub = inX & (group - 1);
		if (flip) {
			word = __builtin_bswap32(word);
			if (!bpp8) {
				word = ((word & 0x0F0F0F0F) << 4) | ((word >> 4) & 0x0F0F0F0F);
			}
			sub = group - 1 - sub;
		}
		int n = group - sub;
		if (n > condition - outX) {
			n = condition - outX;
		}
		word >>= sub * bits;
		uint32_t here = _spriteBlocks(outX, outX + n);
		uint32_t* pixel = &layer[outX];
		outX += n;
		inX += flip ? -n : n;
		if (!(blocks & here)) {
			if (!word) {
				continue;
			}
			// Nothing here yet, so every pixel is unwritten
			for (; n; --n, ++pixel) {
				unsigned p = word & ((1 << bits) - 1);
				word >>= bits;
				if (p) {
					*pixel = palette[p] | flags;
				}
			}
		} else {
			for (; n; --n, ++pixel) {
				unsigned p = word & ((1 << bits) - 1);
				word >>= bits;
				_spritePixel(pixel, p, palette, flags);
			}
		}
		blocks |= here;
		touched |= here;
	}
	return touched;
}

FAST_INLINE void _spriteAffine(struct GBAVideoSoftwareRenderer* renderer, const struct GBAVideoSoftwareFastSprite* sprite, int xAccum, int yAccum, int outX, bool clear, bool bpp8) {
	uint32_t* layer = renderer->spriteLayer;
	const uint16_t* vramBase = &renderer->d.vram[BASE_TILE >> 1];
	const color_t* palette = sprite->palette;
	uint32_t flags = sprite->flags;
	unsigned charBase = sprite->charBase;
	unsigned maskLo = sprite->maskLo;
	unsigned maskHi = sprite->maskHi;
	unsigned strideShift = sprite->strideShift;
	int condition = sprite->condition;
	int dx = sprite->a;
	int dy = sprite->c;
	unsigned widthMask = ~(sprite->width - 1);
	unsigned heightMask = ~(sprite->height - 1);
	for (; outX < condition; ++outX) {
		xAccum += dx;
		yAccum += dy;
		int localX = xAccum >> 8;
		int localY = yAccum >> 8;
		if ((localX & widthMask) | (localY & heightMask)) {
			break;
		}
		unsigned p;
		if (bpp8) {
			unsigned yBase = ((localY & ~0x7) << strideShift) + (localY & 0x7) * 8 + maskHi;
			unsigned xBase = (localX & ~0x7) * 8 + (localX & 6);
			LOAD_16(p, (yBase + ((xBase + charBase) & maskLo)) & 0x7FFE, vramBase);
			p = (p >> ((localX & 1) << 3)) & 0xFF;
		} else {
			unsigned yBase = ((localY & ~0x7) << strideShift) + (localY & 0x7) * 4 + maskHi;
			unsigned xBase = (localX & ~0x7) * 4 + ((localX >> 1) & 2);
			LOAD_16(p, (yBase + ((xBase + charBase) & maskLo)) & 0x7FFE, vramBase);
			p = (p >> ((localX & 3) << 2)) & 0xF;
		}
		if (!clear) {
			_spritePixel(&layer[outX], p, palette, flags);
		} else if (p) {
			layer[outX] = palette[p] | flags;
		}
	}
}

ATTRIBUTE_HOT_GROUP(2) static ATTRIBUTE_NOINLINE uint32_t _spriteNormal16(struct GBAVideoSoftwareRenderer* renderer, const struct GBAVideoSoftwareFastSprite* sprite, unsigned yBase, uint32_t blocks) {
	return _spriteNormal(renderer, sprite, yBase, blocks, false);
}

ATTRIBUTE_HOT_GROUP(2) static ATTRIBUTE_NOINLINE uint32_t _spriteNormal256(struct GBAVideoSoftwareRenderer* renderer, const struct GBAVideoSoftwareFastSprite* sprite, unsigned yBase, uint32_t blocks) {
	return _spriteNormal(renderer, sprite, yBase, blocks, true);
}

ATTRIBUTE_HOT_GROUP(2) static ATTRIBUTE_NOINLINE void _spriteAffine16(struct GBAVideoSoftwareRenderer* renderer, const struct GBAVideoSoftwareFastSprite* sprite, int xAccum, int yAccum, int outX, bool clear) {
	if (clear) {
		_spriteAffine(renderer, sprite, xAccum, yAccum, outX, true, false);
	} else {
		_spriteAffine(renderer, sprite, xAccum, yAccum, outX, false, false);
	}
}

ATTRIBUTE_HOT_GROUP(2) static ATTRIBUTE_NOINLINE void _spriteAffine256(struct GBAVideoSoftwareRenderer* renderer, const struct GBAVideoSoftwareFastSprite* sprite, int xAccum, int yAccum, int outX, bool clear) {
	if (clear) {
		_spriteAffine(renderer, sprite, xAccum, yAccum, outX, true, true);
	} else {
		_spriteAffine(renderer, sprite, xAccum, yAccum, outX, false, true);
	}
}

#ifdef COLOR_16_BIT
// The same sprites straight into the output row. Under: only where the row
// is still empty, for the front to back pass.
FAST_INLINE void _directNormal(struct GBAVideoSoftwareRenderer* renderer, const struct GBAVideoSoftwareFastSprite* sprite, unsigned yBase, color_t* top, uint32_t skip, bool under, bool bpp8) {
	const int group = bpp8 ? 4 : 8;
	const int bits = bpp8 ? 8 : 4;
	const uint16_t* vramBase = &renderer->d.vram[BASE_TILE >> 1];
	const color_t* palette = sprite->palette;
	unsigned charBase = sprite->charBase;
	unsigned maskLo = sprite->maskLo;
	int outX = sprite->outX;
	int condition = sprite->condition;
	int inX = sprite->inX;
	bool flip = sprite->kind & FAST_SPRITE_HFLIP;
	while (outX < condition) {
		uint32_t word = _spriteWord(vramBase, yBase, charBase, maskLo, inX, bpp8);
		int sub = inX & (group - 1);
		if (flip) {
			word = __builtin_bswap32(word);
			if (!bpp8) {
				word = ((word & 0x0F0F0F0F) << 4) | ((word >> 4) & 0x0F0F0F0F);
			}
			sub = group - 1 - sub;
		}
		int n = group - sub;
		if (n > condition - outX) {
			n = condition - outX;
		}
		word >>= sub * bits;
		color_t* pixel = &top[outX];
		uint32_t here = _spriteBlocks(outX, outX + n);
		outX += n;
		inX += flip ? -n : n;
		if (!word || (under && !(here & ~skip))) {
			continue;
		}
		for (; n; --n, ++pixel) {
			unsigned p = word & ((1 << bits) - 1);
			word >>= bits;
			if (p && (!under || (*pixel & MIX_EMPTY))) {
				*pixel = palette[p];
			}
		}
	}
}

FAST_INLINE void _directAffine(struct GBAVideoSoftwareRenderer* renderer, const struct GBAVideoSoftwareFastSprite* sprite, int xAccum, int yAccum, int outX, color_t* top, bool under, bool bpp8) {
	const uint16_t* vramBase = &renderer->d.vram[BASE_TILE >> 1];
	const color_t* palette = sprite->palette;
	unsigned charBase = sprite->charBase;
	unsigned maskLo = sprite->maskLo;
	unsigned maskHi = sprite->maskHi;
	unsigned strideShift = sprite->strideShift;
	int condition = sprite->condition;
	int dx = sprite->a;
	int dy = sprite->c;
	unsigned widthMask = ~(sprite->width - 1);
	unsigned heightMask = ~(sprite->height - 1);
	for (; outX < condition; ++outX) {
		xAccum += dx;
		yAccum += dy;
		int localX = xAccum >> 8;
		int localY = yAccum >> 8;
		if ((localX & widthMask) | (localY & heightMask)) {
			break;
		}
		if (under && !(top[outX] & MIX_EMPTY)) {
			continue;
		}
		unsigned p;
		if (bpp8) {
			unsigned yBase = ((localY & ~0x7) << strideShift) + (localY & 0x7) * 8 + maskHi;
			unsigned xBase = (localX & ~0x7) * 8 + (localX & 6);
			LOAD_16(p, (yBase + ((xBase + charBase) & maskLo)) & 0x7FFE, vramBase);
			p = (p >> ((localX & 1) << 3)) & 0xFF;
		} else {
			unsigned yBase = ((localY & ~0x7) << strideShift) + (localY & 0x7) * 4 + maskHi;
			unsigned xBase = (localX & ~0x7) * 4 + ((localX >> 1) & 2);
			LOAD_16(p, (yBase + ((xBase + charBase) & maskLo)) & 0x7FFE, vramBase);
			p = (p >> ((localX & 3) << 2)) & 0xF;
		}
		if (p) {
			top[outX] = palette[p];
		}
	}
}

FAST_INLINE void _directSprite(struct GBAVideoSoftwareRenderer* renderer, const struct GBAVideoSoftwareFastLine* line, color_t* top, uint32_t skip, bool under) {
	const struct GBAVideoSoftwareFastSprite* sprite = &renderer->fastSprites[line->index];
	unsigned kind = sprite->kind;
	if (kind & FAST_SPRITE_AFFINE) {
		if (kind & FAST_SPRITE_8BPP) {
			_directAffine(renderer, sprite, line->xAccum, line->yAccum, line->outX, top, under, true);
		} else {
			_directAffine(renderer, sprite, line->xAccum, line->yAccum, line->outX, top, under, false);
		}
	} else if (kind & FAST_SPRITE_8BPP) {
		_directNormal(renderer, sprite, line->xAccum, top, skip, under, true);
	} else {
		_directNormal(renderer, sprite, line->xAccum, top, skip, under, false);
	}
}

// Back to front: the first sprite in OAM goes last
static ATTRIBUTE_NOINLINE void _directSpritesOver(struct GBAVideoSoftwareRenderer* renderer, color_t* top, unsigned priority) {
	const struct GBAVideoSoftwareFastLine* first = renderer->fastLine;
	const struct GBAVideoSoftwareFastLine* line = &first[renderer->nFastLine];
	while (line > first) {
		--line;
		if (line->priority == priority) {
			_directSprite(renderer, line, top, 0, false);
		}
	}
}

static ATTRIBUTE_NOINLINE void _directSpritesUnder(struct GBAVideoSoftwareRenderer* renderer, color_t* top, unsigned priority, uint32_t skip) {
	const struct GBAVideoSoftwareFastLine* line = renderer->fastLine;
	const struct GBAVideoSoftwareFastLine* last = &line[renderer->nFastLine];
	for (; line < last; ++line) {
		if (line->priority == priority) {
			_directSprite(renderer, line, top, skip, true);
		}
	}
}
#endif

// What GBAVideoSoftwareRendererPreprocessSprite works out from the registers
// for every sprite
FAST_INLINE void _spriteKey(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareFastSpriteKey* key) {
	bool blendEnable = GBAWindowControlIsBlendEnable(renderer->currentWindow.packed);
	bool alpha = renderer->target1Obj && renderer->blendEffect == BLEND_ALPHA;
	int target2 = renderer->target2Bd;
	target2 |= renderer->bg[0].target2 && renderer->bg[0].enabled;
	target2 |= renderer->bg[1].target2 && renderer->bg[1].enabled;
	target2 |= renderer->bg[2].target2 && renderer->bg[2].enabled;
	target2 |= renderer->bg[3].target2 && renderer->bg[3].enabled;
	bool variant = renderer->target1Obj && blendEnable && (renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN);
	key->mapping = GBARegisterDISPCNTIsObjCharacterMapping(renderer->dispcnt);
	key->bitmap = GBARegisterDISPCNTGetMode(renderer->dispcnt) >= 3;
	key->offsetX = renderer->objOffsetX;
	key->offsetY = renderer->objOffsetY;
	// Indexed by whether the sprite is semitransparent
	int semi;
	for (semi = 0; semi < 2; ++semi) {
		uint32_t flags = FLAG_TARGET_1 * ((blendEnable && alpha) || semi);
		bool force = false;
		bool useVariant = variant;
		if (semi || alpha) {
			if (target2) {
				force = true;
				flags |= FLAG_REBLEND;
				useVariant = false;
			} else {
				flags &= ~FLAG_TARGET_1;
			}
		}
		key->flags[semi] = flags;
		key->force[semi] = force;
		key->palette[semi] = useVariant ? &renderer->variantPalette[0x100] : &renderer->normalPalette[0x100];
	}
}

FAST_INLINE bool _spriteKeyMatches(const struct GBAVideoSoftwareFastSpriteKey* a, const struct GBAVideoSoftwareFastSpriteKey* b) {
	return a->palette[0] == b->palette[0] && a->palette[1] == b->palette[1] &&
	       a->flags[0] == b->flags[0] && a->flags[1] == b->flags[1] &&
	       a->offsetX == b->offsetX && a->offsetY == b->offsetY &&
	       a->force[0] == b->force[0] && a->force[1] == b->force[1] &&
	       a->mapping == b->mapping && a->bitmap == b->bitmap;
}

// The part of GBAVideoSoftwareRendererPreprocessSprite that's the same on
// every line of a sprite, for a line that's one window wide
static ATTRIBUTE_NOINLINE void _prepareSprites(struct GBAVideoSoftwareRenderer* renderer, const struct GBAVideoSoftwareFastSpriteKey* key) {
	const int start = 0;
	const int end = GBA_VIDEO_HORIZONTAL_PIXELS;
	bool mapping = key->mapping;
	int i;
	for (i = 0; i < renderer->oamMax; ++i) {
		const struct GBAObj* sprite = &renderer->sprites[i].obj;
		struct GBAVideoSoftwareFastSprite* fast = &renderer->fastSprites[i];
		unsigned size = GBAObjAttributesAGetShape(sprite->a) * 4 + GBAObjAttributesBGetSize(sprite->b);
		int width = GBAVideoObjSizes[size][0];
		int height = GBAVideoObjSizes[size][1];
		unsigned priority = GBAObjAttributesCGetPriority(sprite->c);
		unsigned semi = GBAObjAttributesAGetMode(sprite->a) == OBJ_MODE_SEMITRANSPARENT;
		bool bpp8 = GBAObjAttributesAIs256Color(sprite->a);
		int32_t x = (uint32_t) GBAObjAttributesBGetX(sprite->b) << 23;
		x >>= 23;
		x += key->offsetX;
		unsigned align = bpp8 && !mapping;
		unsigned charBase = (GBAObjAttributesCGetTile(sprite->c) & ~align) * 0x20;
		int stride = mapping ? (width >> !bpp8) : 0x80;

		fast->priority = priority;
		fast->cycles = renderer->sprites[i].cycles;
		if (GBAObjAttributesAIsMosaic(sprite->a) || GBAObjAttributesAGetMode(sprite->a) == OBJ_MODE_OBJWIN) {
			fast->kind = FAST_SPRITE_SLOW;
			continue;
		}
		if (key->bitmap && GBAObjAttributesCGetTile(sprite->c) < 512) {
			fast->kind = FAST_SPRITE_NONE;
			continue;
		}
		unsigned kind = bpp8 ? FAST_SPRITE_8BPP : 0;
		if (key->force[semi]) {
			kind |= FAST_SPRITE_FORCE;
		}
		fast->flags = key->flags[semi] | (priority << OFFSET_PRIORITY);
		fast->palette = key->palette[semi];
		if (!bpp8) {
			fast->palette = &fast->palette[GBAObjAttributesCGetPalette(sprite->c) << 4];
		}
		fast->charBase = charBase;
		fast->maskLo = mapping ? 0x7FFE : 0x3FE;
		fast->maskHi = mapping ? 0 : charBase & 0x7C00;
		fast->strideShift = __builtin_ctz(stride);
		fast->width = width;
		fast->height = height;
		fast->y = (int) GBAObjAttributesAGetY(sprite->a) + key->offsetY;

		int outX = x >= start ? x : start;
		int inX = outX - x;
		int condition;
		if (GBAObjAttributesAIsTransformed(sprite->a)) {
			int totalWidth = width << GBAObjAttributesAGetDoubleSize(sprite->a);
			int totalHeight = height << GBAObjAttributesAGetDoubleSize(sprite->a);
			struct GBAOAMMatrix mat;
			LOAD_16(mat.a, 0, &renderer->d.oam->mat[GBAObjAttributesBGetMatIndex(sprite->b)].a);
			LOAD_16(mat.b, 0, &renderer->d.oam->mat[GBAObjAttributesBGetMatIndex(sprite->b)].b);
			LOAD_16(mat.c, 0, &renderer->d.oam->mat[GBAObjAttributesBGetMatIndex(sprite->b)].c);
			LOAD_16(mat.d, 0, &renderer->d.oam->mat[GBAObjAttributesBGetMatIndex(sprite->b)].d);
			kind |= FAST_SPRITE_AFFINE;
			condition = x + totalWidth;
			fast->a = mat.a;
			fast->b = mat.b;
			fast->c = mat.c;
			fast->d = mat.d;
			// Line inY adds mat.b * inY and mat.d * inY to these
			fast->xAccum = mat.a * (inX - 1 - (totalWidth >> 1)) - mat.b * (totalHeight >> 1) + (width << 7);
			fast->yAccum = mat.c * (inX - 1 - (totalWidth >> 1)) - mat.d * (totalHeight >> 1) + (height << 7);
		} else {
			condition = x + width;
			if ((int) GBAObjAttributesAGetY(sprite->a) + height - 256 >= 0) {
				fast->y -= 256;
			}
			if (GBAObjAttributesBIsVFlip(sprite->b)) {
				kind |= FAST_SPRITE_VFLIP;
			}
			if (GBAObjAttributesBIsHFlip(sprite->b)) {
				inX = width - inX - 1;
				kind |= FAST_SPRITE_HFLIP;
			}
		}
		if (end < condition) {
			condition = end;
		}
		if (!(kind & FAST_SPRITE_AFFINE) && outX >= condition) {
			kind |= FAST_SPRITE_EMPTY;
		}
		fast->outX = outX;
		fast->condition = condition;
		fast->inX = inX;
		fast->kind = kind;
	}
	renderer->fastSpriteKey = *key;
	renderer->fastSpritesValid = true;
}

static ATTRIBUTE_NOINLINE int _slowSprite(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoRendererSprite* sprite, int y) {
	int mosaicV = GBAMosaicControlGetObjV(renderer->mosaic) + 1;
	if (GBAObjAttributesAIsMosaic(sprite->obj.a) && mosaicV > 1) {
		y -= y % mosaicV;
		if (y < sprite->y && sprite->y < GBA_VIDEO_VERTICAL_PIXELS) {
			y = sprite->y;
		}
		if (y >= (sprite->endY & 0xFF)) {
			y = sprite->endY - 1;
		}
	}
	return GBAVideoSoftwareRendererPreprocessSprite(renderer, &sprite->obj, sprite->index, y);
}

// The affine sprite's accumulators and first pixel on this line. Returns
// false if there's nothing of it to draw.
FAST_INLINE bool _spriteAffineLine(const struct GBAVideoSoftwareFastSprite* sprite, int y, int* outXOut, int* xAccumOut, int* yAccumOut) {
	int inY = y - sprite->y;
	if (inY < 0) {
		inY += 256;
	}
	int a = sprite->a;
	int c = sprite->c;
	int width = sprite->width;
	int height = sprite->height;
	int outX = sprite->outX;
	int xAccum = sprite->xAccum + sprite->b * inY;
	int yAccum = sprite->yAccum + sprite->d * inY;

	// Clip off early pixels
	if (a) {
		if ((xAccum >> 8) < 0) {
			int32_t diffX = -xAccum - 1;
			int32_t skip = diffX / a;
			xAccum += a * skip;
			yAccum += c * skip;
			outX += skip;
		} else if ((xAccum >> 8) >= width) {
			int32_t diffX = (width << 8) - xAccum;
			int32_t skip = diffX / a;
			xAccum += a * skip;
			yAccum += c * skip;
			outX += skip;
		}
	}
	if (c) {
		if ((yAccum >> 8) < 0) {
			int32_t diffY = -yAccum - 1;
			int32_t skip = diffY / c;
			xAccum += a * skip;
			yAccum += c * skip;
			outX += skip;
		} else if ((yAccum >> 8) >= height) {
			int32_t diffY = (height << 8) - yAccum;
			int32_t skip = diffY / c;
			xAccum += a * skip;
			yAccum += c * skip;
			outX += skip;
		}
	}
	*outXOut = outX;
	*xAccumOut = xAccum;
	*yAccumOut = yAccum;
	return outX >= 0 && outX < sprite->condition;
}

// Whether a BG is one of the line's layers
FAST_INLINE bool _bgListed(const struct GBAVideoSoftwareRenderer* renderer, int mode, int i, bool* affine) {
	const struct GBAVideoSoftwareBackground* bg = &renderer->bg[i];
	if (bg->enabled != ENABLED_MAX || renderer->d.disableBG[i]) {
		return false;
	}
	if (mode == 0) {
		*affine = false;
	} else if (mode == 1) {
		if (i == 3) {
			return false;
		}
		*affine = i == 2;
	} else {
		if (i < 2) {
			return false;
		}
		*affine = true;
	}
	return true;
}

// Whether the fast path draws this line, and if it has to blend
FAST_INLINE bool _plan(const struct GBAVideoSoftwareRenderer* renderer, bool forceTarget1, bool* blendOut) {
	bool variantFx = renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN;
	if (forceTarget1 && variantFx) {
		return false;
	}
	bool alpha = renderer->blendEffect == BLEND_ALPHA;
	bool blend = forceTarget1;
	if (alpha || forceTarget1) {
		// Sprites can be target 1 through alpha or semi-transparency
		bool objT1 = forceTarget1 || (alpha && renderer->target1Obj);
		if (objT1 && renderer->target2Obj) {
			return false;
		}
		blend = blend || objT1;
		int i;
		for (i = 0; i < 4; ++i) {
			const struct GBAVideoSoftwareBackground* bg = &renderer->bg[i];
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
	*blendOut = blend;
	return true;
}

#ifdef COLOR_16_BIT
// Sprites can go straight into the row when drawing them a priority at a
// time gives what the sprite layer would: no sprite in front of one that's
// earlier in OAM, none that blend, and none behind a layer that does.
static ATTRIBUTE_NOINLINE bool _spritesDirect(struct GBAVideoSoftwareRenderer* renderer, int y, int* spriteLayersOut) {
	bool blend;
	if (!_plan(renderer, false, &blend)) {
		return false;
	}
	int frontT1 = 3;
	if (blend) {
		if (renderer->target1Obj || ((uintptr_t) &renderer->outputBuffer[renderer->outputBufferStride * y] & 3)) {
			return false;
		}
		int mode = GBARegisterDISPCNTGetMode(renderer->dispcnt);
		frontT1 = -1;
		int i;
		for (i = 0; i < 4; ++i) {
			bool affine;
			if (!_bgListed(renderer, mode, i, &affine) || !renderer->bg[i].target1) {
				continue;
			}
			if (frontT1 < 0 || (int) renderer->bg[i].priority < frontT1) {
				frontT1 = renderer->bg[i].priority;
			}
		}
		if (frontT1 < 0) {
			return false;
		}
	}

	int spriteLayers = 0;
	uint32_t priorityBlocks[4] = { 0, 0, 0, 0 };
	int cycles = renderer->spriteCyclesRemaining;
	struct GBAVideoSoftwareFastLine* out = renderer->fastLine;
	const uint8_t* line = &renderer->spriteLines[renderer->spriteLineStart[y]];
	const uint8_t* last = &renderer->spriteLines[renderer->spriteLineStart[y + 1]];
	for (; line < last; ++line) {
		const struct GBAVideoSoftwareFastSprite* sprite = &renderer->fastSprites[*line];
		unsigned kind = sprite->kind;
		unsigned priority = sprite->priority;
		if (kind & (FAST_SPRITE_SLOW | FAST_SPRITE_FORCE)) {
			return false;
		}
		if (!(kind & FAST_SPRITE_NONE)) {
			if (sprite->flags & FLAG_TARGET_1) {
				return false;
			}
			uint32_t here = 0;
			if (kind & FAST_SPRITE_EMPTY) {
				spriteLayers |= 1 << priority;
			} else if (kind & FAST_SPRITE_AFFINE) {
				int outX;
				int xAccum;
				int yAccum;
				if (_spriteAffineLine(sprite, y, &outX, &xAccum, &yAccum)) {
					here = _spriteBlocks(outX, sprite->condition);
					out->xAccum = xAccum;
					out->yAccum = yAccum;
					out->outX = outX;
				}
			} else {
				int inY = y - sprite->y;
				if (kind & FAST_SPRITE_VFLIP) {
					inY = sprite->height - inY - 1;
				}
				out->xAccum = ((inY & ~0x7) << sprite->strideShift) + (inY & 0x7) * (kind & FAST_SPRITE_8BPP ? 8 : 4) + sprite->maskHi;
				here = _spriteBlocks(sprite->outX, sprite->condition);
			}
			if (here) {
				uint32_t behind = priorityBlocks[3];
				if (priority < 2) {
					behind |= priorityBlocks[2];
				}
				if (priority < 1) {
					behind |= priorityBlocks[1];
				}
				if (here & behind) {
					return false;
				}
				priorityBlocks[priority] |= here;
				spriteLayers |= 1 << priority;
				out->index = *line;
				out->priority = priority;
				++out;
			}
		}
		cycles -= sprite->cycles;
		if (cycles <= 0) {
			break;
		}
	}
	if (spriteLayers >> (frontT1 + 1)) {
		return false;
	}
	renderer->spriteCyclesRemaining = cycles;
	renderer->nFastLine = out - renderer->fastLine;
	renderer->fastSpritesDirect = true;
	renderer->spriteBlocks = 0;
	renderer->spriteLayerDirty = 0;
	*spriteLayersOut = spriteLayers;
	return true;
}
#endif

// GBAVideoSoftwareRendererPreprocessSpriteLayer for a line the fast path may
// take
ATTRIBUTE_HOT_GROUP(2) ATTRIBUTE_NOINLINE int GBAVideoSoftwareRendererFastSpriteLayer(struct GBAVideoSoftwareRenderer* renderer, int y) {
	// One window, the whole line
	renderer->currentWindow = renderer->windows[0].control;
	renderer->start = 0;
	renderer->end = renderer->windows[0].endX;

	struct GBAVideoSoftwareFastSpriteKey key;
	_spriteKey(renderer, &key);
	if (UNLIKELY(!renderer->fastSpritesValid || !_spriteKeyMatches(&key, &renderer->fastSpriteKey))) {
		_prepareSprites(renderer, &key);
	}

	int spriteLayers = 0;
#ifdef COLOR_16_BIT
	if (_spritesDirect(renderer, y, &spriteLayers)) {
		return spriteLayers;
	}
#endif
	uint32_t blocks = 0;
	uint32_t priorityBlocks[4] = { 0, 0, 0, 0 };
	int cycles = renderer->spriteCyclesRemaining;
	const uint8_t* line = &renderer->spriteLines[renderer->spriteLineStart[y]];
	const uint8_t* last = &renderer->spriteLines[renderer->spriteLineStart[y + 1]];
	for (; line < last; ++line) {
		const struct GBAVideoSoftwareFastSprite* sprite = &renderer->fastSprites[*line];
		unsigned kind = sprite->kind;
		unsigned priority = sprite->priority;
		if (UNLIKELY(kind & (FAST_SPRITE_SLOW | FAST_SPRITE_NONE | FAST_SPRITE_EMPTY | FAST_SPRITE_FORCE))) {
			if (kind & FAST_SPRITE_SLOW) {
				renderer->spriteCyclesRemaining = cycles;
				int drawn = _slowSprite(renderer, &renderer->sprites[*line], y);
				spriteLayers |= drawn << priority;
				blocks = 0xFFFFFFFF;
				priorityBlocks[0] = 0xFFFFFFFF;
				priorityBlocks[1] = 0xFFFFFFFF;
				priorityBlocks[2] = 0xFFFFFFFF;
				priorityBlocks[3] = 0xFFFFFFFF;
				goto next;
			}
			if (kind & FAST_SPRITE_NONE) {
				goto next;
			}
			if (kind & FAST_SPRITE_FORCE) {
				renderer->forceTarget1 = true;
			}
			if (kind & FAST_SPRITE_EMPTY) {
				spriteLayers |= 1 << priority;
				goto next;
			}
		}
		if (kind & FAST_SPRITE_AFFINE) {
			int outX;
			int xAccum;
			int yAccum;
			if (_spriteAffineLine(sprite, y, &outX, &xAccum, &yAccum)) {
				uint32_t here = _spriteBlocks(outX, sprite->condition);
				bool clear = !(blocks & here);
				blocks |= here;
				priorityBlocks[priority] |= here;
				if (kind & FAST_SPRITE_8BPP) {
					_spriteAffine256(renderer, sprite, xAccum, yAccum, outX, clear);
				} else {
					_spriteAffine16(renderer, sprite, xAccum, yAccum, outX, clear);
				}
				spriteLayers |= 1 << priority;
			}
		} else {
			int inY = y - sprite->y;
			if (kind & FAST_SPRITE_VFLIP) {
				inY = sprite->height - inY - 1;
			}
			uint32_t touched;
			if (kind & FAST_SPRITE_8BPP) {
				unsigned yBase = ((inY & ~0x7) << sprite->strideShift) + (inY & 0x7) * 8 + sprite->maskHi;
				touched = _spriteNormal256(renderer, sprite, yBase, blocks);
			} else {
				unsigned yBase = ((inY & ~0x7) << sprite->strideShift) + (inY & 0x7) * 4 + sprite->maskHi;
				touched = _spriteNormal16(renderer, sprite, yBase, blocks);
			}
			blocks |= touched;
			priorityBlocks[priority] |= touched;
			spriteLayers |= 1 << priority;
		}
	next:
		cycles -= sprite->cycles;
		if (cycles <= 0) {
			break;
		}
	}
	renderer->spriteCyclesRemaining = cycles;
	renderer->spriteBlocks = blocks;
	renderer->spriteLayerDirty = blocks;
	renderer->spritePriorityBlocks[0] = priorityBlocks[0];
	renderer->spritePriorityBlocks[1] = priorityBlocks[1];
	renderer->spritePriorityBlocks[2] = priorityBlocks[2];
	renderer->spritePriorityBlocks[3] = priorityBlocks[3];
	return spriteLayers;
}

static void _clearScratch(uint32_t* scratch) {
	int x;
	for (x = 0; x < GBA_VIDEO_HORIZONTAL_PIXELS / 2; x += 4) {
		scratch[x] = MIX_EMPTY * 0x10001;
		scratch[x + 1] = MIX_EMPTY * 0x10001;
		scratch[x + 2] = MIX_EMPTY * 0x10001;
		scratch[x + 3] = MIX_EMPTY * 0x10001;
	}
}

// Puts a target 1 layer, drawn into the scratch row, on top of the output.
// Same result as _mix pixel by pixel; the clamp is branchless since a layer
// that can overflow (EVA + EVB > 16) usually does all over.
#if defined(COLOR_16_BIT) && defined(COLOR_5_6_5)
// Weights are mostly powers of two (8/16 and the like), which turns the two
// multiplies into shifts
FAST_INLINE void _mixLoop(color_t* top, uint8_t* attrs, const color_t* src, unsigned attr, unsigned wa, unsigned wb, uint32_t skip, bool shift, bool clamp) {
	const uint32_t mask = 0x07C0F81F;
	const uint32_t over = 0x08010020;
	int x;
	for (x = 0; x < GBA_VIDEO_HORIZONTAL_PIXELS; skip >>= 1) {
		int end = x + 8;
		if (skip & 1) {
			x = end;
			continue;
		}
		for (; x < end; ++x) {
			uint32_t c = src[x];
			if (c & MIX_EMPTY) {
				continue;
			}
			if (attrs[x] & ATTR_T2) {
				uint32_t b = top[x];
				c = (c | (c << 16)) & mask;
				b = (b | (b << 16)) & mask;
				if (shift) {
					c = ((c << wa) + (b << wb)) >> 4;
				} else {
					c = (c * wa + b * wb) >> 4;
				}
				if (clamp) {
					uint32_t o = c & over;
					c |= o - (o >> 5);
				}
				c &= mask;
				c |= c >> 16;
			}
			attrs[x] = attr;
			top[x] = c;
		}
	}
}

static int _log2Weight(unsigned weight) {
	switch (weight) {
	case 1:
		return 0;
	case 2:
		return 1;
	case 4:
		return 2;
	case 8:
		return 3;
	case 16:
		return 4;
	default:
		return -1;
	}
}
#endif

static ATTRIBUTE_NOINLINE void _mixRow(color_t* top, uint8_t* attrs, const color_t* src, unsigned attr, unsigned blda, unsigned bldb, uint32_t skip) {
#if defined(COLOR_16_BIT) && defined(COLOR_5_6_5)
	int sa = _log2Weight(blda);
	int sb = _log2Weight(bldb);
	if (sa >= 0 && sb >= 0) {
		if (blda + bldb > 16) {
			_mixLoop(top, attrs, src, attr, sa, sb, skip, true, true);
		} else {
			_mixLoop(top, attrs, src, attr, sa, sb, skip, true, false);
		}
	} else if (blda + bldb > 16) {
		_mixLoop(top, attrs, src, attr, blda, bldb, skip, false, true);
	} else {
		_mixLoop(top, attrs, src, attr, blda, bldb, skip, false, false);
	}
#else
	UNUSED(skip);
	int x;
	for (x = 0; x < GBA_VIDEO_HORIZONTAL_PIXELS; ++x) {
		color_t c = src[x];
		if (c == MIX_EMPTY) {
			continue;
		}
		if (attrs[x] & ATTR_T2) {
			c = mColorMix5Bit(blda, c, bldb, top[x]);
		}
		attrs[x] = attr;
		top[x] = c;
	}
#endif
}

struct FastLayer {
	int8_t bg; // -1 for the sprites of this priority
	int8_t priority;
	bool affine;
	uint8_t attr;
};

static void _fillRow(color_t* top, color_t color) {
	int x;
#ifdef COLOR_16_BIT
	if (!((uintptr_t) top & 3)) {
		// Two pixels a store
		uint32_t pair = color | ((uint32_t) color << 16);
		uint32_t* out = (uint32_t*) top;
		for (x = 0; x < GBA_VIDEO_HORIZONTAL_PIXELS / 2; x += 4) {
			out[x] = pair;
			out[x + 1] = pair;
			out[x + 2] = pair;
			out[x + 3] = pair;
		}
		return;
	}
#endif
	for (x = 0; x < GBA_VIDEO_HORIZONTAL_PIXELS; ++x) {
		top[x] = color;
	}
}

#ifdef COLOR_16_BIT
// The blocks of a row started out as MIX_EMPTY that are painted all over
static uint32_t _coveredBlocks(const color_t* top) {
	const uint32_t* in = (const uint32_t*) top;
	const uint32_t empty = MIX_EMPTY * 0x10001;
	uint32_t covered = SKIP_NONE;
	int block;
	for (block = 0; block < GBA_VIDEO_HORIZONTAL_PIXELS / 8; ++block, in += 4) {
		if (!((in[0] | in[1] | in[2] | in[3]) & empty)) {
			covered |= 1 << block;
		}
	}
	return covered;
}

// Fills in what the front layers left of the row from the ones behind
static void _mergeBehind(color_t* top, const color_t* behind, uint32_t covered) {
	int x;
	for (x = 0; x < GBA_VIDEO_HORIZONTAL_PIXELS; covered >>= 1) {
		int end = x + 8;
		if (covered & 1) {
			x = end;
			continue;
		}
		for (; x < end; ++x) {
			if (top[x] & MIX_EMPTY) {
				top[x] = behind[x];
			}
		}
	}
}
// Front to back, each layer only where the ones before left the row empty
static void _paintFront(struct GBAVideoSoftwareRenderer* renderer, int y, const struct FastLayer* layers, int nLayers, struct FastRow* row) {
	bool variantFx = renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN;
	bool first = true;
	for (; nLayers; --nLayers) {
		const struct FastLayer* layer = &layers[nLayers - 1];
		if (!first) {
			row->skip = _coveredBlocks(row->top);
			if (row->skip == 0xFFFFFFFF) {
				return;
			}
		}
		if (layer->bg < 0) {
			mPROFILE_START(profileSprites, "fast sprites composite");
			if (renderer->fastSpritesDirect) {
				_directSpritesUnder(renderer, row->top, layer->priority, row->skip);
			} else if (first) {
				_drawSpritesNoBlend(renderer, row, layer->priority);
			} else {
				_drawSpritesUnder(renderer, row, layer->priority);
			}
			mPROFILE_STOP(profileSprites);
		} else {
			struct GBAVideoSoftwareBackground* bg = &renderer->bg[layer->bg];
			color_t* palette = (bg->target1 && variantFx) ? renderer->variantPalette : renderer->normalPalette;
			if (layer->affine) {
				mPROFILE_START(profileAffine, "fast bg affine");
				if (first) {
					_drawAffinePlain(renderer, bg, row, palette, layer->attr);
				} else {
					_drawAffineUnder(renderer, bg, row, palette, layer->attr);
				}
				mPROFILE_STOP(profileAffine);
			} else {
				mPROFILE_START(profileText, "fast bg text");
				if (first) {
					_drawTextPlain(renderer, bg, y, row, palette, layer->attr);
				} else {
					_drawTextUnder(renderer, bg, y, row, palette, layer->attr);
				}
				mPROFILE_STOP(profileText);
			}
		}
		first = false;
	}
	row->skip = _coveredBlocks(row->top);
}

#endif

// Back to front
static void _paintLayers(struct GBAVideoSoftwareRenderer* renderer, int y, const struct FastLayer* layer, int nLayers, struct FastRow* row, bool blend) {
	bool variantFx = renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN;
	uint32_t scratch[GBA_VIDEO_HORIZONTAL_PIXELS / 2];
	struct FastRow scratchRow = { (color_t*) scratch, NULL, row->blda, row->bldb, row->skip };

	for (; nLayers; --nLayers, ++layer) {
		if (layer->bg < 0) {
			mPROFILE_START(profileSprites, "fast sprites composite");
#ifdef COLOR_16_BIT
			if (renderer->fastSpritesDirect) {
				_directSpritesOver(renderer, row->top, layer->priority);
			} else
#endif
			if (blend) {
				_drawSpritesBlend(renderer, row, layer->priority);
			} else {
				_drawSpritesNoBlend(renderer, row, layer->priority);
			}
			mPROFILE_STOP(profileSprites);
			continue;
		}
		struct GBAVideoSoftwareBackground* bg = &renderer->bg[layer->bg];
		unsigned attr = layer->attr;
		color_t* palette = (bg->target1 && variantFx) ? renderer->variantPalette : renderer->normalPalette;
		// A target 1 layer goes through the scratch row
		struct FastRow* dest = row;
		bool mix = blend && (attr & ATTR_T1);
		if (mix) {
			_clearScratch(scratch);
			dest = &scratchRow;
		}
		if (layer->affine) {
			mPROFILE_START(profileAffine, "fast bg affine");
			if (!blend || mix) {
				_drawAffinePlain(renderer, bg, dest, palette, attr);
			} else {
				_drawAffineAttr(renderer, bg, dest, palette, attr);
			}
			mPROFILE_STOP(profileAffine);
		} else {
			mPROFILE_START(profileText, "fast bg text");
			if (!blend || mix) {
				_drawTextPlain(renderer, bg, y, dest, palette, attr);
			} else {
				_drawTextAttr(renderer, bg, y, dest, palette, attr);
			}
			mPROFILE_STOP(profileText);
		}
		if (mix) {
			mPROFILE_START(profileMix, "fast mix");
			_mixRow(row->top, row->attr, scratchRow.top, attr, row->blda, row->bldb, row->skip);
			mPROFILE_STOP(profileMix);
		}
	}
}


static void _paint(struct GBAVideoSoftwareRenderer* renderer, int y, int spriteLayers, struct FastRow* row, bool blend) {
	int mode = GBARegisterDISPCNTGetMode(renderer->dispcnt);
	bool alpha = renderer->blendEffect == BLEND_ALPHA;
	bool variantFx = renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN;
	color_t backdrop = (renderer->target1Bd && variantFx) ? renderer->variantPalette[0] : renderer->normalPalette[0];

	struct FastLayer layers[8];
	int nLayers = 0;
	int priority;
	for (priority = 3; priority >= 0; --priority) {
		int i;
		for (i = 3; i >= 0; --i) {
			struct GBAVideoSoftwareBackground* bg = &renderer->bg[i];
			bool affine;
			if ((int) bg->priority != priority || !_bgListed(renderer, mode, i, &affine)) {
				continue;
			}
			layers[nLayers].bg = i;
			layers[nLayers].priority = priority;
			layers[nLayers].affine = affine;
			layers[nLayers].attr = (bg->target2 ? ATTR_T2 : 0) | ((alpha && bg->target1) ? ATTR_T1 : 0);
			++nLayers;
		}
		if (spriteLayers & (1 << priority)) {
			layers[nLayers].bg = -1;
			layers[nLayers].priority = priority;
			layers[nLayers].affine = false;
			layers[nLayers].attr = 0;
			++nLayers;
		}
	}
	row->skip = SKIP_NONE;

#ifdef COLOR_16_BIT
	// Blending a layer that's then mostly painted over is a lot of work for
	// nothing. If the layers in front of the ones that blend don't themselves,
	// they can go first, and the rest be left out where those cover it all.
	if (blend && !((uintptr_t) row->top & 3)) {
		bool objBlends = renderer->forceTarget1 || (alpha && renderer->target1Obj);
		int split = nLayers;
		while (split > 0) {
			const struct FastLayer* layer = &layers[split - 1];
			if (layer->bg < 0 ? objBlends : (layer->attr & ATTR_T1)) {
				break;
			}
			--split;
		}
		if (split > 0 && split < nLayers) {
			_fillRow(row->top, MIX_EMPTY);
			struct FastRow front = { row->top, NULL, row->blda, row->bldb, SKIP_NONE };
			_paintFront(renderer, y, &layers[split], nLayers - split, &front);
			uint32_t covered = front.skip;
			if (covered == 0xFFFFFFFF) {
				return;
			}

			uint32_t behind[GBA_VIDEO_HORIZONTAL_PIXELS / 2];
			struct FastRow back = { (color_t*) behind, row->attr, row->blda, row->bldb, covered };
			_fillRow(back.top, backdrop);
			memset(row->attr, renderer->target2Bd ? ATTR_T2 : 0, GBA_VIDEO_HORIZONTAL_PIXELS);
			_paintLayers(renderer, y, layers, split, &back, true);
			_mergeBehind(row->top, back.top, covered);
			return;
		}
	}
#endif

	_fillRow(row->top, backdrop);
	if (blend) {
		memset(row->attr, renderer->target2Bd ? ATTR_T2 : 0, GBA_VIDEO_HORIZONTAL_PIXELS);
	}
	_paintLayers(renderer, y, layers, nLayers, row, blend);
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
ATTRIBUTE_HOT_GROUP(3) ATTRIBUTE_NOINLINE bool GBAVideoSoftwareRendererDrawFast(struct GBAVideoSoftwareRenderer* renderer, int y, int spriteLayers, color_t* out) {
	bool blend;
	if (!_plan(renderer, renderer->forceTarget1, &blend)) {
		return false;
	}
	uint8_t attrs[GBA_VIDEO_HORIZONTAL_PIXELS];
	struct FastRow row;
	row.top = out;
	row.attr = attrs;
	row.blda = renderer->blda;
	row.bldb = renderer->bldb;
	_paint(renderer, y, spriteLayers, &row, blend);
	return true;
}
