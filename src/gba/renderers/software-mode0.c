/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba/renderers/software-private.h"

#include <mgba/core/profile.h>
#include <mgba/internal/gba/gba.h>

#define BACKGROUND_TEXT_SELECT_CHARACTER \
	xBase = localX & 0xF8; \
	if (background->size & 1) { \
		xBase += (localX & 0x100) << 5; \
	} \
	screenBase = yBase + (xBase >> 3); \
	LOAD_16(mapData, screenBase << 1, vram); \

#define DRAW_BACKGROUND_MODE_0_TILE_SUFFIX_16(BLEND, OBJWIN) \
	paletteData = GBA_TEXT_MAP_PALETTE(mapData) << 4; \
	palette = &mainPalette[paletteData]; \
	charBase = (background->charBase + (GBA_TEXT_MAP_TILE(mapData) << 5)) + (localY << 2); \
	LOAD_32(tileData, charBase, vram); \
	if (!GBA_TEXT_MAP_HFLIP(mapData)) { \
		tileData >>= 4 * mod8; \
		for (; outX < end; ++outX, ++pixel) { \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 0); \
		} \
	} else { \
		for (outX = end - 1; outX >= renderer->start; --outX) { \
			uint32_t* pixel = &renderer->row[outX]; \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 0); \
		} \
	}

#define DRAW_BACKGROUND_MODE_0_TILE_PREFIX_16(BLEND, OBJWIN) \
	charBase = (background->charBase + (GBA_TEXT_MAP_TILE(mapData) << 5)) + (localY << 2); \
	LOAD_32(tileData, charBase, vram); \
	paletteData = GBA_TEXT_MAP_PALETTE(mapData) << 4; \
	palette = &mainPalette[paletteData]; \
	pixel = &renderer->row[outX]; \
	if (!GBA_TEXT_MAP_HFLIP(mapData)) { \
		if (outX < renderer->start) { \
			tileData >>= 4 * (renderer->start - outX); \
			outX = renderer->start; \
			pixel = &renderer->row[outX]; \
		} \
		for (; outX < renderer->end; ++outX, ++pixel) { \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 0); \
		} \
	} else { \
		tileData >>= 4 * (0x8 - mod8); \
		int end = renderer->end - 8; \
		if (end < -1) { \
			end = -1; \
		} \
		outX = renderer->end - 1; \
		pixel = &renderer->row[outX]; \
		for (; outX > end; --outX, --pixel) { \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 0); \
		} \
		/* Needed for consistency checks */ \
		if (VIDEO_CHECKS) { \
			outX = renderer->end; \
			pixel = &renderer->row[outX]; \
		} \
	}

#define DRAW_BACKGROUND_MODE_0_MOSAIC_16(BLEND, OBJWIN) \
	x = inX & 7; \
	if (mosaicWait) { \
		int baseX = x - (mosaicH - mosaicWait); \
		if (baseX < 0) { \
			int disturbX = (16 + baseX) >> 3; \
			inX -= disturbX << 3; \
			localX = tileX * 8 + inX; \
			BACKGROUND_TEXT_SELECT_CHARACTER; \
			localY = inY & 0x7; \
			if (GBA_TEXT_MAP_VFLIP(mapData)) { \
				localY = 7 - localY; \
			} \
			baseX -= disturbX << 3; \
			inX += disturbX << 3; \
		} else { \
			localX = tileX * 8 + inX; \
			BACKGROUND_TEXT_SELECT_CHARACTER; \
			localY = inY & 0x7; \
			if (GBA_TEXT_MAP_VFLIP(mapData)) { \
				localY = 7 - localY; \
			} \
		} \
		charBase = (background->charBase + (GBA_TEXT_MAP_TILE(mapData) << 5)) + (localY << 2); \
		if (UNLIKELY(charBase >= 0x10000)) { \
			carryData = 0; \
		} else { \
			paletteData = GBA_TEXT_MAP_PALETTE(mapData) << 4; \
			palette = &mainPalette[paletteData]; \
			LOAD_32(tileData, charBase, vram); \
			if (!GBA_TEXT_MAP_HFLIP(mapData)) { \
				tileData >>= 4 * baseX; \
			} else { \
				tileData >>= 4 * (7 - baseX); \
			} \
			tileData &= 0xF; \
			tileData |= tileData << 4; \
			tileData |= tileData << 8; \
			tileData |= tileData << 16; \
			carryData = tileData; \
		} \
	} \
	localX = tileX * 8 + inX; \
	for (; length; ++tileX) { \
		mapData = background->mapCache[(localX >> 3) & 0x3F]; \
		localX += 8; \
		localY = inY & 0x7; \
		if (GBA_TEXT_MAP_VFLIP(mapData)) { \
			localY = 7 - localY; \
		} \
		charBase = (background->charBase + (GBA_TEXT_MAP_TILE(mapData) << 5)) + (localY << 2); \
		tileData = carryData; \
		for (; x < 8 && length; ++x, --length) { \
			if (!mosaicWait) { \
				if (UNLIKELY(charBase >= 0x10000)) { \
					carryData = 0; \
				} else { \
					paletteData = GBA_TEXT_MAP_PALETTE(mapData) << 4; \
					palette = &mainPalette[paletteData]; \
					LOAD_32(tileData, charBase, vram); \
					if (!GBA_TEXT_MAP_HFLIP(mapData)) { \
						tileData >>= x * 4; \
					} else { \
						tileData >>= (7 - x) * 4; \
					} \
					tileData &= 0xF; \
					tileData |= tileData << 4; \
					tileData |= tileData << 8; \
					tileData |= tileData << 16; \
					carryData = tileData; \
				} \
				mosaicWait = mosaicH; \
			} \
			--mosaicWait; \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 0); \
			++pixel; \
		} \
		x = 0; \
	}

#define DRAW_BACKGROUND_MODE_0_TILES_16(BLEND, OBJWIN) \
	for (; tileX < tileEnd; ++tileX) { \
		mapData = background->mapCache[(localX >> 3) & 0x3F]; \
		localX += 8; \
		localY = inY & 0x7; \
		if (GBA_TEXT_MAP_VFLIP(mapData)) { \
			localY = 7 - localY; \
		} \
		paletteData = GBA_TEXT_MAP_PALETTE(mapData) << 4; \
		palette = &mainPalette[paletteData]; \
		charBase = (background->charBase + (GBA_TEXT_MAP_TILE(mapData) << 5)) + (localY << 2); \
		if (UNLIKELY(charBase >= 0x10000)) { \
			pixel += 8; \
			continue; \
		} \
		LOAD_32(tileData, charBase, vram); \
		if (tileData) { \
			if (!GBA_TEXT_MAP_HFLIP(mapData)) { \
				BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 0); \
				BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 1); \
				BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 2); \
				BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 3); \
				BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 4); \
				BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 5); \
				BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 6); \
				BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 7); \
			} else { \
				BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 7); \
				BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 6); \
				BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 5); \
				BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 4); \
				BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 3); \
				BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 2); \
				BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 1); \
				BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 0); \
			} \
		} \
		pixel += 8; \
	}

#define DRAW_BACKGROUND_MODE_0_TILE_SUFFIX_256(BLEND, OBJWIN) \
	charBase = (background->charBase + (GBA_TEXT_MAP_TILE(mapData) << 6)) + (localY << 3); \
	int end2 = end - 4; \
	if (!GBA_TEXT_MAP_HFLIP(mapData)) { \
		int shift = inX & 0x3; \
		if (LIKELY(charBase < 0x10000)) { \
			if (end2 > outX) { \
				LOAD_32(tileData, charBase, vram); \
				tileData >>= 8 * shift; \
				shift = 0; \
				for (; outX < end2; ++outX, ++pixel) { \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
				} \
			} \
		} \
		\
		if (LIKELY(charBase < 0x10000)) { \
			LOAD_32(tileData, charBase + 4, vram); \
			tileData >>= 8 * shift; \
			for (; outX < end; ++outX, ++pixel) { \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
			} \
		} \
	} else { \
		int start = outX; \
		outX = end - 1; \
		pixel = &renderer->row[outX]; \
		if (LIKELY(charBase < 0x10000)) { \
			if (end2 > start) { \
				LOAD_32(tileData, charBase, vram); \
				for (; outX >= end2; --outX, --pixel) { \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
				} \
				charBase += 4; \
			} \
		} \
		\
		if (LIKELY(charBase < 0x10000)) { \
			LOAD_32(tileData, charBase, vram); \
			for (; outX >= renderer->start; --outX, --pixel) { \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
			} \
		} \
		outX = end; \
		pixel = &renderer->row[outX]; \
	}

#define DRAW_BACKGROUND_MODE_0_TILE_PREFIX_256(BLEND, OBJWIN) \
	charBase = (background->charBase + (GBA_TEXT_MAP_TILE(mapData) << 6)) + (localY << 3); \
	if (UNLIKELY(charBase >= 0x10000)) { \
		return; \
	} \
	int end = mod8 - 4; \
	pixel = &renderer->row[outX]; \
	if (!GBA_TEXT_MAP_HFLIP(mapData)) { \
		if (end > 0) { \
			LOAD_32(tileData, charBase, vram); \
			for (; outX < renderer->end - end; ++outX, ++pixel) { \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
			} \
			charBase += 4; \
		} \
		\
		LOAD_32(tileData, charBase, vram); \
		for (; outX < renderer->end; ++outX, ++pixel) { \
			BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
		} \
	} else { \
		int shift = (8 - mod8) & 0x3; \
		int start = outX; \
		outX = renderer->end - 1; \
		pixel = &renderer->row[outX]; \
		if (end > 0) { \
			LOAD_32(tileData, charBase, vram); \
			tileData >>= 8 * shift; \
			for (; outX >= start + 4; --outX, --pixel) { \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
			} \
			shift = 0; \
		} \
		\
		LOAD_32(tileData, charBase + 4, vram); \
		tileData >>= 8 * shift; \
		for (; outX >= start; --outX, --pixel) { \
			BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
		} \
		/* Needed for consistency checks */ \
		if (VIDEO_CHECKS) { \
			outX = renderer->end; \
			pixel = &renderer->row[outX]; \
		} \
	}

#define DRAW_BACKGROUND_MODE_0_TILES_256(BLEND, OBJWIN) \
	for (; tileX < tileEnd; ++tileX) { \
		mapData = background->mapCache[(localX >> 3) & 0x3F]; \
		localX += 8; \
		localY = inY & 0x7; \
		if (GBA_TEXT_MAP_VFLIP(mapData)) { \
			localY = 7 - localY; \
		} \
		charBase = (background->charBase + (GBA_TEXT_MAP_TILE(mapData) << 6)) + (localY << 3); \
		if (UNLIKELY(charBase >= 0x10000)) { \
			pixel += 8; \
			continue; \
		} \
		if (!GBA_TEXT_MAP_HFLIP(mapData)) { \
			LOAD_32(tileData, charBase, vram); \
			if (tileData) { \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 1); \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 2); \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 3); \
			} \
			pixel += 4; \
			LOAD_32(tileData, charBase + 4, vram); \
			if (tileData) { \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 1); \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 2); \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 3); \
			} \
			pixel += 4; \
		} else { \
			LOAD_32(tileData, charBase + 4, vram); \
			if (tileData) { \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 3); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 2); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 1); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
			} \
			pixel += 4; \
			LOAD_32(tileData, charBase, vram); \
			if (tileData) { \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 3); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 2); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 1); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
			} \
			pixel += 4; \
		} \
	}

#define DRAW_BACKGROUND_MODE_0_MOSAIC_256(BLEND, OBJWIN) \
	x = inX & 7; \
	if (mosaicWait) { \
		int baseX = x - (mosaicH - mosaicWait); \
		if (baseX < 0) { \
			int disturbX = (16 + baseX) >> 3; \
			inX -= disturbX << 3; \
			localX = tileX * 8 + inX; \
			BACKGROUND_TEXT_SELECT_CHARACTER; \
			localY = inY & 0x7; \
			if (GBA_TEXT_MAP_VFLIP(mapData)) { \
				localY = 7 - localY; \
			} \
			baseX -= disturbX << 3; \
			inX += disturbX << 3; \
		} else { \
			localX = tileX * 8 + inX; \
			BACKGROUND_TEXT_SELECT_CHARACTER; \
			localY = inY & 0x7; \
			if (GBA_TEXT_MAP_VFLIP(mapData)) { \
				localY = 7 - localY; \
			} \
		} \
		charBase = (background->charBase + (GBA_TEXT_MAP_TILE(mapData) << 6)) + (localY << 3); \
		if (UNLIKELY(charBase >= 0x10000)) { \
			carryData = 0; \
		} else { \
			LOAD_32(tileData, charBase, vram); \
			if (!GBA_TEXT_MAP_HFLIP(mapData)) { \
				if (x >= 4) { \
					LOAD_32(tileData, charBase + 4, vram); \
					tileData >>= (x - 4) * 8; \
				} else { \
					LOAD_32(tileData, charBase, vram); \
					tileData >>= x * 8; \
				} \
			} else { \
				if (x >= 4) { \
					LOAD_32(tileData, charBase, vram); \
					tileData >>= (7 - x) * 8; \
				} else { \
					LOAD_32(tileData, charBase + 4, vram); \
					tileData >>= (3 - x) * 8; \
				} \
			} \
			tileData &= 0xFF; \
			carryData = tileData; \
		} \
	} \
	localX = tileX * 8 + inX; \
	for (; length; ++tileX) { \
		mapData = background->mapCache[(localX >> 3) & 0x3F]; \
		localX += 8; \
		localY = inY & 0x7; \
		if (GBA_TEXT_MAP_VFLIP(mapData)) { \
			localY = 7 - localY; \
		} \
		charBase = (background->charBase + (GBA_TEXT_MAP_TILE(mapData) << 6)) + (localY << 3); \
		tileData = carryData; \
		for (; x < 8 && length; ++x, --length) { \
			if (!mosaicWait) { \
				if (UNLIKELY(charBase >= 0x10000)) { \
					carryData = 0; \
				} else { \
					if (!GBA_TEXT_MAP_HFLIP(mapData)) { \
						if (x >= 4) { \
							LOAD_32(tileData, charBase + 4, vram); \
							tileData >>= (x - 4) * 8; \
						} else { \
							LOAD_32(tileData, charBase, vram); \
							tileData >>= x * 8; \
						} \
					} else { \
						if (x >= 4) { \
							LOAD_32(tileData, charBase, vram); \
							tileData >>= (7 - x) * 8; \
						} else { \
							LOAD_32(tileData, charBase + 4, vram); \
							tileData >>= (3 - x) * 8; \
						} \
					} \
					tileData &= 0xFF; \
					carryData = tileData; \
				} \
				mosaicWait = mosaicH; \
			} \
			tileData |= tileData << 8; \
			--mosaicWait; \
			BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
			++pixel; \
		} \
		x = 0; \
	}

#define DRAW_BACKGROUND_MODE_0(BPP, BLEND, OBJWIN) \
	uint32_t* pixel = &renderer->row[outX]; \
	if (background->mosaic && GBAMosaicControlGetBgH(renderer->mosaic)) { \
		int mosaicH = GBAMosaicControlGetBgH(renderer->mosaic) + 1; \
		int x; \
		int mosaicWait = (mosaicH - outX + GBA_VIDEO_HORIZONTAL_PIXELS * mosaicH) % mosaicH; \
		int carryData = 0; \
		paletteData = 0; /* Quiets compiler warning */ \
		DRAW_BACKGROUND_MODE_0_MOSAIC_ ## BPP (BLEND, OBJWIN) \
		return; \
	} \
	\
	if (inX & 0x7) { \
		localX = tileX * 8 + inX; \
		BACKGROUND_TEXT_SELECT_CHARACTER; \
		localY = inY & 0x7; \
		if (GBA_TEXT_MAP_VFLIP(mapData)) { \
			localY = 7 - localY; \
		} \
		int mod8 = inX & 0x7; \
		int end = outX + 0x8 - mod8; \
		if (end > renderer->end) { \
			end = renderer->end; \
		} \
		if (UNLIKELY(end == outX)) { \
			return; \
		} \
		if (UNLIKELY(end < outX)) { \
			mLOG(GBA_VIDEO, FATAL, "Out of bounds background draw!"); \
			return; \
		} \
		DRAW_BACKGROUND_MODE_0_TILE_SUFFIX_ ## BPP (BLEND, OBJWIN) \
		outX = end; \
		if (tileX < tileEnd) { \
			++tileX; \
		} else if (VIDEO_CHECKS && UNLIKELY(tileX > tileEnd)) { \
			mLOG(GBA_VIDEO, FATAL, "Invariant doesn't hold in background draw! tileX (%u) > tileEnd (%u)", tileX, tileEnd); \
			return; \
		} \
		length -= end - renderer->start; \
	} \
	/*! TODO: Make sure these lines can be removed */ \
	/*!*/ pixel = &renderer->row[outX]; \
	outX += (tileEnd - tileX) * 8; \
	/*!*/ if (VIDEO_CHECKS &&  UNLIKELY(outX > GBA_VIDEO_HORIZONTAL_PIXELS)) { \
	/*!*/	mLOG(GBA_VIDEO, FATAL, "Out of bounds background draw would occur!"); \
	/*!*/	return; \
	/*!*/ } \
	localX = (tileX * 8 + inX) & 0x1FF; \
	DRAW_BACKGROUND_MODE_0_TILES_ ## BPP (BLEND, OBJWIN) \
	if (length & 0x7) { \
		localX = tileX * 8 + inX; \
		BACKGROUND_TEXT_SELECT_CHARACTER; \
		localY = inY & 0x7; \
		if (GBA_TEXT_MAP_VFLIP(mapData)) { \
			localY = 7 - localY; \
		} \
		int mod8 = length & 0x7; \
		if (VIDEO_CHECKS && UNLIKELY(outX + mod8 != renderer->end)) { \
			mLOG(GBA_VIDEO, FATAL, "Invariant doesn't hold in background draw!"); \
			return; \
		} \
		DRAW_BACKGROUND_MODE_0_TILE_PREFIX_ ## BPP (BLEND, OBJWIN) \
	} \
	if (VIDEO_CHECKS && UNLIKELY(&renderer->row[outX] != pixel)) { \
		mLOG(GBA_VIDEO, FATAL, "Background draw ended in the wrong place! Diff: %" PRIXPTR, &renderer->row[outX] - pixel); \
	} \
	if (VIDEO_CHECKS && UNLIKELY(outX > GBA_VIDEO_HORIZONTAL_PIXELS)) { \
		mLOG(GBA_VIDEO, FATAL, "Out of bounds background draw occurred!"); \
		return; \
	}

/* Each blend/objwin/depth combination is its own function. Combined, they are
 * far larger than small instruction caches (e.g. 8KiB on SH-4), but only one
 * runs per background line. */
#define DEFINE_BACKGROUND_MODE_0(BPP, BLEND, OBJWIN) \
	static ATTRIBUTE_NOINLINE void _drawBackgroundMode0_ ## BPP ## _ ## BLEND ## _ ## OBJWIN( \
		struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, \
		int inX, int inY, int length, unsigned yBase, color_t* mainPalette) { \
		uint16_t mapData; \
		int localX; \
		int localY; \
		unsigned xBase; \
		uint32_t flags = background->flags; \
		uint32_t objwinFlags = background->objwinFlags; \
		uint32_t screenBase; \
		uint32_t charBase; \
		color_t* palette = mainPalette; \
		PREPARE_OBJWIN; \
		UNUSED(objwinSlowPath); \
		UNUSED(objwinOnly); \
		UNUSED(objwinFlags); \
		int outX = renderer->start; \
		uint32_t tileData; \
		uint32_t current; \
		int pixelData; \
		int paletteData; \
		UNUSED(paletteData); \
		int tileX = 0; \
		int tileEnd = ((length + inX) >> 3) - (inX >> 3); \
		uint16_t* vram = renderer->d.vram; \
		DRAW_BACKGROUND_MODE_0(BPP, BLEND, OBJWIN); \
	}

DEFINE_BACKGROUND_MODE_0(16, NoBlend, NO_OBJWIN)
DEFINE_BACKGROUND_MODE_0(256, NoBlend, NO_OBJWIN)
DEFINE_BACKGROUND_MODE_0(16, Blend, NO_OBJWIN)
DEFINE_BACKGROUND_MODE_0(256, Blend, NO_OBJWIN)
DEFINE_BACKGROUND_MODE_0(16, NoBlend, OBJWIN)
DEFINE_BACKGROUND_MODE_0(256, NoBlend, OBJWIN)
DEFINE_BACKGROUND_MODE_0(16, Blend, OBJWIN)
DEFINE_BACKGROUND_MODE_0(256, Blend, OBJWIN)

void GBAVideoSoftwareRendererDrawBackgroundMode0(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y) {
	int inX = (renderer->start + background->x - background->offsetX) & 0x1FF;
	int length = renderer->end - renderer->start;
	if (background->mosaic) {
		int mosaicV = GBAMosaicControlGetBgV(renderer->mosaic) + 1;
		y -= y % mosaicV;
	}
	int inY = y + background->y - background->offsetY;
	uint16_t mapData;

	unsigned yBase = inY & 0xF8;
	if (background->size == 2) {
		yBase += inY & 0x100;
	} else if (background->size == 3) {
		yBase += (inY & 0x100) << 1;
	}
	yBase = (background->screenBase >> 1) + (yBase << 2);

	int localX;
	unsigned xBase;
	uint32_t screenBase;
	uint32_t flags = background->flags;
	bool variant = background->variant;

	color_t* mainPalette = renderer->normalPalette;
	if (renderer->d.highlightAmount && background->highlight) {
		mainPalette = renderer->highlightPalette;
	}
	if (variant) {
		mainPalette = renderer->variantPalette;
		if (renderer->d.highlightAmount && background->highlight) {
			mainPalette = renderer->highlightVariantPalette;
		}
	}
	bool objwinSlowPath = GBARegisterDISPCNTIsObjwinEnable(renderer->dispcnt);
	uint16_t* vram = renderer->d.vram;

	if (background->yCache != inY >> 3) {
		int tileX;
		localX = 0;
		for (tileX = 0; tileX < 64; ++tileX, localX += 8) {
			BACKGROUND_TEXT_SELECT_CHARACTER;
			background->mapCache[tileX] = mapData;
		}
		background->yCache = inY >> 3;
	}

#ifdef M_PROFILE
	bool profileMosaic = background->mosaic && GBAMosaicControlGetBgH(renderer->mosaic);
#endif
#define CALL_BACKGROUND_MODE_0(BPP, BLEND, OBJWIN) \
	do { \
		mPROFILE_START(profileBackground, profileMosaic ? \
			"bg mode0 " #BPP "col " #BLEND " " #OBJWIN " mosaic" : \
			"bg mode0 " #BPP "col " #BLEND " " #OBJWIN); \
		_drawBackgroundMode0_ ## BPP ## _ ## BLEND ## _ ## OBJWIN(renderer, background, inX, inY, length, yBase, mainPalette); \
		mPROFILE_STOP(profileBackground); \
	} while (0)

	if (!objwinSlowPath) {
		if (!(flags & FLAG_TARGET_2)) {
			if (!background->multipalette) {
				CALL_BACKGROUND_MODE_0(16, NoBlend, NO_OBJWIN);
			} else {
				CALL_BACKGROUND_MODE_0(256, NoBlend, NO_OBJWIN);
			}
		} else {
			if (!background->multipalette) {
				CALL_BACKGROUND_MODE_0(16, Blend, NO_OBJWIN);
			} else {
				CALL_BACKGROUND_MODE_0(256, Blend, NO_OBJWIN);
			}
		}
	} else {
		if (!(flags & FLAG_TARGET_2)) {
			if (!background->multipalette) {
				CALL_BACKGROUND_MODE_0(16, NoBlend, OBJWIN);
			} else {
				CALL_BACKGROUND_MODE_0(256, NoBlend, OBJWIN);
			}
		} else {
			if (!background->multipalette) {
				CALL_BACKGROUND_MODE_0(16, Blend, OBJWIN);
			} else {
				CALL_BACKGROUND_MODE_0(256, Blend, OBJWIN);
			}
		}
	}
#undef CALL_BACKGROUND_MODE_0
}
