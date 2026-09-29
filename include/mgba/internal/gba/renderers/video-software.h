/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef VIDEO_SOFTWARE_H
#define VIDEO_SOFTWARE_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/core/core.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/renderers/common.h>
#include <mgba/internal/gba/video.h>

struct GBAVideoSoftwareBackground {
	unsigned index;
	int enabled;
	unsigned priority;
	uint32_t charBase;
	int mosaic;
	int multipalette;
	uint32_t screenBase;
	int overflow;
	int size;
	int target1;
	int target2;
	uint16_t x;
	uint16_t y;
	int32_t refx;
	int32_t refy;
	int16_t dx;
	int16_t dmx;
	int16_t dy;
	int16_t dmy;
	int32_t sx;
	int32_t sy;
	int yCache;
	uint16_t mapCache[64];
	uint32_t flags;
	uint32_t objwinFlags;
	bool variant;
	int32_t offsetX;
	int32_t offsetY;
	bool highlight;
};

enum {
	OFFSET_PRIORITY = 30,
	OFFSET_INDEX = 28,
};

#define FLAG_PRIORITY       0xC0000000
#define FLAG_INDEX          0x30000000
#define FLAG_IS_BACKGROUND  0x08000000
#define FLAG_UNWRITTEN      0xFC000000
#define FLAG_REBLEND        0x04000000
#define FLAG_TARGET_1       0x02000000
#define FLAG_TARGET_2       0x01000000
#define FLAG_OBJWIN         0x01000000
#define FLAG_ORDER_MASK     0xF8000000

#define IS_WRITABLE(PIXEL) ((PIXEL) & 0xFE000000)

struct WindowControl {
	GBAWindowControl packed;
	int8_t priority;
};

#define MAX_WINDOW 5

struct Window {
	uint8_t endX;
	struct WindowControl control;
};

#define GBA_VIDEO_SOFTWARE_DEFERRED_MAX 1024

struct GBAVideoSoftwareRenderer {
	struct GBAVideoRenderer d;

	color_t* outputBuffer;
	int outputBufferStride;

	uint32_t* temporaryBuffer;

	GBARegisterDISPCNT dispcnt;

	uint32_t row[GBA_VIDEO_HORIZONTAL_PIXELS];
	uint32_t spriteLayer[GBA_VIDEO_HORIZONTAL_PIXELS];
	int32_t spriteCyclesRemaining;

	// BLDCNT
	unsigned target1Obj;
	unsigned target1Bd;
	unsigned target2Obj;
	unsigned target2Bd;
	bool blendDirty;
	enum GBAVideoBlendEffect blendEffect;
	color_t normalPalette[512];
	color_t variantPalette[512];
	color_t highlightPalette[512];
	color_t highlightVariantPalette[512];

	uint16_t blda;
	uint16_t bldb;
	uint16_t bldy;

	GBAMosaicControl mosaic;
	bool greenswap;

	struct WindowN {
		struct GBAVideoWindowRegion h;
		struct GBAVideoWindowRegion v;
		struct WindowControl control;
		int16_t offsetX;
		int16_t offsetY;
	} winN[2];

	struct WindowControl winout;
	struct WindowControl objwin;

	struct WindowControl currentWindow;

	int nWindows;
	struct Window windows[MAX_WINDOW];

	struct GBAVideoSoftwareBackground bg[4];

	bool forceTarget1;
	bool oamDirty;
	int oamMax;
	struct GBAVideoRendererSprite sprites[128];
	int16_t objOffsetX;
	int16_t objOffsetY;

	uint32_t scanlineDirty[5];
	uint16_t nextIo[REG_SOUND1CNT_LO >> 1];
	struct ScanlineCache {
		uint16_t io[REG_SOUND1CNT_LO >> 1];
		int32_t scale[2][2];
	} cache[GBA_VIDEO_VERTICAL_PIXELS];
	int nextY;

	int start;
	int end;

	uint8_t lastHighlightAmount;

	// Draw simple lines back to front straight into outputBuffer
	bool fastPath;

	// Hold scanlines, and the register writes between them, back until the
	// frame is done or video memory is about to change, and draw them all at
	// once. Only for a renderer the core talks to directly.
	bool deferLines;
	int nDeferred;
	struct GBAVideoSoftwareDeferred {
		uint16_t address;
		uint16_t value;
	} deferred[GBA_VIDEO_SOFTWARE_DEFERRED_MAX];
	uint32_t fastLines;
	uint32_t slowLines;

	// Filled in with the sprite layer: which 8 pixel blocks of it may hold
	// something, and which may hold something of each priority
	uint32_t spriteBlocks;
	uint32_t spritePriorityBlocks[4];
	// The blocks of the sprite layer that aren't known to be clear
	uint32_t spriteLayerDirty;

	// The sprites on each line, as indices into sprites and in the same
	// order. Line y has spriteLines[spriteLineStart[y]] up to
	// spriteLines[spriteLineStart[y + 1]]
	uint16_t spriteLineStart[GBA_VIDEO_VERTICAL_PIXELS + 1];
	uint8_t spriteLines[128 * GBA_VIDEO_VERTICAL_PIXELS];

	// What the fast path needs of each of sprites, worked out when OAM or
	// the registers behind fastSpriteKey change instead of on every line
	bool fastSpritesValid;
	struct GBAVideoSoftwareFastSpriteKey {
		const color_t* palette[2];
		uint32_t flags[2];
		int16_t offsetX;
		int16_t offsetY;
		uint8_t force[2];
		uint8_t mapping;
		uint8_t bitmap;
	} fastSpriteKey;
	struct GBAVideoSoftwareFastSprite {
		const color_t* palette;
		uint32_t flags;
		// Affine: the accumulators on the sprite's first line
		int32_t xAccum;
		int32_t yAccum;
		int16_t a;
		int16_t b;
		int16_t c;
		int16_t d;
		int16_t y;
		int16_t outX;
		int16_t condition;
		int16_t inX;
		int16_t cycles;
		uint16_t charBase;
		uint16_t maskLo;
		uint16_t maskHi;
		uint8_t width;
		uint8_t height;
		uint8_t strideShift;
		uint8_t priority;
		uint8_t kind;
	} fastSprites[128];

	// The sprites of a line that go straight into the output row, in OAM
	// order
	bool fastSpritesDirect;
	int nFastLine;
	struct GBAVideoSoftwareFastLine {
		// yBase for a regular sprite
		int32_t xAccum;
		int32_t yAccum;
		int16_t outX;
		uint8_t index;
		uint8_t priority;
	} fastLine[128];
};

void GBAVideoSoftwareRendererCreate(struct GBAVideoSoftwareRenderer* renderer);

CXX_GUARD_START

#endif
