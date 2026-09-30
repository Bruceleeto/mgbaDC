/* Fast scanline compositor for the software renderer.
 *
 * One pass, front to back, straight into the output row. A pixel starts out
 * EMPTY, is painted by the first layer that has something there, and is then
 * done. A target 1 pixel is left PENDING instead (its colour with the spare
 * green bit set, straight from a palette prepared that way) until the next
 * layer with something under it resolves it: mixed if that layer is target
 * 2, kept as it is if not. 8 pixel blocks that are all done are skipped by
 * the layers behind.
 *
 * Handles modes 0-2 without windows or BG mosaic, alpha blending and
 * brighten/darken via the variant palette. Anything else, and lines with a
 * sprite it can't draw, go through the regular path.
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

#if defined(COLOR_16_BIT) && defined(COLOR_5_6_5)
#define FAST_OK
#endif

#ifdef FAST_OK
// Green's low bit is never set in a converted colour, so it marks a pixel
// that isn't done: EMPTY, or PENDING with its colour in the other bits
#define PENDING 0x0020
#define EMPTY 0xFFFF
#define EMPTY_PAIR 0xFFFFFFFF
// Blocks of 8 pixels that are done, a bit each. The two past the end of the
// line are always set.
#define COVERED_NONE 0xC0000000

struct FastLine {
	color_t* top;
	uint32_t covered;
	unsigned blda;
	unsigned bldb;
	bool target2;
	// Blocks any layer has painted a pixel in, a bit per 8 pixels. Text
	// tiles on untouched blocks skip the per-pixel test.
	uint32_t touched;
	// A semi-transparent sprite has drawn on this line: later sprites must
	// leave its pending pixels for the BG, not resolve them
	bool strict;
};

// The blocks a tile at x lies in, which can start 7 pixels left of the line
FAST_INLINE uint32_t _tileBlocks(int x) {
	return ((x & 7 ? 3U : 1U) << ((x + 8) >> 3)) >> 1;
}

FAST_INLINE uint32_t _spriteBlocks(int x, int end) {
	return (2U << ((end - 1) >> 3)) - (1U << (x >> 3));
}

// mColorMix5Bit, with its clamps behind one test: a channel can only overflow
// when EVA + EVB > 16, so the usual case pays a single branch. Same result
// bit for bit; green keeps mGBA's 5 bits.
FAST_INLINE color_t _mix(unsigned blda, unsigned a, unsigned bldb, unsigned b) {
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
}

// A pending pixel meets the layer under it. Out of line: it's rare next to
// the plain case and the kernels are unrolled.
ATTRIBUTE_HOT_GROUP(3) static ATTRIBUTE_NOINLINE color_t _resolve(color_t pending, color_t under, bool target2, const struct FastLine* line) {
	pending &= ~PENDING;
	if (!target2) {
		return pending;
	}
	return _mix(line->blda, pending, line->bldb, under);
}

// Paints COLOR at T[I] if nothing's there yet. COLOR is only evaluated then.
#define PAINT(T, I, COLOR) \
	do { \
		color_t d_ = (T)[I]; \
		if (d_ & PENDING) { \
			color_t c_ = (COLOR); \
			(T)[I] = d_ == EMPTY ? c_ : _resolve(d_, c_, target2, line); \
		} \
	} while (0)

// Same for sprites: a pixel another sprite left pending is not theirs to resolve
#define SPRITE_PAINT(T, I, COLOR) \
	do { \
		color_t d_ = (T)[I]; \
		if (d_ & PENDING) { \
			color_t c_ = (COLOR); \
			if (d_ == EMPTY) { \
				(T)[I] = c_; \
			} else if (!line->strict) { \
				(T)[I] = _resolve(d_, c_, target2, line); \
			} \
		} \
	} while (0)

// A 4bpp row with no transparent pixel: nibble-wise "has zero" test.
FAST_INLINE bool _opaque4(uint32_t t) {
	return !((t - 0x11111111) & ~t & 0x88888888);
}

FAST_INLINE bool _opaque8(uint32_t t) {
	return !((t - 0x01010101) & ~t & 0x80808080);
}

ATTRIBUTE_HOT_GROUP(3) static ATTRIBUTE_NOINLINE void _text(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y, struct FastLine* line, const color_t* mainPalette, bool target2) {
	int inX = (background->x - background->offsetX) & 0x1FF;
	int inY = y + background->y - background->offsetY;
	const uint16_t* vram = renderer->d.vram;
	color_t* top = line->top;

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
	int offset = inX & 7;
	int outX = -offset;
	uint32_t charBase0 = background->charBase;
	const uint16_t* mapCache = background->mapCache;
	uint32_t covered = line->covered;
	uint32_t touched = line->touched;
	// Tiles that painted every one of their pixels, a bit each
	uint32_t opaque = 0;
	uint32_t tileBit = 1;
	if (!background->multipalette) {
		for (; outX < GBA_VIDEO_HORIZONTAL_PIXELS; outX += 8, ++tile, tileBit <<= 1) {
			uint32_t blocks = _tileBlocks(outX);
			if ((covered & blocks) == blocks) {
				continue;
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
			const color_t* palette = &mainPalette[GBA_TEXT_MAP_PALETTE(mapData) << 4];
			if (GBA_TEXT_MAP_HFLIP(mapData)) {
				// Reverse nibble order so pixel i is always at bits 4*i
				tileData = (tileData >> 16) | (tileData << 16);
				tileData = ((tileData >> 8) & 0x00FF00FF) | ((tileData << 8) & 0xFF00FF00);
				tileData = ((tileData >> 4) & 0x0F0F0F0F) | ((tileData << 4) & 0xF0F0F0F0);
			}
			if (outX >= 0 && outX <= GBA_VIDEO_HORIZONTAL_PIXELS - 8) {
				color_t* t = &top[outX];
				int i;
				if (!(touched & blocks)) {
					// Nothing painted here yet: no need to look before writing
					touched |= blocks;
					if (_opaque4(tileData)) {
						opaque |= tileBit;
						_Pragma("GCC unroll 8")
						for (i = 0; i < 8; ++i, tileData >>= 4) {
							KEEP(tileData);
							t[i] = *(const color_t*) ((uintptr_t) palette + ((tileData << 1) & 0x1E));
						}
						continue;
					}
					_Pragma("GCC unroll 8")
					for (i = 0; i < 8; ++i, tileData >>= 4) {
						KEEP(tileData);
						unsigned p = tileData & 0xF;
						if (p) {
							t[i] = palette[p];
						}
					}
					continue;
				}
				touched |= blocks;
				if (_opaque4(tileData)) {
					opaque |= tileBit;
					// Walk the row down one register; byte offsets into the palette
					_Pragma("GCC unroll 8")
					for (i = 0; i < 8; ++i, tileData >>= 4) {
						KEEP(tileData);
						PAINT(t, i, *(const color_t*) ((uintptr_t) palette + ((tileData << 1) & 0x1E)));
					}
					continue;
				}
				_Pragma("GCC unroll 8")
				for (i = 0; i < 8; ++i, tileData >>= 4) {
					KEEP(tileData);
					unsigned p = tileData & 0xF;
					if (p) {
						PAINT(t, i, palette[p]);
					}
				}
			} else {
				// Edge tiles
				touched |= blocks;
				int i = outX < 0 ? -outX : 0;
				int end = GBA_VIDEO_HORIZONTAL_PIXELS - outX < 8 ? GBA_VIDEO_HORIZONTAL_PIXELS - outX : 8;
				_Pragma("GCC unroll 1")
				for (tileData >>= i * 4; i < end; ++i, tileData >>= 4) {
					unsigned p = tileData & 0xF;
					if (p) {
						PAINT(top, outX + i, palette[p]);
					}
				}
			}
		}
	} else {
		touched = 0xFFFFFFFF;
		for (; outX < GBA_VIDEO_HORIZONTAL_PIXELS; outX += 8, ++tile, tileBit <<= 1) {
			uint32_t blocks = _tileBlocks(outX);
			if ((covered & blocks) == blocks) {
				continue;
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
			if (_opaque8(lo) && _opaque8(hi) && outX >= 0 && outX <= GBA_VIDEO_HORIZONTAL_PIXELS - 8) {
				opaque |= tileBit;
			}
			int i = outX < 0 ? -outX : 0;
			int end = GBA_VIDEO_HORIZONTAL_PIXELS - outX < 8 ? GBA_VIDEO_HORIZONTAL_PIXELS - outX : 8;
			color_t* t = &top[outX];
			_Pragma("GCC unroll 1")
			for (; i < end; ++i) {
				unsigned p = ((i < 4 ? lo : hi) >> ((i & 3) * 8)) & 0xFF;
				if (p) {
					PAINT(t, i, mainPalette[p]);
				}
			}
		}
	}
	// A block is done when the tile over its left part and the one over its
	// right part both painted all of theirs
	if (offset) {
		opaque &= opaque >> 1;
	}
	// Pending pixels still need the layer under them
	if (mainPalette != renderer->pendingPalette) {
		line->covered = covered | opaque;
	}
	line->touched = touched;
}

// One run of an affine BG. Two copies, so the wrapping one (the usual) has
// nothing to test but the pixel.
// Per-line constants of an affine BG
struct FastAffine {
	int32_t dx;
	int32_t dy;
	uint32_t invDx; // 2^32 / |dx|, 0 when dx is 0
	uint32_t invDy;
	const uint8_t* screenBase;
	const uint8_t* charBase;
	const color_t* palette;
	uint32_t mask;
	int shift;
};

FAST_INLINE const uint8_t* _affineTile(const struct FastAffine* a, int32_t x, int32_t y) {
	uint32_t lx = x & a->mask;
	uint32_t ly = y & a->mask;
	return a->charBase + (a->screenBase[(lx >> 11) + ((ly >> 11) << a->shift)] << 6);
}

// Pixels from tile-local coordinate t (8.8, 0..0x7FF) that stay inside the
// tile, stepping by d. Never more than the truth, never zero.
FAST_INLINE unsigned _affinePixelsInTile(uint32_t t, int32_t d, uint32_t inv) {
	uint32_t room = d > 0 ? 0x7FF - t : t;
	return (unsigned) (((uint64_t) room * inv) >> 32) + 1;
}

// One run of a wrapping affine BG. The work is done a tile at a time: the
// map lookup, the tile pointer and the prefetch of the next tile happen once
// per tile, the pixel loop only walks tx/ty.
FAST_INLINE bool _affineRunWrap(color_t* top, color_t* end, int32_t x, int32_t y, const struct FastAffine* a, const struct FastLine* line) {
	bool solid = true;
#ifdef AFFINE_PLAIN
	for (; top < end; ++top, x += a->dx, y += a->dy) {
		color_t d = *top;
		if (!(d & PENDING)) {
			continue;
		}
		unsigned p = _affineTile(a, x, y)[((y >> 5) & 0x38) + ((x >> 8) & 7)];
		if (p) {
			color_t c = a->palette[p];
			*top = d == EMPTY ? c : _resolve(d, c, line->target2, line);
		}
	}
	return;
#endif
	const uint8_t* tile = _affineTile(a, x, y);
	int32_t dx = a->dx;
	int32_t dy = a->dy;
	while (top < end) {
		uint32_t tx = x & 0x7FF;
		uint32_t ty = y & 0x7FF;
		unsigned n = end - top;
		if (dx) {
			unsigned nx = _affinePixelsInTile(tx, dx, a->invDx);
			if (nx < n) {
				n = nx;
			}
		}
		if (dy) {
			unsigned ny = _affinePixelsInTile(ty, dy, a->invDy);
			if (ny < n) {
				n = ny;
			}
		}
		x += dx * n;
		y += dy * n;
		const uint8_t* next = _affineTile(a, x, y);
		__builtin_prefetch(next);
		color_t* stop = top + n;
		const color_t* palette = a->palette;
		for (;;) {
			for (; top < stop; ++top, tx += dx, ty += dy) {
				color_t d = *top;
				if (!(d & PENDING)) {
					continue;
				}
				unsigned p = tile[((ty >> 5) & 0x38) + (tx >> 8)];
				if (p) {
					if (d != EMPTY) {
						break;
					}
					*top = palette[p];
				} else {
					solid = false;
				}
			}
			if (top == stop) {
				break;
			}
			// A pixel waiting to blend; rare, kept out of the loop above
			*top = _resolve(*top, palette[tile[((ty >> 5) & 0x38) + (tx >> 8)]], line->target2, line);
			++top;
			tx += dx;
			ty += dy;
		}
		tile = next;
	}
	return solid;
}

// Same, clipped to the map. Rare, so it's the plain loop.
FAST_INLINE void _affineRunClip(color_t* top, color_t* end, int32_t x, int32_t y, const struct FastAffine* a, const struct FastLine* line) {
	for (; top < end; ++top, x += a->dx, y += a->dy) {
		color_t d = *top;
		if (!(d & PENDING) || ((x | y) & ~a->mask)) {
			continue;
		}
		unsigned p = _affineTile(a, x, y)[((y >> 5) & 0x38) + ((x >> 8) & 7)];
		if (p) {
			color_t c = a->palette[p];
			*top = d == EMPTY ? c : _resolve(d, c, line->target2, line);
		}
	}
}

ATTRIBUTE_HOT_GROUP(3) static ATTRIBUTE_NOINLINE void _affine(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, struct FastLine* line, const color_t* palette, bool target2) {
	color_t* top = line->top;
	struct FastAffine a;
	a.dx = background->dx;
	a.dy = background->dy;
	a.invDx = a.dx ? 0xFFFFFFFFu / (uint32_t) (a.dx < 0 ? -a.dx : a.dx) : 0;
	a.invDy = a.dy ? 0xFFFFFFFFu / (uint32_t) (a.dy < 0 ? -a.dy : a.dy) : 0;
	a.screenBase = &((const uint8_t*) renderer->d.vram)[background->screenBase];
	a.charBase = &((const uint8_t*) renderer->d.vram)[background->charBase];
	a.palette = palette;
	a.mask = (0x8000 << background->size) - 1;
	a.shift = 4 + background->size;
	int32_t x = background->sx;
	int32_t y = background->sy;
	int32_t dx = a.dx;
	int32_t dy = a.dy;
	uint32_t covered = line->covered;
	bool wrap = background->overflow;
	line->target2 = target2;
	line->touched = 0xFFFFFFFF;
	int block;
	for (block = 0; block < GBA_VIDEO_HORIZONTAL_PIXELS / 8; covered >>= 1) {
		// A run of blocks that aren't done yet
		int end = block;
		while (end < GBA_VIDEO_HORIZONTAL_PIXELS / 8 && !(covered & 1)) {
			++end;
			covered >>= 1;
		}
		if (end > block) {
			if (wrap) {
				if (_affineRunWrap(&top[block * 8], &top[end * 8], x, y, &a, line)) {
					// Every pixel of these blocks is done now
					line->covered |= ((1U << end) - 1) & ~((1U << block) - 1);
				}
			} else {
				_affineRunClip(&top[block * 8], &top[end * 8], x, y, &a, line);
			}
			x += (uint32_t) dx * 8 * (end - block);
			y += (uint32_t) dy * 8 * (end - block);
			block = end;
			if (block == GBA_VIDEO_HORIZONTAL_PIXELS / 8) {
				break;
			}
		}
		// A done block
		x += (uint32_t) dx * 8;
		y += (uint32_t) dy * 8;
		++block;
	}
}

// What's left of the line after every layer
ATTRIBUTE_HOT_GROUP(3) static ATTRIBUTE_NOINLINE void _backdrop(struct FastLine* line, color_t color, bool target2) {
	color_t* top = line->top;
	uint32_t covered = line->covered;
	int block;
	for (block = 0; block < GBA_VIDEO_HORIZONTAL_PIXELS / 8; ++block, covered >>= 1, top += 8) {
		if (covered & 1) {
			continue;
		}
		int i;
		_Pragma("GCC unroll 8")
		for (i = 0; i < 8; ++i) {
			PAINT(top, i, color);
		}
	}
}

// Sprite kernels. What GBAVideoSoftwareRendererPreprocessSprite would put in
// the sprite layer, for the sprites the fast path draws, painted straight
// into the row. They read a tile row at a time and skip the blank ones.

#define FAST_SPRITE_8BPP 0x01
#define FAST_SPRITE_AFFINE 0x02
#define FAST_SPRITE_HFLIP 0x04
#define FAST_SPRITE_VFLIP 0x08
// Counts as drawn, but has nothing on screen
#define FAST_SPRITE_EMPTY 0x20
// Not drawn at all
#define FAST_SPRITE_NONE 0x40
// One for the regular path: mosaic, OBJ window, or blending it can't do
#define FAST_SPRITE_SLOW 0x80

// 8 pixels of a 16 color tile row or 4 of a 256 color one, first pixel lowest
FAST_INLINE uint32_t _spriteWord(const uint16_t* vramBase, unsigned yBase, unsigned charBase, unsigned maskLo, int localX, bool bpp8) {
	unsigned xBase = bpp8 ? (localX & ~0x7) * 8 + (localX & 4) : (localX & ~0x7) * 4;
	uint32_t lo;
	uint32_t hi;
	LOAD_16(lo, (yBase + ((xBase + charBase) & maskLo)) & 0x7FFE, vramBase);
	LOAD_16(hi, (yBase + ((xBase + 2 + charBase) & maskLo)) & 0x7FFE, vramBase);
	return lo | (hi << 16);
}

// Flipped sprites get their tile rows reversed, so there's only the one
// direction to walk.
FAST_INLINE void _spriteNormal(struct GBAVideoSoftwareRenderer* renderer, const struct GBAVideoSoftwareFastSprite* sprite, unsigned yBase, struct FastLine* line, bool bpp8) {
	const int group = bpp8 ? 4 : 8;
	const int bits = bpp8 ? 8 : 4;
	const uint16_t* vramBase = &renderer->d.vram[BASE_TILE >> 1];
	const color_t* palette = sprite->palette;
	color_t* top = line->top;
	uint32_t covered = line->covered;
	bool target2 = renderer->target2Obj;
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
		if (!word || !(here & ~covered)) {
			continue;
		}
		for (; n; --n, ++pixel) {
			unsigned p = word & ((1 << bits) - 1);
			word >>= bits;
			if (p) {
				SPRITE_PAINT(pixel, 0, palette[p]);
			}
		}
	}
}

// Affine sprite that's scaled but not rotated (c == 0): the row of the
// sprite is fixed for the whole line, so only x moves.
FAST_INLINE void _spriteScale(struct GBAVideoSoftwareRenderer* renderer, const struct GBAVideoSoftwareFastSprite* sprite, int xAccum, int yAccum, int outX, struct FastLine* line, bool bpp8) {
	const color_t* palette = sprite->palette;
	bool target2 = renderer->target2Obj;
	int localY = yAccum >> 8;
	if (localY & ~(sprite->height - 1)) {
		return;
	}
	int dx = sprite->a;
	unsigned widthMask = ~(sprite->width - 1);
	unsigned maskLo = sprite->maskLo | 1; // byte addressing, the mask is for words
	unsigned charBase = sprite->charBase;
	unsigned yBase;
	if (bpp8) {
		yBase = ((localY & ~0x7) << sprite->strideShift) + (localY & 0x7) * 8 + sprite->maskHi;
	} else {
		yBase = ((localY & ~0x7) << sprite->strideShift) + (localY & 0x7) * 4 + sprite->maskHi;
	}
	const uint8_t* tiles = &((const uint8_t*) renderer->d.vram)[BASE_TILE];
	color_t* pixel = &line->top[outX];
	color_t* end = &line->top[sprite->condition];
	for (; pixel < end; ++pixel) {
		xAccum += dx;
		int localX = xAccum >> 8;
		if (localX & widthMask) {
			break;
		}
		color_t d = *pixel;
		if (!(d & PENDING)) {
			continue;
		}
		unsigned p;
		if (bpp8) {
			p = tiles[(yBase + (((localX & ~0x7) * 8 + (localX & 7) + charBase) & maskLo)) & 0x7FFF];
		} else {
			p = tiles[(yBase + (((localX & ~0x7) * 4 + ((localX >> 1) & 3) + charBase) & maskLo)) & 0x7FFF];
			p = (p >> ((localX & 1) << 2)) & 0xF;
		}
		if (p) {
			color_t c = palette[p];
			if (d == EMPTY) {
				*pixel = c;
			} else if (!line->strict) {
				*pixel = _resolve(d, c, target2, line);
			}
		}
	}
}

FAST_INLINE void _spriteAffine(struct GBAVideoSoftwareRenderer* renderer, const struct GBAVideoSoftwareFastSprite* sprite, int xAccum, int yAccum, int outX, struct FastLine* line, bool bpp8) {
	const uint16_t* vramBase = &renderer->d.vram[BASE_TILE >> 1];
	const color_t* palette = sprite->palette;
	bool target2 = renderer->target2Obj;
	unsigned charBase = sprite->charBase;
	unsigned maskLo = sprite->maskLo;
	unsigned maskHi = sprite->maskHi;
	unsigned strideShift = sprite->strideShift;
	int dx = sprite->a;
	int dy = sprite->c;
	unsigned widthMask = ~(sprite->width - 1);
	unsigned heightMask = ~(sprite->height - 1);
	color_t* pixel = &line->top[outX];
	color_t* end = &line->top[sprite->condition];
	for (; pixel < end; ++pixel) {
		xAccum += dx;
		yAccum += dy;
		int localX = xAccum >> 8;
		int localY = yAccum >> 8;
		if ((localX & widthMask) | (localY & heightMask)) {
			break;
		}
		if (!(*pixel & PENDING)) {
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
			SPRITE_PAINT(pixel, 0, palette[p]);
		}
	}
}

ATTRIBUTE_HOT_GROUP(4) static ATTRIBUTE_NOINLINE void _spriteNormal16(struct GBAVideoSoftwareRenderer* renderer, const struct GBAVideoSoftwareFastSprite* sprite, unsigned yBase, struct FastLine* line) {
	_spriteNormal(renderer, sprite, yBase, line, false);
}

ATTRIBUTE_HOT_GROUP(4) static ATTRIBUTE_NOINLINE void _spriteNormal256(struct GBAVideoSoftwareRenderer* renderer, const struct GBAVideoSoftwareFastSprite* sprite, unsigned yBase, struct FastLine* line) {
	_spriteNormal(renderer, sprite, yBase, line, true);
}

ATTRIBUTE_HOT_GROUP(4) static ATTRIBUTE_NOINLINE void _spriteScale16(struct GBAVideoSoftwareRenderer* renderer, const struct GBAVideoSoftwareFastSprite* sprite, int xAccum, int yAccum, int outX, struct FastLine* line) {
	_spriteScale(renderer, sprite, xAccum, yAccum, outX, line, false);
}

ATTRIBUTE_HOT_GROUP(4) static ATTRIBUTE_NOINLINE void _spriteScale256(struct GBAVideoSoftwareRenderer* renderer, const struct GBAVideoSoftwareFastSprite* sprite, int xAccum, int yAccum, int outX, struct FastLine* line) {
	_spriteScale(renderer, sprite, xAccum, yAccum, outX, line, true);
}

ATTRIBUTE_HOT_GROUP(4) static ATTRIBUTE_NOINLINE void _spriteAffine16(struct GBAVideoSoftwareRenderer* renderer, const struct GBAVideoSoftwareFastSprite* sprite, int xAccum, int yAccum, int outX, struct FastLine* line) {
	_spriteAffine(renderer, sprite, xAccum, yAccum, outX, line, false);
}

ATTRIBUTE_HOT_GROUP(4) static ATTRIBUTE_NOINLINE void _spriteAffine256(struct GBAVideoSoftwareRenderer* renderer, const struct GBAVideoSoftwareFastSprite* sprite, int xAccum, int yAccum, int outX, struct FastLine* line) {
	_spriteAffine(renderer, sprite, xAccum, yAccum, outX, line, true);
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

// The line's sprites of one priority, front to back: the first in OAM wins
ATTRIBUTE_HOT_GROUP(4) static ATTRIBUTE_NOINLINE void _sprites(struct GBAVideoSoftwareRenderer* renderer, int y, unsigned priority, struct FastLine* line) {
	int start = renderer->spriteLineStart[y];
	int end = renderer->spriteLineStart[y + 1];
	const uint8_t* index = &renderer->spriteLines[start];
	const uint8_t* priorities = &renderer->spriteLinePriority[start];
	const uint16_t* cycles = &renderer->spriteLineCycles[start];
	int budget = GBARegisterDISPCNTIsHblankIntervalFree(renderer->dispcnt) ? OBJ_HBLANK_FREE_LENGTH : OBJ_LENGTH;
	int i;
	for (i = 0; i < end - start; ++i) {
		if (priorities[i] != priority) {
			continue;
		}
		if (cycles[i] >= budget) {
			// Out of time on this line, and so is everything after it
			break;
		}
		const struct GBAVideoSoftwareFastSprite* sprite = &renderer->fastSprites[index[i]];
		unsigned kind = sprite->kind;
		if (kind & (FAST_SPRITE_NONE | FAST_SPRITE_EMPTY)) {
			continue;
		}
		if ((size_t) (sprite->palette - renderer->pendingPalette) < 512) {
			line->strict = true;
		}
		line->touched |= _spriteBlocks(sprite->outX, sprite->condition);
		if (kind & FAST_SPRITE_AFFINE) {
			int outX;
			int xAccum;
			int yAccum;
			if (!_spriteAffineLine(sprite, y, &outX, &xAccum, &yAccum)) {
				continue;
			}
			if (!sprite->c) {
				mPROFILE_START(profileScale, "sprite scale");
				if (kind & FAST_SPRITE_8BPP) {
					_spriteScale256(renderer, sprite, xAccum, yAccum, outX, line);
				} else {
					_spriteScale16(renderer, sprite, xAccum, yAccum, outX, line);
				}
				mPROFILE_STOP(profileScale);
			} else {
				mPROFILE_START(profileRotate, "sprite rotate");
				if (kind & FAST_SPRITE_8BPP) {
					_spriteAffine256(renderer, sprite, xAccum, yAccum, outX, line);
				} else {
					_spriteAffine16(renderer, sprite, xAccum, yAccum, outX, line);
				}
				mPROFILE_STOP(profileRotate);
			}
		} else {
			int inY = y - sprite->y;
			if (kind & FAST_SPRITE_VFLIP) {
				inY = sprite->height - inY - 1;
			}
			unsigned yBase = ((inY & ~0x7) << sprite->strideShift) + sprite->maskHi;
			mPROFILE_START(profileNormal, "sprite normal");
			if (kind & FAST_SPRITE_8BPP) {
				_spriteNormal256(renderer, sprite, yBase + (inY & 0x7) * 8, line);
			} else {
				_spriteNormal16(renderer, sprite, yBase + (inY & 0x7) * 4, line);
			}
			mPROFILE_STOP(profileNormal);
		}
	}
}

// What GBAVideoSoftwareRendererPreprocessSprite works out from the registers
// for every sprite
FAST_INLINE void _spriteKey(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareFastSpriteKey* key) {
	bool alpha = renderer->blendEffect == BLEND_ALPHA;
	bool variant = renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN;
	int target2 = renderer->target2Bd;
	target2 |= renderer->bg[0].target2 && renderer->bg[0].enabled;
	target2 |= renderer->bg[1].target2 && renderer->bg[1].enabled;
	target2 |= renderer->bg[2].target2 && renderer->bg[2].enabled;
	target2 |= renderer->bg[3].target2 && renderer->bg[3].enabled;
	// A target-1 BG above a semi-transparent sprite: one pending bit can't
	// tell the two apart
	bool bgTarget1 = alpha && ((renderer->bg[0].target1 && renderer->bg[0].enabled) || (renderer->bg[1].target1 && renderer->bg[1].enabled) ||
	                           (renderer->bg[2].target1 && renderer->bg[2].enabled) || (renderer->bg[3].target1 && renderer->bg[3].enabled));
	key->mapping = GBARegisterDISPCNTIsObjCharacterMapping(renderer->dispcnt);
	key->bitmap = GBARegisterDISPCNTGetMode(renderer->dispcnt) >= 3;
	key->offsetX = renderer->objOffsetX;
	key->offsetY = renderer->objOffsetY;
	// Indexed by whether the sprite is semitransparent
	int semi;
	for (semi = 0; semi < 2; ++semi) {
		bool target1 = (renderer->target1Obj && alpha) || semi;
		bool slow = false;
		const color_t* palette = &renderer->normalPalette[0x100];
		if (target1 && !target2) {
			// Nothing to blend with
			target1 = false;
		}
		if (target1) {
			if (renderer->target2Obj || (semi && variant) || bgTarget1) {
				// Sprites over sprites, or a semi-transparent sprite mixed
				// while the rest is brightened: the regular path's job
				slow = true;
			}
			palette = &renderer->pendingPalette[0x100];
		} else if (renderer->target1Obj && variant) {
			palette = &renderer->variantPalette[0x100];
		}
		key->palette[semi] = palette;
		key->slow[semi] = slow;
	}
}

FAST_INLINE bool _spriteKeyMatches(const struct GBAVideoSoftwareFastSpriteKey* a, const struct GBAVideoSoftwareFastSpriteKey* b) {
	return a->palette[0] == b->palette[0] && a->palette[1] == b->palette[1] &&
	       a->offsetX == b->offsetX && a->offsetY == b->offsetY &&
	       a->slow[0] == b->slow[0] && a->slow[1] == b->slow[1] &&
	       a->mapping == b->mapping && a->bitmap == b->bitmap;
}

// The lines a sprite is on: from its top down, and from the top of the screen
// if it wraps around
static void _spriteLineRanges(const struct GBAVideoRendererSprite* sprite, int* start, int* end, int* wrapEnd) {
	*start = sprite->y < 0 ? 0 : sprite->y;
	*end = sprite->endY > GBA_VIDEO_VERTICAL_PIXELS ? GBA_VIDEO_VERTICAL_PIXELS : sprite->endY;
	*wrapEnd = sprite->endY - 256;
	if (*wrapEnd > sprite->y) {
		*wrapEnd = sprite->y;
	}
	if (*wrapEnd > *end) {
		*wrapEnd = *end;
	}
}

// The part of GBAVideoSoftwareRendererPreprocessSprite that's the same on
// every line of a sprite, for a line that's one window wide
static ATTRIBUTE_NOINLINE void _prepareSprites(struct GBAVideoSoftwareRenderer* renderer, const struct GBAVideoSoftwareFastSpriteKey* key) {
	const int start = 0;
	const int end = GBA_VIDEO_HORIZONTAL_PIXELS;
	bool mapping = key->mapping;
	memset(renderer->spriteLineSlow, 0, sizeof(renderer->spriteLineSlow));
	int i;
	for (i = 0; i < renderer->oamMax; ++i) {
		const struct GBAObj* sprite = &renderer->sprites[i].obj;
		struct GBAVideoSoftwareFastSprite* fast = &renderer->fastSprites[i];
		unsigned size = GBAObjAttributesAGetShape(sprite->a) * 4 + GBAObjAttributesBGetSize(sprite->b);
		int width = GBAVideoObjSizes[size][0];
		int height = GBAVideoObjSizes[size][1];
		unsigned semi = GBAObjAttributesAGetMode(sprite->a) == OBJ_MODE_SEMITRANSPARENT;
		bool bpp8 = GBAObjAttributesAIs256Color(sprite->a);
		int32_t x = (uint32_t) GBAObjAttributesBGetX(sprite->b) << 23;
		x >>= 23;
		x += key->offsetX;
		unsigned align = bpp8 && !mapping;
		unsigned charBase = (GBAObjAttributesCGetTile(sprite->c) & ~align) * 0x20;
		int stride = mapping ? (width >> !bpp8) : 0x80;

		if (GBAObjAttributesAIsMosaic(sprite->a) || GBAObjAttributesAGetMode(sprite->a) == OBJ_MODE_OBJWIN || key->slow[semi]) {
			fast->kind = FAST_SPRITE_SLOW;
			// Every line it's on takes the regular path
			int lineStart, lineEnd, wrapEnd;
			int y;
			_spriteLineRanges(&renderer->sprites[i], &lineStart, &lineEnd, &wrapEnd);
			for (y = 0; y < wrapEnd; ++y) {
				renderer->spriteLineSlow[y] = 1;
			}
			for (y = lineStart; y < lineEnd; ++y) {
				renderer->spriteLineSlow[y] = 1;
			}
			continue;
		}
		if (key->bitmap && GBAObjAttributesCGetTile(sprite->c) < 512) {
			fast->kind = FAST_SPRITE_NONE;
			continue;
		}
		unsigned kind = bpp8 ? FAST_SPRITE_8BPP : 0;
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

// Whether the line's blending is something the one pass can do
FAST_INLINE bool _plan(const struct GBAVideoSoftwareRenderer* renderer) {
	if (renderer->blendEffect != BLEND_ALPHA) {
		return true;
	}
	int i;
	for (i = 0; i < 4; ++i) {
		const struct GBAVideoSoftwareBackground* bg = &renderer->bg[i];
		if (bg->enabled == ENABLED_MAX && bg->target1 && bg->target2) {
			return false;
		}
	}
	return true;
}
#endif

// The lines' sprite lists, from the cleaned OAM: in OAM order, with each
// one's priority and the cycles the ones before it on the line took
ATTRIBUTE_NOINLINE void GBAVideoSoftwareRendererListSpriteLines(struct GBAVideoSoftwareRenderer* renderer) {
	uint16_t* lineStart = renderer->spriteLineStart;
	uint16_t lineCycles[GBA_VIDEO_VERTICAL_PIXELS];
	memset(renderer->spriteLineStart, 0, sizeof(renderer->spriteLineStart));
	memset(lineCycles, 0, sizeof(lineCycles));
	memset(renderer->spriteLinePriorities, 0, sizeof(renderer->spriteLinePriorities));
	int i;
	int y;
	int start, end, wrapEnd;
	for (i = 0; i < renderer->oamMax; ++i) {
		_spriteLineRanges(&renderer->sprites[i], &start, &end, &wrapEnd);
		for (y = 0; y < wrapEnd; ++y) {
			++lineStart[y];
		}
		for (y = start; y < end; ++y) {
			++lineStart[y];
		}
	}
	int total = 0;
	for (y = 0; y < GBA_VIDEO_VERTICAL_PIXELS; ++y) {
		int count = lineStart[y];
		lineStart[y] = total;
		total += count;
	}
	lineStart[GBA_VIDEO_VERTICAL_PIXELS] = total;
	for (i = 0; i < renderer->oamMax; ++i) {
		const struct GBAVideoRendererSprite* sprite = &renderer->sprites[i];
		unsigned priority = GBAObjAttributesCGetPriority(sprite->obj.c);
		_spriteLineRanges(sprite, &start, &end, &wrapEnd);
		for (y = 0; y < wrapEnd; ++y) {
			int at = lineStart[y]++;
			renderer->spriteLines[at] = i;
			renderer->spriteLinePriority[at] = priority;
			renderer->spriteLineCycles[at] = lineCycles[y];
			lineCycles[y] += sprite->cycles;
			renderer->spriteLinePriorities[y] |= 1 << priority;
		}
		for (y = start; y < end; ++y) {
			int at = lineStart[y]++;
			renderer->spriteLines[at] = i;
			renderer->spriteLinePriority[at] = priority;
			renderer->spriteLineCycles[at] = lineCycles[y];
			lineCycles[y] += sprite->cycles;
			renderer->spriteLinePriorities[y] |= 1 << priority;
		}
	}
	// Each start is now where the next line starts
	memmove(&lineStart[1], &lineStart[0], sizeof(lineStart[0]) * (GBA_VIDEO_VERTICAL_PIXELS - 1));
	lineStart[0] = 0;
}

bool GBAVideoSoftwareRendererFastEligible(struct GBAVideoSoftwareRenderer* renderer) {
#ifdef FAST_OK
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
#else
	UNUSED(renderer);
	return false;
#endif
}

// For a line the fast path may take, with sprites on: gets the sprites ready
// if they aren't, and says whether the line can be drawn in one pass. If it
// can't, the regular sprite layer has to be built.
ATTRIBUTE_NOINLINE bool GBAVideoSoftwareRendererFastSpriteLayer(struct GBAVideoSoftwareRenderer* renderer, int y) {
#ifdef FAST_OK
	struct GBAVideoSoftwareFastSpriteKey key;
	_spriteKey(renderer, &key);
	bool stale = !renderer->fastSpritesValid || !_spriteKeyMatches(&key, &renderer->fastSpriteKey);
	if (UNLIKELY(stale)) {
		mPROFILE_START(profilePrepare, "sprite prepare");
		_prepareSprites(renderer, &key);
		mPROFILE_STOP(profilePrepare);
	}
	return !renderer->spriteLineSlow[y] && _plan(renderer);
#else
	UNUSED(renderer);
	UNUSED(y);
	return false;
#endif
}

#ifdef FAST_OK
ATTRIBUTE_HOT_GROUP_BIG(3) ATTRIBUTE_NOINLINE void GBAVideoSoftwareRendererDrawFast(struct GBAVideoSoftwareRenderer* renderer, int y, int spriteLayers, color_t* out) {
	int mode = GBARegisterDISPCNTGetMode(renderer->dispcnt);
	bool alpha = renderer->blendEffect == BLEND_ALPHA;
	bool variantFx = renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN;
	struct FastLine line = { out, COVERED_NONE, renderer->blda, renderer->bldb, false, 0, false };

	mPROFILE_START(profileFill, "fast fill");
	int x;
	if (!((uintptr_t) out & 3)) {
		uint32_t* pair = (uint32_t*) out;
		for (x = 0; x < GBA_VIDEO_HORIZONTAL_PIXELS / 2; x += 4) {
			pair[x] = EMPTY_PAIR;
			pair[x + 1] = EMPTY_PAIR;
			pair[x + 2] = EMPTY_PAIR;
			pair[x + 3] = EMPTY_PAIR;
		}
	} else {
		for (x = 0; x < GBA_VIDEO_HORIZONTAL_PIXELS; ++x) {
			out[x] = EMPTY;
		}
	}
	mPROFILE_STOP(profileFill);

	int priority;
	for (priority = 0; priority < 4; ++priority) {
		// Sprites go in front of the BGs of their priority
		if (spriteLayers & (1 << priority)) {
			mPROFILE_START(profileSprites, "fast sprites");
			_sprites(renderer, y, priority, &line);
			mPROFILE_STOP(profileSprites);
		}
		int i;
		for (i = 0; i < 4; ++i) {
			struct GBAVideoSoftwareBackground* bg = &renderer->bg[i];
			bool affine;
			if ((int) bg->priority != priority || !_bgListed(renderer, mode, i, &affine)) {
				continue;
			}
			const color_t* palette = renderer->normalPalette;
			if (bg->target1) {
				if (alpha) {
					palette = renderer->pendingPalette;
				} else if (variantFx) {
					palette = renderer->variantPalette;
				}
			}
			if (affine) {
				mPROFILE_START(profileAffine, "fast bg affine");
				_affine(renderer, bg, &line, palette, bg->target2);
				mPROFILE_STOP(profileAffine);
			} else {
				mPROFILE_START(profileText, "fast bg text");
				_text(renderer, bg, y, &line, palette, bg->target2);
				mPROFILE_STOP(profileText);
			}
			if (line.covered == 0xFFFFFFFF) {
				return;
			}
		}
	}
	mPROFILE_START(profileBackdrop, "fast backdrop");
	color_t backdrop = (renderer->target1Bd && variantFx) ? renderer->variantPalette[0] : renderer->normalPalette[0];
	_backdrop(&line, backdrop, renderer->target2Bd);
	mPROFILE_STOP(profileBackdrop);
}
#else
void GBAVideoSoftwareRendererDrawFast(struct GBAVideoSoftwareRenderer* renderer, int y, int spriteLayers, color_t* out) {
	UNUSED(renderer);
	UNUSED(y);
	UNUSED(spriteLayers);
	UNUSED(out);
}
#endif
