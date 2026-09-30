/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba/renderers/software-private.h"

#include <mgba/core/profile.h>

#include <mgba/core/cache-set.h>
#include <mgba/internal/arm/macros.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/renderers/cache-set.h>

#include <mgba-util/memory.h>

#ifdef M_PROFILE
// Lines the fast compositor took, and those it didn't
uint32_t GBAVideoLineCount[2];
#endif

// Set by a hardware renderer (the Dreamcast PVR path): each line's registers
// go to the hook instead of being drawn. The bitmap marks VRAM written, one
// bit per 32 bytes.
void (*GBAVideoSoftwareLineHook)(struct GBAVideoSoftwareRenderer* renderer, int y);
// Only this renderer is hooked; the others draw as usual (NULL: all)
struct GBAVideoSoftwareRenderer* GBAVideoSoftwareLineHookTarget;
uint32_t GBAVideoSoftwareVRAMDirty[96];

#define DIRTY_SCANLINE(R, Y) R->scanlineDirty[Y >> 5] |= (1U << (Y & 0x1F))
#define CLEAN_SCANLINE(R, Y) R->scanlineDirty[Y >> 5] &= ~(1U << (Y & 0x1F))

static void GBAVideoSoftwareRendererInit(struct GBAVideoRenderer* renderer);
static void GBAVideoSoftwareRendererDeinit(struct GBAVideoRenderer* renderer);
static void GBAVideoSoftwareRendererReset(struct GBAVideoRenderer* renderer);
static void GBAVideoSoftwareRendererWriteVRAM(struct GBAVideoRenderer* renderer, uint32_t address);
static void GBAVideoSoftwareRendererWriteOAM(struct GBAVideoRenderer* renderer, uint32_t oam);
static void GBAVideoSoftwareRendererWritePalette(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value);
static uint16_t GBAVideoSoftwareRendererWriteVideoRegister(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value);
static void GBAVideoSoftwareRendererDrawScanline(struct GBAVideoRenderer* renderer, int y);
static void GBAVideoSoftwareRendererFlushLines(struct GBAVideoRenderer* renderer);
static uint16_t _writeVideoRegisterNow(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value);
static void _drawScanlineNow(struct GBAVideoRenderer* renderer, int y);
static void GBAVideoSoftwareRendererFinishFrame(struct GBAVideoRenderer* renderer);
static void GBAVideoSoftwareRendererGetPixels(struct GBAVideoRenderer* renderer, size_t* stride, const void** pixels);
static void GBAVideoSoftwareRendererPutPixels(struct GBAVideoRenderer* renderer, size_t stride, const void* pixels);

static void GBAVideoSoftwareRendererUpdateDISPCNT(struct GBAVideoSoftwareRenderer* renderer);
static void GBAVideoSoftwareRendererWriteBGCNT(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBGX_LO(struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBGX_HI(struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBGY_LO(struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBGY_HI(struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBLDCNT(struct GBAVideoSoftwareRenderer* renderer, uint16_t value);

static void GBAVideoSoftwareRendererPreprocessBuffer(struct GBAVideoSoftwareRenderer* renderer, int y, bool fillRow);
static void _fillBackdrop(struct GBAVideoSoftwareRenderer* renderer);
static void GBAVideoSoftwareRendererPostprocessBuffer(struct GBAVideoSoftwareRenderer* renderer);
static int GBAVideoSoftwareRendererPreprocessSpriteLayer(struct GBAVideoSoftwareRenderer* renderer, int y, bool fast);

static void _updatePalettes(struct GBAVideoSoftwareRenderer* renderer);
static void _updateFlags(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* bg);

static void _breakWindow(struct GBAVideoSoftwareRenderer* softwareRenderer, struct WindowN* win, int y);
static void _breakWindowInner(struct GBAVideoSoftwareRenderer* softwareRenderer, struct WindowN* win);

void GBAVideoSoftwareRendererCreate(struct GBAVideoSoftwareRenderer* renderer) {
	renderer->d.init = GBAVideoSoftwareRendererInit;
	renderer->d.reset = GBAVideoSoftwareRendererReset;
	renderer->d.deinit = GBAVideoSoftwareRendererDeinit;
	renderer->d.writeVideoRegister = GBAVideoSoftwareRendererWriteVideoRegister;
	renderer->d.writeVRAM = GBAVideoSoftwareRendererWriteVRAM;
	renderer->d.coarseVRAM = true;
	renderer->d.pendingLines = 0;
	renderer->d.flushLines = GBAVideoSoftwareRendererFlushLines;
	renderer->d.writeOAM = GBAVideoSoftwareRendererWriteOAM;
	renderer->d.writePalette = GBAVideoSoftwareRendererWritePalette;
	renderer->d.drawScanline = GBAVideoSoftwareRendererDrawScanline;
	renderer->d.finishFrame = GBAVideoSoftwareRendererFinishFrame;
	renderer->d.getPixels = GBAVideoSoftwareRendererGetPixels;
	renderer->d.putPixels = GBAVideoSoftwareRendererPutPixels;

	renderer->d.disableBG[0] = false;
	renderer->d.disableBG[1] = false;
	renderer->d.disableBG[2] = false;
	renderer->d.disableBG[3] = false;
	renderer->d.disableOBJ = false;
	renderer->d.disableWIN[0] = false;
	renderer->d.disableWIN[1] = false;
	renderer->d.disableOBJWIN = false;

	renderer->d.highlightBG[0] = false;
	renderer->d.highlightBG[1] = false;
	renderer->d.highlightBG[2] = false;
	renderer->d.highlightBG[3] = false;
	int i;
	for (i = 0; i < 128; ++i) {
		renderer->d.highlightOBJ[i] = false;
	}
	renderer->d.highlightColor = M_COLOR_WHITE;
	renderer->d.highlightAmount = 0;

	renderer->temporaryBuffer = 0;
	renderer->fastPath = true;
#ifdef _arch_dreamcast
	renderer->deferLines = true;
#else
	renderer->deferLines = false;
#endif
	renderer->nDeferred = 0;
	renderer->fastLines = 0;
	renderer->slowLines = 0;
}

static void GBAVideoSoftwareRendererInit(struct GBAVideoRenderer* renderer) {
	GBAVideoSoftwareRendererReset(renderer);

	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;

	int y;
	for (y = 0; y < GBA_VIDEO_VERTICAL_PIXELS; ++y) {
		color_t* row = &softwareRenderer->outputBuffer[softwareRenderer->outputBufferStride * y];
		int x;
		for (x = 0; x < GBA_VIDEO_HORIZONTAL_PIXELS; ++x) {
			row[x] = M_COLOR_WHITE;
		}
	}
}

static void GBAVideoSoftwareRendererReset(struct GBAVideoRenderer* renderer) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	int i;

	softwareRenderer->nDeferred = 0;
	renderer->pendingLines = 0;

	softwareRenderer->dispcnt = 0x0080;

	softwareRenderer->target1Obj = 0;
	softwareRenderer->target1Bd = 0;
	softwareRenderer->target2Obj = 0;
	softwareRenderer->target2Bd = 0;
	softwareRenderer->blendEffect = BLEND_NONE;
	for (i = 0; i < 1024; i += 2) {
		uint16_t entry;
		LOAD_16(entry, i, softwareRenderer->d.palette);
		GBAVideoSoftwareRendererWritePalette(renderer, i, entry);
	}
	softwareRenderer->blendDirty = false;
	_updatePalettes(softwareRenderer);

	softwareRenderer->blda = 0;
	softwareRenderer->bldb = 0;
	softwareRenderer->bldy = 0;

	softwareRenderer->winN[0] = (struct WindowN) { .control = { .priority = 0 } };
	softwareRenderer->winN[1] = (struct WindowN) { .control = { .priority = 1 } };
	softwareRenderer->objwin = (struct WindowControl) { .priority = 2 };
	softwareRenderer->winout = (struct WindowControl) { .priority = 3 };
	softwareRenderer->oamDirty = 1;
	softwareRenderer->spriteLayerDirty = 0xFFFFFFFF;
	softwareRenderer->oamMax = 0;

	softwareRenderer->mosaic = 0;
	softwareRenderer->greenswap = false;
	softwareRenderer->nextY = 0;

	softwareRenderer->objOffsetX = 0;
	softwareRenderer->objOffsetY = 0;

	memset(softwareRenderer->scanlineDirty, 0xFFFFFFFF, sizeof(softwareRenderer->scanlineDirty));
	memset(softwareRenderer->cache, 0, sizeof(softwareRenderer->cache));
	memset(softwareRenderer->nextIo, 0, sizeof(softwareRenderer->nextIo));

	softwareRenderer->lastHighlightAmount = 0;

	for (i = 0; i < 4; ++i) {
		struct GBAVideoSoftwareBackground* bg = &softwareRenderer->bg[i];
		memset(bg, 0, sizeof(*bg));
		bg->index = i;
		bg->dx = 256;
		bg->dmy = 256;
		bg->yCache = -1;
	}
}

static void GBAVideoSoftwareRendererDeinit(struct GBAVideoRenderer* renderer) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	softwareRenderer->nDeferred = 0;
	renderer->pendingLines = 0;
	UNUSED(softwareRenderer);
}

// What a register reads back as, which is all the core needs to know right
// away. Same masks as _writeVideoRegisterNow applies.
static uint16_t _maskVideoRegister(uint32_t address, uint16_t value) {
	switch (address) {
	case REG_DISPCNT:
		return value & 0xFFF7;
	case REG_BG0CNT:
	case REG_BG1CNT:
		return value & 0xDFFF;
	case REG_BG0HOFS:
	case REG_BG0VOFS:
	case REG_BG1HOFS:
	case REG_BG1VOFS:
	case REG_BG2HOFS:
	case REG_BG2VOFS:
	case REG_BG3HOFS:
	case REG_BG3VOFS:
		return value & 0x01FF;
	case REG_BLDCNT:
		return value & 0x3FFF;
	case REG_BLDALPHA:
		return value & 0x1F1F;
	case REG_BLDY:
		value &= 0x1F;
		return value > 0x10 ? 0x10 : value;
	case REG_WININ:
	case REG_WINOUT:
		return value & 0x3F3F;
	default:
		return value;
	}
}

static void _defer(struct GBAVideoSoftwareRenderer* softwareRenderer, uint16_t address, uint16_t value) {
	softwareRenderer->deferred[softwareRenderer->nDeferred].address = address;
	softwareRenderer->deferred[softwareRenderer->nDeferred].value = value;
	++softwareRenderer->nDeferred;
}

// No register is at an odd address
#define DEFERRED_LINE 0xFFFF

static void GBAVideoSoftwareRendererFlushLines(struct GBAVideoRenderer* renderer) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	int n = softwareRenderer->nDeferred;
	softwareRenderer->nDeferred = 0;
	renderer->pendingLines = 0;
	int i;
	mPROFILE_ADD(pFlush, "vid: line flushes", 1);
	for (i = 0; i < n; ++i) {
		struct GBAVideoSoftwareDeferred* deferred = &softwareRenderer->deferred[i];
		if (deferred->address == DEFERRED_LINE) {
			mPROFILE_ADD(pLines, "vid: lines flushed", 1);
			_drawScanlineNow(renderer, deferred->value);
		} else {
#ifdef M_PROFILE
			unsigned a = deferred->address;
			if (a >= REG_BG2PA && a < REG_WIN0H) {
				mPROFILE_ADD(pAff, "vid: replay affine regs", 1);
			} else if (a >= REG_BG0HOFS && a < REG_BG2PA) {
				mPROFILE_ADD(pScroll, "vid: replay scroll regs", 1);
			} else if (a >= REG_BLDCNT && a <= REG_BLDY) {
				mPROFILE_ADD(pBld, "vid: replay blend regs", 1);
			} else {
				mPROFILE_ADD(pOther, "vid: replay other regs", 1);
			}
#endif
			mPROFILE_START(profileReplay, "register replay");
			_writeVideoRegisterNow(renderer, deferred->address, deferred->value);
			mPROFILE_STOP(profileReplay);
		}
	}
}

static uint16_t GBAVideoSoftwareRendererWriteVideoRegister(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	if (renderer->pendingLines) {
		// Has to wait its turn behind the lines that came before it
		if (softwareRenderer->nDeferred < GBA_VIDEO_SOFTWARE_DEFERRED_MAX) {
			value = _maskVideoRegister(address, value);
			_defer(softwareRenderer, address, value);
			return value;
		}
		GBAVideoSoftwareRendererFlushLines(renderer);
	}
	return _writeVideoRegisterNow(renderer, address, value);
}

static void GBAVideoSoftwareRendererDrawScanline(struct GBAVideoRenderer* renderer, int y) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	if (!softwareRenderer->deferLines) {
		_drawScanlineNow(renderer, y);
		return;
	}
	if (softwareRenderer->nDeferred == GBA_VIDEO_SOFTWARE_DEFERRED_MAX) {
		GBAVideoSoftwareRendererFlushLines(renderer);
	}
	_defer(softwareRenderer, DEFERRED_LINE, y);
	++renderer->pendingLines;
}

static uint16_t _writeVideoRegisterNow(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	if (renderer->cache) {
		GBAVideoCacheWriteVideoRegister(renderer->cache, address, value);
	}

	switch (address) {
	case REG_DISPCNT:
		value &= 0xFFF7;
		softwareRenderer->dispcnt = value;
		GBAVideoSoftwareRendererUpdateDISPCNT(softwareRenderer);
		break;
	case REG_GREENSWP:
		softwareRenderer->greenswap = value & 1;
		break;
	case REG_BG0CNT:
		value &= 0xDFFF;
		GBAVideoSoftwareRendererWriteBGCNT(softwareRenderer, &softwareRenderer->bg[0], value);
		break;
	case REG_BG1CNT:
		value &= 0xDFFF;
		GBAVideoSoftwareRendererWriteBGCNT(softwareRenderer, &softwareRenderer->bg[1], value);
		break;
	case REG_BG2CNT:
		value &= 0xFFFF;
		GBAVideoSoftwareRendererWriteBGCNT(softwareRenderer, &softwareRenderer->bg[2], value);
		break;
	case REG_BG3CNT:
		value &= 0xFFFF;
		GBAVideoSoftwareRendererWriteBGCNT(softwareRenderer, &softwareRenderer->bg[3], value);
		break;
	case REG_BG0HOFS:
		value &= 0x01FF;
		softwareRenderer->bg[0].x = value;
		break;
	case REG_BG0VOFS:
		value &= 0x01FF;
		softwareRenderer->bg[0].y = value;
		break;
	case REG_BG1HOFS:
		value &= 0x01FF;
		softwareRenderer->bg[1].x = value;
		break;
	case REG_BG1VOFS:
		value &= 0x01FF;
		softwareRenderer->bg[1].y = value;
		break;
	case REG_BG2HOFS:
		value &= 0x01FF;
		softwareRenderer->bg[2].x = value;
		break;
	case REG_BG2VOFS:
		value &= 0x01FF;
		softwareRenderer->bg[2].y = value;
		break;
	case REG_BG3HOFS:
		value &= 0x01FF;
		softwareRenderer->bg[3].x = value;
		break;
	case REG_BG3VOFS:
		value &= 0x01FF;
		softwareRenderer->bg[3].y = value;
		break;
	case REG_BG2PA:
		softwareRenderer->bg[2].dx = value;
		break;
	case REG_BG2PB:
		softwareRenderer->bg[2].dmx = value;
		break;
	case REG_BG2PC:
		softwareRenderer->bg[2].dy = value;
		break;
	case REG_BG2PD:
		softwareRenderer->bg[2].dmy = value;
		break;
	case REG_BG2X_LO:
		GBAVideoSoftwareRendererWriteBGX_LO(&softwareRenderer->bg[2], value);
		if (softwareRenderer->bg[2].sx != softwareRenderer->cache[softwareRenderer->nextY].scale[0][0]) {
			DIRTY_SCANLINE(softwareRenderer, softwareRenderer->nextY);
		}
		break;
	case REG_BG2X_HI:
		GBAVideoSoftwareRendererWriteBGX_HI(&softwareRenderer->bg[2], value);
		if (softwareRenderer->bg[2].sx != softwareRenderer->cache[softwareRenderer->nextY].scale[0][0]) {
			DIRTY_SCANLINE(softwareRenderer, softwareRenderer->nextY);
		}
		break;
	case REG_BG2Y_LO:
		GBAVideoSoftwareRendererWriteBGY_LO(&softwareRenderer->bg[2], value);
		if (softwareRenderer->bg[2].sy != softwareRenderer->cache[softwareRenderer->nextY].scale[0][1]) {
			DIRTY_SCANLINE(softwareRenderer, softwareRenderer->nextY);
		}
		break;
	case REG_BG2Y_HI:
		GBAVideoSoftwareRendererWriteBGY_HI(&softwareRenderer->bg[2], value);
		if (softwareRenderer->bg[2].sy != softwareRenderer->cache[softwareRenderer->nextY].scale[0][1]) {
			DIRTY_SCANLINE(softwareRenderer, softwareRenderer->nextY);
		}
		break;
	case REG_BG3PA:
		softwareRenderer->bg[3].dx = value;
		break;
	case REG_BG3PB:
		softwareRenderer->bg[3].dmx = value;
		break;
	case REG_BG3PC:
		softwareRenderer->bg[3].dy = value;
		break;
	case REG_BG3PD:
		softwareRenderer->bg[3].dmy = value;
		break;
	case REG_BG3X_LO:
		GBAVideoSoftwareRendererWriteBGX_LO(&softwareRenderer->bg[3], value);
		if (softwareRenderer->bg[3].sx != softwareRenderer->cache[softwareRenderer->nextY].scale[1][0]) {
			DIRTY_SCANLINE(softwareRenderer, softwareRenderer->nextY);
		}
		break;
	case REG_BG3X_HI:
		GBAVideoSoftwareRendererWriteBGX_HI(&softwareRenderer->bg[3], value);
		if (softwareRenderer->bg[3].sx != softwareRenderer->cache[softwareRenderer->nextY].scale[1][0]) {
			DIRTY_SCANLINE(softwareRenderer, softwareRenderer->nextY);
		}
		break;
	case REG_BG3Y_LO:
		GBAVideoSoftwareRendererWriteBGY_LO(&softwareRenderer->bg[3], value);
		if (softwareRenderer->bg[3].sy != softwareRenderer->cache[softwareRenderer->nextY].scale[1][1]) {
			DIRTY_SCANLINE(softwareRenderer, softwareRenderer->nextY);
		}
		break;
	case REG_BG3Y_HI:
		GBAVideoSoftwareRendererWriteBGY_HI(&softwareRenderer->bg[3], value);
		if (softwareRenderer->bg[3].sy != softwareRenderer->cache[softwareRenderer->nextY].scale[1][1]) {
			DIRTY_SCANLINE(softwareRenderer, softwareRenderer->nextY);
		}
		break;
	case REG_BLDCNT:
		GBAVideoSoftwareRendererWriteBLDCNT(softwareRenderer, value);
		value &= 0x3FFF;
		break;
	case REG_BLDALPHA:
		softwareRenderer->blda = value & 0x1F;
		if (softwareRenderer->blda > 0x10) {
			softwareRenderer->blda = 0x10;
		}
		softwareRenderer->bldb = (value >> 8) & 0x1F;
		if (softwareRenderer->bldb > 0x10) {
			softwareRenderer->bldb = 0x10;
		}
		value &= 0x1F1F;
		break;
	case REG_BLDY:
		value &= 0x1F;
		if (value > 0x10) {
			value = 0x10;
		}
		if (softwareRenderer->bldy != value) {
			softwareRenderer->bldy = value;
			softwareRenderer->blendDirty = true;
		}
		break;
	case REG_WIN0H:
		softwareRenderer->winN[0].h.end = value;
		softwareRenderer->winN[0].h.start = value >> 8;
		if (softwareRenderer->winN[0].h.start > GBA_VIDEO_HORIZONTAL_PIXELS && softwareRenderer->winN[0].h.start > softwareRenderer->winN[0].h.end) {
			softwareRenderer->winN[0].h.start = 0;
		}
		if (softwareRenderer->winN[0].h.end > GBA_VIDEO_HORIZONTAL_PIXELS) {
			softwareRenderer->winN[0].h.end = GBA_VIDEO_HORIZONTAL_PIXELS;
			if (softwareRenderer->winN[0].h.start > GBA_VIDEO_HORIZONTAL_PIXELS) {
				softwareRenderer->winN[0].h.start = GBA_VIDEO_HORIZONTAL_PIXELS;
			}
		}
		break;
	case REG_WIN1H:
		softwareRenderer->winN[1].h.end = value;
		softwareRenderer->winN[1].h.start = value >> 8;
		if (softwareRenderer->winN[1].h.start > GBA_VIDEO_HORIZONTAL_PIXELS && softwareRenderer->winN[1].h.start > softwareRenderer->winN[1].h.end) {
			softwareRenderer->winN[1].h.start = 0;
		}
		if (softwareRenderer->winN[1].h.end > GBA_VIDEO_HORIZONTAL_PIXELS) {
			softwareRenderer->winN[1].h.end = GBA_VIDEO_HORIZONTAL_PIXELS;
			if (softwareRenderer->winN[1].h.start > GBA_VIDEO_HORIZONTAL_PIXELS) {
				softwareRenderer->winN[1].h.start = GBA_VIDEO_HORIZONTAL_PIXELS;
			}
		}
		break;
	case REG_WIN0V:
		softwareRenderer->winN[0].v.end = value;
		softwareRenderer->winN[0].v.start = value >> 8;
		if (softwareRenderer->winN[0].v.start > GBA_VIDEO_VERTICAL_PIXELS && softwareRenderer->winN[0].v.start > softwareRenderer->winN[0].v.end) {
			softwareRenderer->winN[0].v.start = 0;
		}
		if (softwareRenderer->winN[0].v.end > GBA_VIDEO_VERTICAL_PIXELS) {
			softwareRenderer->winN[0].v.end = GBA_VIDEO_VERTICAL_PIXELS;
			if (softwareRenderer->winN[0].v.start > GBA_VIDEO_VERTICAL_PIXELS) {
				softwareRenderer->winN[0].v.start = GBA_VIDEO_VERTICAL_PIXELS;
			}
		}
		break;
	case REG_WIN1V:
		softwareRenderer->winN[1].v.end = value;
		softwareRenderer->winN[1].v.start = value >> 8;
		if (softwareRenderer->winN[1].v.start > GBA_VIDEO_VERTICAL_PIXELS && softwareRenderer->winN[1].v.start > softwareRenderer->winN[1].v.end) {
			softwareRenderer->winN[1].v.start = 0;
		}
		if (softwareRenderer->winN[1].v.end > GBA_VIDEO_VERTICAL_PIXELS) {
			softwareRenderer->winN[1].v.end = GBA_VIDEO_VERTICAL_PIXELS;
			if (softwareRenderer->winN[1].v.start > GBA_VIDEO_VERTICAL_PIXELS) {
				softwareRenderer->winN[1].v.start = GBA_VIDEO_VERTICAL_PIXELS;
			}
		}
		break;
	case REG_WININ:
		value &= 0x3F3F;
		softwareRenderer->winN[0].control.packed = value;
		softwareRenderer->winN[1].control.packed = value >> 8;
		break;
	case REG_WINOUT:
		value &= 0x3F3F;
		softwareRenderer->winout.packed = value;
		softwareRenderer->objwin.packed = value >> 8;
		break;
	case REG_MOSAIC:
		softwareRenderer->mosaic = value;
		break;
	default:
		mLOG(GBA_VIDEO, GAME_ERROR, "Invalid video register: 0x%03" PRIX32, address);
	}
	softwareRenderer->nextIo[address >> 1] = value;
	if (softwareRenderer->cache[softwareRenderer->nextY].io[address >> 1] != value) {
		softwareRenderer->cache[softwareRenderer->nextY].io[address >> 1] = value;
		DIRTY_SCANLINE(softwareRenderer, softwareRenderer->nextY);
	}
	return value;
}

static void GBAVideoSoftwareRendererWriteVRAM(struct GBAVideoRenderer* renderer, uint32_t address) {
	mPROFILE_ADD(pVram, "cpu: VRAM writes", 1);
	if (renderer->pendingLines) {
		mPROFILE_ADD(pVramFlush, "cpu: VRAM writes forcing flush", 1);
	}
	{
		uint32_t unit = (address >= 0x18000 ? address - 0x8000 : address) >> 5;
		GBAVideoSoftwareVRAMDirty[unit >> 5] |= 1U << (unit & 31);
	}
	GBA_VIDEO_TOUCH(renderer);
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	if (renderer->cache) {
		mCacheSetWriteVRAM(renderer->cache, address);
	}
	memset(softwareRenderer->scanlineDirty, 0xFFFFFFFF, sizeof(softwareRenderer->scanlineDirty));
	softwareRenderer->bg[0].yCache = -1;
	softwareRenderer->bg[1].yCache = -1;
	softwareRenderer->bg[2].yCache = -1;
	softwareRenderer->bg[3].yCache = -1;
}

static void GBAVideoSoftwareRendererWriteOAM(struct GBAVideoRenderer* renderer, uint32_t oam) {
	mPROFILE_ADD(pOam, "cpu: OAM writes", 1);
	if (renderer->pendingLines) {
		mPROFILE_ADD(pOamFlush, "cpu: OAM writes forcing flush", 1);
	}
	GBA_VIDEO_TOUCH(renderer);
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	UNUSED(oam);
	softwareRenderer->oamDirty = 1;
	softwareRenderer->spriteLayerDirty = 0xFFFFFFFF;
	memset(softwareRenderer->scanlineDirty, 0xFFFFFFFF, sizeof(softwareRenderer->scanlineDirty));
}

static void GBAVideoSoftwareRendererWritePalette(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value) {
	mPROFILE_ADD(pPal, "cpu: palette writes", 1);
	if (renderer->pendingLines) {
		mPROFILE_ADD(pPalFlush, "cpu: palette writes forcing flush", 1);
	}
	GBA_VIDEO_TOUCH(renderer);
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	color_t color = mColorFrom555(value);
	softwareRenderer->normalPalette[address >> 1] = color;
#if defined(COLOR_16_BIT) && defined(COLOR_5_6_5)
	// The fast path's "waiting to blend" form: green's spare bit set. White
	// would read as its empty pixel, so it loses a bit of blue instead.
	color_t pending = color | 0x0020;
	if (pending == 0xFFFF) {
		pending = 0xFFFE;
	}
	softwareRenderer->pendingPalette[address >> 1] = pending;
#endif
	if (softwareRenderer->blendEffect == BLEND_BRIGHTEN) {
		softwareRenderer->variantPalette[address >> 1] = _brighten(color, softwareRenderer->bldy);
	} else if (softwareRenderer->blendEffect == BLEND_DARKEN) {
		softwareRenderer->variantPalette[address >> 1] = _darken(color, softwareRenderer->bldy);
	}
	int highlightAmount = renderer->highlightAmount >> 4;
	if (highlightAmount) {
		softwareRenderer->highlightPalette[address >> 1] = mColorMix5Bit(0x10 - highlightAmount, softwareRenderer->normalPalette[address >> 1], highlightAmount, renderer->highlightColor);
		softwareRenderer->highlightVariantPalette[address >> 1] = mColorMix5Bit(0x10 - highlightAmount, softwareRenderer->variantPalette[address >> 1], highlightAmount, renderer->highlightColor);
	} else {
		softwareRenderer->highlightPalette[address >> 1] = softwareRenderer->normalPalette[address >> 1];
		softwareRenderer->highlightVariantPalette[address >> 1] = softwareRenderer->variantPalette[address >> 1];
	}
	if (renderer->cache) {
		mCacheSetWritePalette(renderer->cache, address >> 1, color);
	}
	memset(softwareRenderer->scanlineDirty, 0xFFFFFFFF, sizeof(softwareRenderer->scanlineDirty));
}

static void _breakWindow(struct GBAVideoSoftwareRenderer* softwareRenderer, struct WindowN* win, int y) {
	if (win->v.end >= win->v.start) {
		if (y >= win->v.end + win->offsetY) {
			return;
		}
		if (y < win->v.start + win->offsetY) {
			return;
		}
	} else if (y >= win->v.end + win->offsetY && y < win->v.start + win->offsetY) {
		return;
	}
	if (win->h.end > GBA_VIDEO_HORIZONTAL_PIXELS || win->h.end < win->h.start) {
		struct WindowN splits[2] = { *win, *win };
		splits[0].h.start = 0;
		splits[1].h.end = GBA_VIDEO_HORIZONTAL_PIXELS;
		_breakWindowInner(softwareRenderer, &splits[0]);
		_breakWindowInner(softwareRenderer, &splits[1]);
	} else {
		_breakWindowInner(softwareRenderer, win);
	}
}

static void _breakWindowInner(struct GBAVideoSoftwareRenderer* softwareRenderer, struct WindowN* win) {
	int activeWindow;
	int startX = 0;
	if (win->h.end > 0) {
		for (activeWindow = 0; activeWindow < softwareRenderer->nWindows; ++activeWindow) {
			if (win->h.start < softwareRenderer->windows[activeWindow].endX) {
				// Insert a window before the end of the active window
				struct Window oldWindow = softwareRenderer->windows[activeWindow];
				if (win->h.start > startX) {
					// And after the start of the active window
					int nextWindow = softwareRenderer->nWindows;
					++softwareRenderer->nWindows;
					for (; nextWindow > activeWindow; --nextWindow) {
						softwareRenderer->windows[nextWindow] = softwareRenderer->windows[nextWindow - 1];
					}
					softwareRenderer->windows[activeWindow].endX = win->h.start;
					++activeWindow;
				}
				softwareRenderer->windows[activeWindow].control = win->control;
				softwareRenderer->windows[activeWindow].endX = win->h.end;
				if (win->h.end >= oldWindow.endX) {
					// Trim off extra windows we've overwritten
					for (++activeWindow; softwareRenderer->nWindows > activeWindow + 1 && win->h.end >= softwareRenderer->windows[activeWindow].endX; ++activeWindow) {
						if (VIDEO_CHECKS && activeWindow >= MAX_WINDOW) {
							mLOG(GBA_VIDEO, FATAL, "Out of bounds window write will occur");
							return;
						}
						softwareRenderer->windows[activeWindow] = softwareRenderer->windows[activeWindow + 1];
						--softwareRenderer->nWindows;
					}
				} else {
					++activeWindow;
					int nextWindow = softwareRenderer->nWindows;
					++softwareRenderer->nWindows;
					for (; nextWindow > activeWindow; --nextWindow) {
						softwareRenderer->windows[nextWindow] = softwareRenderer->windows[nextWindow - 1];
					}
					softwareRenderer->windows[activeWindow] = oldWindow;
				}
				break;
			}
			startX = softwareRenderer->windows[activeWindow].endX;
		}
	}
#ifdef DEBUG
	if (softwareRenderer->nWindows > MAX_WINDOW) {
		mLOG(GBA_VIDEO, FATAL, "Out of bounds window write occurred!");
	}
#endif
}

// Brings the line's copy of the registers up to date. Returns whether it
// had to change.
static inline bool _updateIoCache(uint16_t* cache, const uint16_t* io) {
	typedef uint32_t __attribute__((may_alias)) Word;
	Word* out = (Word*) cache;
	const Word* in = (const Word*) io;
	uint32_t changed = 0;
	size_t i;
	for (i = 0; i < (REG_SOUND1CNT_LO >> 1) / 2; i += 4) {
		uint32_t a = in[i];
		uint32_t b = in[i + 1];
		uint32_t c = in[i + 2];
		uint32_t d = in[i + 3];
		changed |= (a ^ out[i]) | (b ^ out[i + 1]) | (c ^ out[i + 2]) | (d ^ out[i + 3]);
		out[i] = a;
		out[i + 1] = b;
		out[i + 2] = c;
		out[i + 3] = d;
	}
	return changed;
}

static ATTRIBUTE_NOINLINE void _drawScanlineSlow(struct GBAVideoSoftwareRenderer* softwareRenderer, int y, int spriteLayers);
static ATTRIBUTE_NOINLINE void _copyOut(struct GBAVideoSoftwareRenderer* softwareRenderer, color_t* row);

ATTRIBUTE_HOT_GROUP_BIG(3) static void _drawScanlineNow(struct GBAVideoRenderer* renderer, int y) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;

	if (y == GBA_VIDEO_VERTICAL_PIXELS - 1) {
		softwareRenderer->nextY = 0;
	} else {
		softwareRenderer->nextY = y + 1;
	}

	bool hooked = GBAVideoSoftwareLineHook &&
	              (!GBAVideoSoftwareLineHookTarget || GBAVideoSoftwareLineHookTarget == softwareRenderer);
	if (hooked) {
		GBAVideoSoftwareLineHook(softwareRenderer, y);
		goto advance;
	}

	mPROFILE_START(profileSetup, "line setup");
	bool dirty = softwareRenderer->scanlineDirty[y >> 5] & (1U << (y & 0x1F));
	if (_updateIoCache(softwareRenderer->cache[y].io, softwareRenderer->nextIo)) {
		dirty = true;
	}

	if (GBARegisterDISPCNTGetMode(softwareRenderer->dispcnt) != 0) {
		if (softwareRenderer->cache[y].scale[0][0] != softwareRenderer->bg[2].sx ||
		    softwareRenderer->cache[y].scale[0][1] != softwareRenderer->bg[2].sy ||
		    softwareRenderer->cache[y].scale[1][0] != softwareRenderer->bg[3].sx ||
		    softwareRenderer->cache[y].scale[1][1] != softwareRenderer->bg[3].sy) {
			dirty = true;
		}
	}
	softwareRenderer->cache[y].scale[0][0] = softwareRenderer->bg[2].sx;
	softwareRenderer->cache[y].scale[0][1] = softwareRenderer->bg[2].sy;
	softwareRenderer->cache[y].scale[1][0] = softwareRenderer->bg[3].sx;
	softwareRenderer->cache[y].scale[1][1] = softwareRenderer->bg[3].sy;
	mPROFILE_STOP(profileSetup);

	if (!dirty) {
		mPROFILE_START(profileClean, "clean line (skipped)");
		mPROFILE_STOP(profileClean);
		if (GBARegisterDISPCNTGetMode(softwareRenderer->dispcnt) != 0) {
			if (softwareRenderer->bg[2].enabled == ENABLED_MAX) {
				softwareRenderer->bg[2].sx += softwareRenderer->bg[2].dmx;
				softwareRenderer->bg[2].sy += softwareRenderer->bg[2].dmy;
			}
			if (softwareRenderer->bg[3].enabled == ENABLED_MAX) {
				softwareRenderer->bg[3].sx += softwareRenderer->bg[3].dmx;
				softwareRenderer->bg[3].sy += softwareRenderer->bg[3].dmy;
			}
		}
		return;
	}

	CLEAN_SCANLINE(softwareRenderer, y);

	color_t* row = &softwareRenderer->outputBuffer[softwareRenderer->outputBufferStride * y];
	if (GBARegisterDISPCNTIsForcedBlank(softwareRenderer->dispcnt)) {
		int x;
		for (x = 0; x < GBA_VIDEO_HORIZONTAL_PIXELS; ++x) {
			row[x] = M_COLOR_WHITE;
		}
		return;
	}

	bool fastEligible = GBAVideoSoftwareRendererFastEligible(softwareRenderer);
	mPROFILE_START(profilePre, "preprocess");
	GBAVideoSoftwareRendererPreprocessBuffer(softwareRenderer, y, !fastEligible);
	mPROFILE_STOP(profilePre);
	softwareRenderer->spriteCyclesRemaining = GBARegisterDISPCNTIsHblankIntervalFree(softwareRenderer->dispcnt) ? OBJ_HBLANK_FREE_LENGTH : OBJ_LENGTH;
	mPROFILE_START(profileSprites, "sprites draw");
	int spriteLayers = GBAVideoSoftwareRendererPreprocessSpriteLayer(softwareRenderer, y, fastEligible);
	mPROFILE_STOP(profileSprites);

	bool fast = fastEligible && softwareRenderer->fastSpritesDirect;
	if (fast) {
		mPROFILE_START(profileFast, "fast composite");
		GBAVideoSoftwareRendererDrawFast(softwareRenderer, y, spriteLayers, row);
		mPROFILE_STOP(profileFast);
	} else if (fastEligible) {
		_fillBackdrop(softwareRenderer);
	}
	if (fast) {
		++softwareRenderer->fastLines;
#ifdef M_PROFILE
		++GBAVideoLineCount[0];
#endif
	} else {
		++softwareRenderer->slowLines;
#ifdef M_PROFILE
		++GBAVideoLineCount[1];
#endif
		_drawScanlineSlow(softwareRenderer, y, spriteLayers);
	}

advance:
	if (GBARegisterDISPCNTGetMode(softwareRenderer->dispcnt) != 0) {
		if (softwareRenderer->bg[2].enabled == ENABLED_MAX) {
			softwareRenderer->bg[2].sx += softwareRenderer->bg[2].dmx;
			softwareRenderer->bg[2].sy += softwareRenderer->bg[2].dmy;
		}
		if (softwareRenderer->bg[3].enabled == ENABLED_MAX) {
			softwareRenderer->bg[3].sx += softwareRenderer->bg[3].dmx;
			softwareRenderer->bg[3].sy += softwareRenderer->bg[3].dmy;
		}
	}

	if (softwareRenderer->bg[0].enabled != 0 && softwareRenderer->bg[0].enabled < ENABLED_MAX) {
		++softwareRenderer->bg[0].enabled;
		DIRTY_SCANLINE(softwareRenderer, y);
	}
	if (softwareRenderer->bg[1].enabled != 0 && softwareRenderer->bg[1].enabled < ENABLED_MAX) {
		++softwareRenderer->bg[1].enabled;
		DIRTY_SCANLINE(softwareRenderer, y);
	}
	if (softwareRenderer->bg[2].enabled != 0 && softwareRenderer->bg[2].enabled < ENABLED_MAX) {
		++softwareRenderer->bg[2].enabled;
		DIRTY_SCANLINE(softwareRenderer, y);
	}
	if (softwareRenderer->bg[3].enabled != 0 && softwareRenderer->bg[3].enabled < ENABLED_MAX) {
		++softwareRenderer->bg[3].enabled;
		DIRTY_SCANLINE(softwareRenderer, y);
	}

	if (!hooked && !fast) {
		_copyOut(softwareRenderer, row);
	}
}

// The line as the regular renderer draws it, into softwareRenderer->row
static void _drawScanlineSlow(struct GBAVideoSoftwareRenderer* softwareRenderer, int y, int spriteLayers) {
	int w;
	unsigned priority;
	softwareRenderer->end = 0;
	for (w = 0; w < softwareRenderer->nWindows; ++w) {
		softwareRenderer->start = softwareRenderer->end;
		softwareRenderer->end = softwareRenderer->windows[w].endX;
		softwareRenderer->currentWindow = softwareRenderer->windows[w].control;
		switch (GBARegisterDISPCNTGetMode(softwareRenderer->dispcnt)) {
		case 0:
			if (softwareRenderer->bg[0].enabled == ENABLED_MAX) {
				_updateFlags(softwareRenderer, &softwareRenderer->bg[0]);
			}
			if (softwareRenderer->bg[1].enabled == ENABLED_MAX) {
				_updateFlags(softwareRenderer, &softwareRenderer->bg[1]);
			}
			// Fall through
		case 2:
			if (softwareRenderer->bg[3].enabled == ENABLED_MAX) {
				_updateFlags(softwareRenderer, &softwareRenderer->bg[3]);
			}
			// Fall through
		case 3:
		case 4:
		case 5:
			if (softwareRenderer->bg[2].enabled == ENABLED_MAX) {
				_updateFlags(softwareRenderer, &softwareRenderer->bg[2]);
			}
			break;
		case 1:
			if (softwareRenderer->bg[0].enabled == ENABLED_MAX) {
				_updateFlags(softwareRenderer, &softwareRenderer->bg[0]);
			}
			if (softwareRenderer->bg[1].enabled == ENABLED_MAX) {
				_updateFlags(softwareRenderer, &softwareRenderer->bg[1]);
			}
			if (softwareRenderer->bg[2].enabled == ENABLED_MAX) {
				_updateFlags(softwareRenderer, &softwareRenderer->bg[2]);
			}
			break;
		}

		for (priority = 0; priority < 4; ++priority) {
			if (spriteLayers & (1 << priority)) {
				mPROFILE_START(profileComposite, "sprites composite");
				GBAVideoSoftwareRendererPostprocessSprite(softwareRenderer, priority);
				mPROFILE_STOP(profileComposite);
			}
			mPROFILE_START(profileBackgrounds, "backgrounds");
			if (TEST_LAYER_ENABLED(0) && GBARegisterDISPCNTGetMode(softwareRenderer->dispcnt) < 2) {
				GBAVideoSoftwareRendererDrawBackgroundMode0(softwareRenderer, &softwareRenderer->bg[0], y);
			}
			if (TEST_LAYER_ENABLED(1) && GBARegisterDISPCNTGetMode(softwareRenderer->dispcnt) < 2) {
				GBAVideoSoftwareRendererDrawBackgroundMode0(softwareRenderer, &softwareRenderer->bg[1], y);
			}
			if (TEST_LAYER_ENABLED(2)) {
				switch (GBARegisterDISPCNTGetMode(softwareRenderer->dispcnt)) {
				case 0:
					GBAVideoSoftwareRendererDrawBackgroundMode0(softwareRenderer, &softwareRenderer->bg[2], y);
					break;
				case 1:
				case 2:
				{
					mPROFILE_START(profileMode2, "bg mode2 affine");
					GBAVideoSoftwareRendererDrawBackgroundMode2(softwareRenderer, &softwareRenderer->bg[2], y);
					mPROFILE_STOP(profileMode2);
					break;
				}
				case 3:
				{
					mPROFILE_START(profileMode3, "bg mode3 bitmap");
					GBAVideoSoftwareRendererDrawBackgroundMode3(softwareRenderer, &softwareRenderer->bg[2], y);
					mPROFILE_STOP(profileMode3);
					break;
				}
				case 4:
				{
					mPROFILE_START(profileMode4, "bg mode4 bitmap");
					GBAVideoSoftwareRendererDrawBackgroundMode4(softwareRenderer, &softwareRenderer->bg[2], y);
					mPROFILE_STOP(profileMode4);
					break;
				}
				case 5:
				{
					mPROFILE_START(profileMode5, "bg mode5 bitmap");
					GBAVideoSoftwareRendererDrawBackgroundMode5(softwareRenderer, &softwareRenderer->bg[2], y);
					mPROFILE_STOP(profileMode5);
					break;
				}
				}
			}
			if (TEST_LAYER_ENABLED(3)) {
				switch (GBARegisterDISPCNTGetMode(softwareRenderer->dispcnt)) {
				case 0:
					GBAVideoSoftwareRendererDrawBackgroundMode0(softwareRenderer, &softwareRenderer->bg[3], y);
					break;
				case 2:
				{
					mPROFILE_START(profileMode2, "bg mode2 affine");
					GBAVideoSoftwareRendererDrawBackgroundMode2(softwareRenderer, &softwareRenderer->bg[3], y);
					mPROFILE_STOP(profileMode2);
					break;
				}
				}
			}
			mPROFILE_STOP(profileBackgrounds);
		}
	}

	mPROFILE_START(profilePost, "postprocess");
	GBAVideoSoftwareRendererPostprocessBuffer(softwareRenderer);
	mPROFILE_STOP(profilePost);
}

static void _copyOut(struct GBAVideoSoftwareRenderer* softwareRenderer, color_t* row) {
	mPROFILE_START(profileCopy, "copy out");
	int x;
	if (softwareRenderer->greenswap) {
		for (x = 0; x < GBA_VIDEO_HORIZONTAL_PIXELS; x += 4) {
			row[x] = softwareRenderer->row[x] & (M_COLOR_RED | M_COLOR_BLUE);
			row[x] |= softwareRenderer->row[x + 1] & M_COLOR_GREEN;
			row[x + 1] = softwareRenderer->row[x + 1] & (M_COLOR_RED | M_COLOR_BLUE);
			row[x + 1] |= softwareRenderer->row[x] & M_COLOR_GREEN;
			row[x + 2] = softwareRenderer->row[x + 2] & (M_COLOR_RED | M_COLOR_BLUE);
			row[x + 2] |= softwareRenderer->row[x + 3] & M_COLOR_GREEN;
			row[x + 3] = softwareRenderer->row[x + 3] & (M_COLOR_RED | M_COLOR_BLUE);
			row[x + 3] |= softwareRenderer->row[x + 2] & M_COLOR_GREEN;

		}
	} else {
#ifdef COLOR_16_BIT
		for (x = 0; x < GBA_VIDEO_HORIZONTAL_PIXELS; x += 4) {
			row[x] = softwareRenderer->row[x];
			row[x + 1] = softwareRenderer->row[x + 1];
			row[x + 2] = softwareRenderer->row[x + 2];
			row[x + 3] = softwareRenderer->row[x + 3];
		}
#else
		memcpy(row, softwareRenderer->row, GBA_VIDEO_HORIZONTAL_PIXELS * sizeof(*row));
#endif
	}
	mPROFILE_STOP(profileCopy);
}

static void GBAVideoSoftwareRendererFinishFrame(struct GBAVideoRenderer* renderer) {
	GBA_VIDEO_TOUCH(renderer);
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;

	softwareRenderer->nextY = 0;
	if (softwareRenderer->temporaryBuffer) {
		mappedMemoryFree(softwareRenderer->temporaryBuffer, GBA_VIDEO_HORIZONTAL_PIXELS * GBA_VIDEO_VERTICAL_PIXELS * 4);
		softwareRenderer->temporaryBuffer = 0;
	}
	softwareRenderer->bg[2].sx = softwareRenderer->bg[2].refx;
	softwareRenderer->bg[2].sy = softwareRenderer->bg[2].refy;
	softwareRenderer->bg[3].sx = softwareRenderer->bg[3].refx;
	softwareRenderer->bg[3].sy = softwareRenderer->bg[3].refy;

	if (softwareRenderer->bg[0].enabled > 0) {
		softwareRenderer->bg[0].enabled = ENABLED_MAX;
	}
	if (softwareRenderer->bg[1].enabled > 0) {
		softwareRenderer->bg[1].enabled = ENABLED_MAX;
	}
	if (softwareRenderer->bg[2].enabled > 0) {
		softwareRenderer->bg[2].enabled = ENABLED_MAX;
	}
	if (softwareRenderer->bg[3].enabled > 0) {
		softwareRenderer->bg[3].enabled = ENABLED_MAX;
	}
}

static void GBAVideoSoftwareRendererGetPixels(struct GBAVideoRenderer* renderer, size_t* stride, const void** pixels) {
	GBA_VIDEO_TOUCH(renderer);
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	*stride = softwareRenderer->outputBufferStride;
	*pixels = softwareRenderer->outputBuffer;
}

static void GBAVideoSoftwareRendererPutPixels(struct GBAVideoRenderer* renderer, size_t stride, const void* pixels) {
	GBA_VIDEO_TOUCH(renderer);
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;

	const color_t* colorPixels = pixels;
	unsigned i;
	for (i = 0; i < GBA_VIDEO_VERTICAL_PIXELS; ++i) {
		memmove(&softwareRenderer->outputBuffer[softwareRenderer->outputBufferStride * i], &colorPixels[stride * i], GBA_VIDEO_HORIZONTAL_PIXELS * BYTES_PER_PIXEL);
	}
}

static void _enableBg(struct GBAVideoSoftwareRenderer* renderer, int bg, bool active) {
	int wasActive = renderer->bg[bg].enabled;
	if (!active) {
		if (renderer->nextY == 0 || (wasActive > 0 && wasActive < ENABLED_MAX)) {
			renderer->bg[bg].enabled = 0;
		} else if (wasActive == ENABLED_MAX) {
			renderer->bg[bg].enabled = -2;
		}
	} else if (!wasActive && active) {
		if (renderer->nextY == 0) {
			// TODO: Investigate in more depth how switching background works in different modes
			renderer->bg[bg].enabled = ENABLED_MAX;
		} else if (GBARegisterDISPCNTGetMode(renderer->dispcnt) > 2) {
			renderer->bg[bg].enabled = 2;
		} else {
			renderer->bg[bg].enabled = 1;
		}
	} else if (wasActive < 0 && active) {
		renderer->bg[bg].enabled = ENABLED_MAX;
	}
}

static void GBAVideoSoftwareRendererUpdateDISPCNT(struct GBAVideoSoftwareRenderer* renderer) {
	_enableBg(renderer, 0, GBARegisterDISPCNTGetBg0Enable(renderer->dispcnt));
	_enableBg(renderer, 1, GBARegisterDISPCNTGetBg1Enable(renderer->dispcnt));
	_enableBg(renderer, 2, GBARegisterDISPCNTGetBg2Enable(renderer->dispcnt));
	_enableBg(renderer, 3, GBARegisterDISPCNTGetBg3Enable(renderer->dispcnt));
}

static void GBAVideoSoftwareRendererWriteBGCNT(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	UNUSED(renderer);
	bg->priority = GBARegisterBGCNTGetPriority(value);
	bg->charBase = GBARegisterBGCNTGetCharBase(value) << 14;
	bg->mosaic = GBARegisterBGCNTGetMosaic(value);
	bg->multipalette = GBARegisterBGCNTGet256Color(value);
	bg->screenBase = GBARegisterBGCNTGetScreenBase(value) << 11;
	bg->overflow = GBARegisterBGCNTGetOverflow(value);
	bg->size = GBARegisterBGCNTGetSize(value);
	bg->yCache = -1;

	_updateFlags(renderer, bg);
}

static void GBAVideoSoftwareRendererWriteBGX_LO(struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	bg->refx = (bg->refx & 0xFFFF0000) | value;
	bg->sx = bg->refx;
}

static void GBAVideoSoftwareRendererWriteBGX_HI(struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	bg->refx = (bg->refx & 0x0000FFFF) | (value << 16);
	bg->refx <<= 4;
	bg->refx >>= 4;
	bg->sx = bg->refx;
}

static void GBAVideoSoftwareRendererWriteBGY_LO(struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	bg->refy = (bg->refy & 0xFFFF0000) | value;
	bg->sy = bg->refy;
}

static void GBAVideoSoftwareRendererWriteBGY_HI(struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	bg->refy = (bg->refy & 0x0000FFFF) | (value << 16);
	bg->refy <<= 4;
	bg->refy >>= 4;
	bg->sy = bg->refy;
}

static void GBAVideoSoftwareRendererWriteBLDCNT(struct GBAVideoSoftwareRenderer* renderer, uint16_t value) {
	enum GBAVideoBlendEffect oldEffect = renderer->blendEffect;

	renderer->bg[0].target1 = GBARegisterBLDCNTGetTarget1Bg0(value);
	renderer->bg[1].target1 = GBARegisterBLDCNTGetTarget1Bg1(value);
	renderer->bg[2].target1 = GBARegisterBLDCNTGetTarget1Bg2(value);
	renderer->bg[3].target1 = GBARegisterBLDCNTGetTarget1Bg3(value);
	renderer->bg[0].target2 = GBARegisterBLDCNTGetTarget2Bg0(value);
	renderer->bg[1].target2 = GBARegisterBLDCNTGetTarget2Bg1(value);
	renderer->bg[2].target2 = GBARegisterBLDCNTGetTarget2Bg2(value);
	renderer->bg[3].target2 = GBARegisterBLDCNTGetTarget2Bg3(value);

	renderer->blendEffect = GBARegisterBLDCNTGetEffect(value);
	renderer->target1Obj = GBARegisterBLDCNTGetTarget1Obj(value);
	renderer->target1Bd = GBARegisterBLDCNTGetTarget1Bd(value);
	renderer->target2Obj = GBARegisterBLDCNTGetTarget2Obj(value);
	renderer->target2Bd = GBARegisterBLDCNTGetTarget2Bd(value);

	if (oldEffect != renderer->blendEffect) {
		renderer->blendDirty = true;
	}
}

void GBAVideoSoftwareRendererPreprocessBuffer(struct GBAVideoSoftwareRenderer* softwareRenderer, int y, bool fillRow) {
	int x;
	uint32_t dirty = softwareRenderer->spriteLayerDirty;
	// Anything but the fast path's sprites may write all over it
	softwareRenderer->spriteLayerDirty = 0xFFFFFFFF;
	if (dirty == 0xFFFFFFFF) {
		for (x = 0; x < GBA_VIDEO_HORIZONTAL_PIXELS; x += 4) {
			softwareRenderer->spriteLayer[x] = FLAG_UNWRITTEN;
			softwareRenderer->spriteLayer[x + 1] = FLAG_UNWRITTEN;
			softwareRenderer->spriteLayer[x + 2] = FLAG_UNWRITTEN;
			softwareRenderer->spriteLayer[x + 3] = FLAG_UNWRITTEN;
		}
	} else {
		uint32_t* layer = softwareRenderer->spriteLayer;
		for (; dirty; dirty >>= 1, layer += 8) {
			if (!(dirty & 1)) {
				continue;
			}
			layer[0] = FLAG_UNWRITTEN;
			layer[1] = FLAG_UNWRITTEN;
			layer[2] = FLAG_UNWRITTEN;
			layer[3] = FLAG_UNWRITTEN;
			layer[4] = FLAG_UNWRITTEN;
			layer[5] = FLAG_UNWRITTEN;
			layer[6] = FLAG_UNWRITTEN;
			layer[7] = FLAG_UNWRITTEN;
		}
	}

	softwareRenderer->windows[0].endX = GBA_VIDEO_HORIZONTAL_PIXELS;
	softwareRenderer->nWindows = 1;
	if (GBARegisterDISPCNTIsWin0Enable(softwareRenderer->dispcnt) || GBARegisterDISPCNTIsWin1Enable(softwareRenderer->dispcnt) || GBARegisterDISPCNTIsObjwinEnable(softwareRenderer->dispcnt)) {
		softwareRenderer->windows[0].control = softwareRenderer->winout;
		if (GBARegisterDISPCNTIsWin1Enable(softwareRenderer->dispcnt) && !softwareRenderer->d.disableWIN[1]) {
			_breakWindow(softwareRenderer, &softwareRenderer->winN[1], y);
		}
		if (GBARegisterDISPCNTIsWin0Enable(softwareRenderer->dispcnt) && !softwareRenderer->d.disableWIN[0]) {
			_breakWindow(softwareRenderer, &softwareRenderer->winN[0], y);
		}
	} else {
		softwareRenderer->windows[0].control.packed = 0xFF;
	}

	GBAVideoSoftwareRendererUpdateDISPCNT(softwareRenderer);

	if (softwareRenderer->lastHighlightAmount != softwareRenderer->d.highlightAmount) {
		softwareRenderer->lastHighlightAmount = softwareRenderer->d.highlightAmount;
		if (softwareRenderer->lastHighlightAmount) {
			softwareRenderer->blendDirty = true;
		}
	}

	if (softwareRenderer->blendDirty) {
		mPROFILE_START(profilePalettes, "palette recompute");
		_updatePalettes(softwareRenderer);
		mPROFILE_STOP(profilePalettes);
		softwareRenderer->blendDirty = false;
	}
	softwareRenderer->forceTarget1 = false;
	softwareRenderer->fastSpritesDirect = false;

	if (fillRow) {
		_fillBackdrop(softwareRenderer);
	}

	softwareRenderer->bg[0].highlight = softwareRenderer->d.highlightBG[0];
	softwareRenderer->bg[1].highlight = softwareRenderer->d.highlightBG[1];
	softwareRenderer->bg[2].highlight = softwareRenderer->d.highlightBG[2];
	softwareRenderer->bg[3].highlight = softwareRenderer->d.highlightBG[3];
}

static void _fillBackdrop(struct GBAVideoSoftwareRenderer* softwareRenderer) {
	int w;
	int x = 0;
	for (w = 0; w < softwareRenderer->nWindows; ++w) {
		// TOOD: handle objwin on backdrop
		uint32_t backdrop = FLAG_UNWRITTEN | FLAG_PRIORITY | FLAG_IS_BACKGROUND;
		if (!softwareRenderer->target1Bd || softwareRenderer->blendEffect == BLEND_NONE || softwareRenderer->blendEffect == BLEND_ALPHA || !GBAWindowControlIsBlendEnable(softwareRenderer->windows[w].control.packed)) {
			backdrop |= softwareRenderer->normalPalette[0];
		} else {
			backdrop |= softwareRenderer->variantPalette[0];
		}
		int end = softwareRenderer->windows[w].endX;
		for (; x & 3; ++x) {
			softwareRenderer->row[x] = backdrop;
		}
		for (; x < end - 3; x += 4) {
			softwareRenderer->row[x] = backdrop;
			softwareRenderer->row[x + 1] = backdrop;
			softwareRenderer->row[x + 2] = backdrop;
			softwareRenderer->row[x + 3] = backdrop;
		}
		for (; x < end; ++x) {
			softwareRenderer->row[x] = backdrop;
		}
	}
}

void GBAVideoSoftwareRendererPostprocessBuffer(struct GBAVideoSoftwareRenderer* softwareRenderer) {
	int x, w;
	if ((softwareRenderer->forceTarget1 || softwareRenderer->bg[0].target1 || softwareRenderer->bg[1].target1 || softwareRenderer->bg[2].target1 || softwareRenderer->bg[3].target1) && softwareRenderer->target2Bd) {
		x = 0;
		for (w = 0; w < softwareRenderer->nWindows; ++w) {
			uint32_t backdrop = 0;
			if (!softwareRenderer->target1Bd || softwareRenderer->blendEffect == BLEND_NONE || softwareRenderer->blendEffect == BLEND_ALPHA || !GBAWindowControlIsBlendEnable(softwareRenderer->windows[w].control.packed)) {
				backdrop |= softwareRenderer->normalPalette[0];
			} else {
				backdrop |= softwareRenderer->variantPalette[0];
			}
			int end = softwareRenderer->windows[w].endX;
			for (; x < end; ++x) {
				uint32_t color = softwareRenderer->row[x];
				if (color & FLAG_TARGET_1) {
					softwareRenderer->row[x] = mColorMix5Bit(softwareRenderer->bldb, backdrop, softwareRenderer->blda, color);
				}
			}
		}
	}
	if (softwareRenderer->forceTarget1 && (softwareRenderer->blendEffect == BLEND_DARKEN || softwareRenderer->blendEffect == BLEND_BRIGHTEN)) {
		x = 0;
		for (w = 0; w < softwareRenderer->nWindows; ++w) {
			int end = softwareRenderer->windows[w].endX;
			uint32_t mask = FLAG_REBLEND | FLAG_IS_BACKGROUND;
			uint32_t match = FLAG_REBLEND;
			bool objBlend = GBAWindowControlIsBlendEnable(softwareRenderer->objwin.packed);
			bool winBlend = GBAWindowControlIsBlendEnable(softwareRenderer->windows[w].control.packed);
			if (GBARegisterDISPCNTIsObjwinEnable(softwareRenderer->dispcnt) && objBlend != winBlend) {
				mask |= FLAG_OBJWIN;
				if (objBlend) {
					match |= FLAG_OBJWIN;
				}
			} else if (!winBlend) {
				x = end;
				continue;
			}
			if (softwareRenderer->blendEffect == BLEND_DARKEN) {
				for (; x < end; ++x) {
					uint32_t color = softwareRenderer->row[x];
					if ((color & mask) == match) {
						softwareRenderer->row[x] = _darken(color, softwareRenderer->bldy);
					}
				}
			} else if (softwareRenderer->blendEffect == BLEND_BRIGHTEN) {
				for (; x < end; ++x) {
					uint32_t color = softwareRenderer->row[x];
					if ((color & mask) == match) {
						softwareRenderer->row[x] = _brighten(color, softwareRenderer->bldy);
					}
				}
			}
		}
	}
}

// The regular path's sprite layer is a lot of code: kept out of the line drawer's cache group
ATTRIBUTE_NOINLINE int GBAVideoSoftwareRendererPreprocessSpriteLayer(struct GBAVideoSoftwareRenderer* renderer, int y, bool fast) {
	int w;
	int spriteLayers = 0;
	renderer->fastSpritesDirect = false;
	if (GBARegisterDISPCNTIsObjEnable(renderer->dispcnt) && !renderer->d.disableOBJ) {
		if (renderer->oamDirty) {
			mPROFILE_START(profileOam, "OAM rebuild");
			renderer->oamMax = GBAVideoRendererCleanOAM(renderer->d.oam->obj, renderer->sprites, renderer->objOffsetY);
			GBAVideoSoftwareRendererListSpriteLines(renderer);
			mPROFILE_STOP(profileOam);
			renderer->oamDirty = false;
			renderer->fastSpritesValid = false;
		}
		bool direct = false;
		if (fast) {
			mPROFILE_START(profileKey, "sprite key");
			direct = GBAVideoSoftwareRendererFastSpriteLayer(renderer, y);
			mPROFILE_STOP(profileKey);
		}
		if (direct) {
			// Straight into the row, no sprite layer
			renderer->fastSpritesDirect = true;
			renderer->spriteLayerDirty = 0;
			return renderer->spriteLinePriorities[y];
		}
		int mosaicV = GBAMosaicControlGetObjV(renderer->mosaic) + 1;
		int mosaicY = y - (y % mosaicV);
		int i;
		int last = renderer->spriteLineStart[y + 1];
		for (i = renderer->spriteLineStart[y]; i < last; ++i) {
			struct GBAVideoRendererSprite* sprite = &renderer->sprites[renderer->spriteLines[i]];
			int localY = y;
			renderer->end = 0;
			if (GBAObjAttributesAIsMosaic(sprite->obj.a) && mosaicV > 1) {
				localY = mosaicY;
				if (localY < sprite->y && sprite->y < GBA_VIDEO_VERTICAL_PIXELS) {
					localY = sprite->y;
				}
				if (localY >= (sprite->endY & 0xFF)) {
					localY = sprite->endY - 1;
				}
			}
			for (w = 0; w < renderer->nWindows; ++w) {
				renderer->currentWindow = renderer->windows[w].control;
				renderer->start = renderer->end;
				renderer->end = renderer->windows[w].endX;
				// TODO: partial sprite drawing
				if (!GBAWindowControlIsObjEnable(renderer->currentWindow.packed) && !GBARegisterDISPCNTIsObjwinEnable(renderer->dispcnt)) {
					continue;
				}

#ifdef M_PROFILE
				static const char* const spriteKinds[4] = {
					"sprite 4bpp", "sprite 8bpp", "sprite affine 4bpp", "sprite affine 8bpp"
				};
				const char* spriteKind = spriteKinds[(GBAObjAttributesAIs256Color(sprite->obj.a) ? 1 : 0) |
				                                     (GBAObjAttributesAIsTransformed(sprite->obj.a) ? 2 : 0)];
#endif
				mPROFILE_START(profileSprite, spriteKind);
				int drawn = GBAVideoSoftwareRendererPreprocessSprite(renderer, &sprite->obj, sprite->index, localY);
				mPROFILE_STOP(profileSprite);
				spriteLayers |= drawn << GBAObjAttributesCGetPriority(sprite->obj.c);
			}
			renderer->spriteCyclesRemaining -= sprite->cycles;
			if (renderer->spriteCyclesRemaining <= 0) {
				break;
			}
		}
	} else {
		renderer->fastSpritesDirect = fast;
		renderer->spriteLayerDirty = 0;
	}
	return spriteLayers;
}

static void _updatePalettes(struct GBAVideoSoftwareRenderer* renderer) {
	int i;
	if (renderer->blendEffect == BLEND_BRIGHTEN) {
		for (i = 0; i < 512; ++i) {
			renderer->variantPalette[i] = _brighten(renderer->normalPalette[i], renderer->bldy);
		}
	} else if (renderer->blendEffect == BLEND_DARKEN) {
		for (i = 0; i < 512; ++i) {
			renderer->variantPalette[i] = _darken(renderer->normalPalette[i], renderer->bldy);
		}
	} else {
		for (i = 0; i < 512; ++i) {
			renderer->variantPalette[i] = renderer->normalPalette[i];
		}
	}
	unsigned highlightAmount = renderer->d.highlightAmount >> 4;

	if (highlightAmount) {
		for (i = 0; i < 512; ++i) {
			renderer->highlightPalette[i] = mColorMix5Bit(0x10 - highlightAmount, renderer->normalPalette[i], highlightAmount, renderer->d.highlightColor);
			renderer->highlightVariantPalette[i] = mColorMix5Bit(0x10 - highlightAmount, renderer->variantPalette[i], highlightAmount, renderer->d.highlightColor);
		}
	}
}

void _updateFlags(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background) {
	uint32_t flags = (background->priority << OFFSET_PRIORITY) | (background->index << OFFSET_INDEX) | FLAG_IS_BACKGROUND;
	if (background->target2) {
		flags |= FLAG_TARGET_2;
	}
	uint32_t objwinFlags = flags;
	if (renderer->blendEffect == BLEND_ALPHA) {
		if (renderer->blda == 0x10 && renderer->bldb == 0) {
			flags &= ~FLAG_TARGET_2;
			objwinFlags &= ~FLAG_TARGET_2;
		} else if (background->target1) {
			if (GBAWindowControlIsBlendEnable(renderer->currentWindow.packed)) {
				flags |= FLAG_TARGET_1;
			}
			if (GBAWindowControlIsBlendEnable(renderer->objwin.packed)) {
				objwinFlags |= FLAG_TARGET_1;
			}
		}
	}
	background->flags = flags;
	background->objwinFlags = objwinFlags;
	background->variant = background->target1 && GBAWindowControlIsBlendEnable(renderer->currentWindow.packed) && (renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN);
}
