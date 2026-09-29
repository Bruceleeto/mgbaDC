/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/dma.h>

#include <mgba/internal/arm/macros.h>

#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>

mLOG_DEFINE_CATEGORY(GBA_DMA, "GBA DMA", "gba.dma");

static void _dmaEvent(struct mTiming* timing, void* context, uint32_t cyclesLate);

static void GBADMAService(struct GBA* gba, int number, struct GBADMA* info);

static const int DMA_OFFSET[] = { 1, -1, 0, 1 };

void GBADMAInit(struct GBA* gba) {
	gba->memory.dmaEvent.name = "GBA DMA";
	gba->memory.dmaEvent.callback = _dmaEvent;
	gba->memory.dmaEvent.context = gba;
	gba->memory.dmaEvent.priority = 0x40;
}

void GBADMAReset(struct GBA* gba) {
	memset(gba->memory.dma, 0, sizeof(gba->memory.dma));
	int i;
	for (i = 0; i < 4; ++i) {
		gba->memory.dma[i].count = 0x4000;
	}
	gba->memory.dma[3].count = 0x10000;
	gba->memory.activeDMA = -1;
}
static bool _isValidDMASAD(int dma, uint32_t address) {
	if (dma == 0 && address >= BASE_CART0 && address < BASE_CART_SRAM) {
		return false;
	}
	return address >= BASE_WORKING_RAM;
}

static bool _isValidDMADAD(int dma, uint32_t address) {
	return dma == 3 || address < BASE_CART0;
}

uint32_t GBADMAWriteSAD(struct GBA* gba, int dma, uint32_t address) {
	struct GBAMemory* memory = &gba->memory;
	if (_isValidDMASAD(dma, address)) {
		memory->dma[dma].source = address & 0x0FFFFFFE;
	} else {
		mLOG(GBA_DMA, GAME_ERROR, "Invalid DMA source address: 0x%08" PRIX32, address);
		memory->dma[dma].source = 0;
	}
	return memory->dma[dma].source;
}

uint32_t GBADMAWriteDAD(struct GBA* gba, int dma, uint32_t address) {
	struct GBAMemory* memory = &gba->memory;
	address &= 0x0FFFFFFE;
	if (_isValidDMADAD(dma, address)) {
		memory->dma[dma].dest = address;
	} else {
		mLOG(GBA_DMA, GAME_ERROR, "Invalid DMA destination address: 0x%08" PRIX32, address);
	}
	return memory->dma[dma].dest;
}

void GBADMAWriteCNT_LO(struct GBA* gba, int dma, uint16_t count) {
	struct GBAMemory* memory = &gba->memory;
	memory->dma[dma].count = count ? count : (dma == 3 ? 0x10000 : 0x4000);
}

uint16_t GBADMAWriteCNT_HI(struct GBA* gba, int dma, uint16_t control) {
	struct GBAMemory* memory = &gba->memory;
	struct GBADMA* currentDma = &memory->dma[dma];
	int wasEnabled = GBADMARegisterIsEnable(currentDma->reg);
	if (dma < 3) {
		control &= 0xF7E0;
	} else {
		control &= 0xFFE0;
	}
	currentDma->reg = control;

	if (GBADMARegisterIsDRQ(currentDma->reg)) {
		mLOG(GBA_DMA, STUB, "DRQ not implemented");
	}

	if (!wasEnabled && GBADMARegisterIsEnable(currentDma->reg)) {
		currentDma->nextSource = currentDma->source;
		currentDma->nextDest = currentDma->dest;

		uint32_t width = 2 << GBADMARegisterGetWidth(currentDma->reg);
		if (currentDma->nextSource & (width - 1)) {
			mLOG(GBA_DMA, GAME_ERROR, "Misaligned DMA source address: 0x%08" PRIX32, currentDma->nextSource);
		}
		if (currentDma->nextDest & (width - 1)) {
			mLOG(GBA_DMA, GAME_ERROR, "Misaligned DMA destination address: 0x%08" PRIX32, currentDma->nextDest);
		}
		mLOG(GBA_DMA, INFO, "Starting DMA %i 0x%08" PRIX32 " -> 0x%08" PRIX32 " (%04X:%04X)", dma,
		     currentDma->nextSource, currentDma->nextDest,
		     currentDma->reg, (unsigned) (currentDma->count & 0xFFFF));

		currentDma->nextSource &= -width;
		currentDma->nextDest &= -width;

		GBADMASchedule(gba, dma, currentDma);
	}
	// If the DMA has already occurred, this value might have changed since the function started
	return currentDma->reg;
};

void GBADMASchedule(struct GBA* gba, int number, struct GBADMA* info) {
	switch (GBADMARegisterGetTiming(info->reg)) {
	case GBA_DMA_TIMING_NOW:
		info->when = mTimingCurrentTime(&gba->timing) + 3; // DMAs take 3 cycles to start
		info->nextCount = info->count;
		break;
	case GBA_DMA_TIMING_HBLANK:
	case GBA_DMA_TIMING_VBLANK:
		// Handled implicitly
		return;
	case GBA_DMA_TIMING_CUSTOM:
		switch (number) {
		case 0:
			mLOG(GBA_DMA, WARN, "Discarding invalid DMA0 scheduling");
			return;
		case 1:
		case 2:
			GBAAudioScheduleFifoDma(&gba->audio, number, info);
			break;
		case 3:
			// Handled implicitly
			break;
		}
	}
	GBADMAUpdate(gba);
}

void GBADMARunHblank(struct GBA* gba, int32_t cycles) {
	struct GBAMemory* memory = &gba->memory;
	struct GBADMA* dma;
	bool found = false;
	int i;
	for (i = 0; i < 4; ++i) {
		dma = &memory->dma[i];
		if (GBADMARegisterIsEnable(dma->reg) && GBADMARegisterGetTiming(dma->reg) == GBA_DMA_TIMING_HBLANK && !dma->nextCount) {
			dma->when = mTimingCurrentTime(&gba->timing) + 3 + cycles;
			dma->nextCount = dma->count;
			found = true;
		}
	}
	if (found) {
		GBADMAUpdate(gba);
	}
}

void GBADMARunVblank(struct GBA* gba, int32_t cycles) {
	struct GBAMemory* memory = &gba->memory;
	struct GBADMA* dma;
	bool found = false;
	int i;
	for (i = 0; i < 4; ++i) {
		dma = &memory->dma[i];
		if (GBADMARegisterIsEnable(dma->reg) && GBADMARegisterGetTiming(dma->reg) == GBA_DMA_TIMING_VBLANK && !dma->nextCount) {
			dma->when = mTimingCurrentTime(&gba->timing) + 3 + cycles;
			dma->nextCount = dma->count;
			found = true;
		}
	}
	if (found) {
		GBADMAUpdate(gba);
	}
}

void GBADMARunDisplayStart(struct GBA* gba, int32_t cycles) {
	struct GBAMemory* memory = &gba->memory;
	struct GBADMA* dma = &memory->dma[3];
	if (GBADMARegisterIsEnable(dma->reg) && GBADMARegisterGetTiming(dma->reg) == GBA_DMA_TIMING_CUSTOM && !dma->nextCount) {
		dma->when = mTimingCurrentTime(&gba->timing) + 3 + cycles;
		dma->nextCount = dma->count;
		GBADMAUpdate(gba);
	}
}

void _dmaEvent(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	UNUSED(timing);
	UNUSED(cyclesLate);
	struct GBA* gba = context;
	struct GBAMemory* memory = &gba->memory;
	struct GBADMA* dma = &memory->dma[memory->activeDMA];
	if (dma->nextCount == dma->count) {
		dma->when = mTimingCurrentTime(&gba->timing);
	}
	if (dma->nextCount & 0xFFFFF) {
		GBADMAService(gba, memory->activeDMA, dma);
	} else {
		dma->nextCount = 0;
		bool noRepeat = !GBADMARegisterIsRepeat(dma->reg);
		noRepeat |= GBADMARegisterGetTiming(dma->reg) == GBA_DMA_TIMING_NOW;
		noRepeat |= memory->activeDMA == 3 && GBADMARegisterGetTiming(dma->reg) == GBA_DMA_TIMING_CUSTOM && gba->video.vcount == GBA_VIDEO_VERTICAL_PIXELS + 1;
		if (noRepeat) {
			dma->reg = GBADMARegisterClearEnable(dma->reg);

			// Clear the enable bit in memory
			memory->io[(REG_DMA0CNT_HI + memory->activeDMA * (REG_DMA1CNT_HI - REG_DMA0CNT_HI)) >> 1] &= 0x7FE0;
		}
		if (GBADMARegisterGetDestControl(dma->reg) == GBA_DMA_INCREMENT_RELOAD) {
			dma->nextDest = dma->dest;
		}
		if (GBADMARegisterIsDoIRQ(dma->reg)) {
			GBARaiseIRQ(gba, GBA_IRQ_DMA0 + memory->activeDMA, cyclesLate);
		}
		GBADMAUpdate(gba);
	}
}

void GBADMAUpdate(struct GBA* gba) {
	int i;
	struct GBAMemory* memory = &gba->memory;
	uint32_t currentTime = mTimingCurrentTime(&gba->timing);
	int32_t leastTime = INT_MAX;
	memory->activeDMA = -1;
	for (i = 0; i < 4; ++i) {
		struct GBADMA* dma = &memory->dma[i];
		if (GBADMARegisterIsEnable(dma->reg) && dma->nextCount) {
			int32_t time = dma->when - currentTime;
			if (memory->activeDMA == -1 || time < leastTime) {
				leastTime = time;
				memory->activeDMA = i;
			}
		}
	}

	if (memory->activeDMA >= 0) {
		gba->dmaPC = gba->cpu->gprs[ARM_PC];
		mTimingDeschedule(&gba->timing, &memory->dmaEvent);
		mTimingSchedule(&gba->timing, &memory->dmaEvent, memory->dma[memory->activeDMA].when - currentTime);
	} else {
		gba->cpuBlocked = false;
	}
}

// The common shapes, RAM or ROM into palette, VRAM or OAM, done inline: the
// same loads, stores and renderer notifications GBALoad/GBAStore would do,
// without two calls through the memory handlers per unit.
static bool _fastLoad(struct GBAMemory* memory, uint32_t address, uint32_t width, uint32_t* value) {
	switch (address >> BASE_OFFSET) {
	case REGION_WORKING_RAM:
		if (width == 4) {
			LOAD_32(*value, address & (SIZE_WORKING_RAM - 4), memory->wram);
		} else {
			LOAD_16(*value, address & (SIZE_WORKING_RAM - 2), memory->wram);
		}
		return true;
	case REGION_WORKING_IRAM:
		if (width == 4) {
			LOAD_32(*value, address & (SIZE_WORKING_IRAM - 4), memory->iwram);
		} else {
			LOAD_16(*value, address & (SIZE_WORKING_IRAM - 2), memory->iwram);
		}
		return true;
	case REGION_CART0:
	case REGION_CART0_EX:
	case REGION_CART1:
	case REGION_CART1_EX:
	case REGION_CART2:
		if ((address & (SIZE_CART0 - 1)) >= memory->romSize) {
			return false;
		}
		if (width == 4) {
			LOAD_32(*value, address & (SIZE_CART0 - 4), memory->rom);
		} else {
			LOAD_16(*value, address & (SIZE_CART0 - 2), memory->rom);
		}
		return true;
	default:
		return false;
	}
}

static bool _fastStoreOK(uint32_t address) {
	switch (address >> BASE_OFFSET) {
	case REGION_PALETTE_RAM:
	case REGION_OAM:
		return true;
	case REGION_VRAM:
		return (address & 0x0001FFFF) < SIZE_VRAM;
	default:
		return false;
	}
}

static void _fastStore(struct GBA* gba, uint32_t address, uint32_t width, uint32_t value) {
	struct GBAVideoRenderer* renderer = gba->video.renderer;
	if (width == 4) {
		uint32_t oldValue;
		switch (address >> BASE_OFFSET) {
		case REGION_PALETTE_RAM:
			LOAD_32(oldValue, address & (SIZE_PALETTE_RAM - 4), gba->video.palette);
			if (oldValue != value) {
				STORE_32(value, address & (SIZE_PALETTE_RAM - 4), gba->video.palette);
				renderer->writePalette(renderer, (address & (SIZE_PALETTE_RAM - 4)) + 2, value >> 16);
				renderer->writePalette(renderer, address & (SIZE_PALETTE_RAM - 4), value);
			}
			break;
		case REGION_VRAM:
			LOAD_32(oldValue, address & 0x0001FFFC, gba->video.vram);
			if (oldValue != value) {
				STORE_32(value, address & 0x0001FFFC, gba->video.vram);
				renderer->writeVRAM(renderer, (address & 0x0001FFFC) + 2);
				renderer->writeVRAM(renderer, address & 0x0001FFFC);
			}
			break;
		case REGION_OAM:
			LOAD_32(oldValue, address & (SIZE_OAM - 4), gba->video.oam.raw);
			if (oldValue != value) {
				STORE_32(value, address & (SIZE_OAM - 4), gba->video.oam.raw);
				renderer->writeOAM(renderer, (address & (SIZE_OAM - 4)) >> 1);
				renderer->writeOAM(renderer, ((address & (SIZE_OAM - 4)) >> 1) + 1);
			}
			break;
		}
	} else {
		uint16_t oldValue;
		uint16_t half = value;
		switch (address >> BASE_OFFSET) {
		case REGION_PALETTE_RAM:
			LOAD_16(oldValue, address & (SIZE_PALETTE_RAM - 2), gba->video.palette);
			if (oldValue != half) {
				STORE_16(half, address & (SIZE_PALETTE_RAM - 2), gba->video.palette);
				renderer->writePalette(renderer, address & (SIZE_PALETTE_RAM - 2), half);
			}
			break;
		case REGION_VRAM:
			LOAD_16(oldValue, address & 0x0001FFFE, gba->video.vram);
			if (oldValue != half) {
				STORE_16(half, address & 0x0001FFFE, gba->video.vram);
				renderer->writeVRAM(renderer, address & 0x0001FFFE);
			}
			break;
		case REGION_OAM:
			LOAD_16(oldValue, address & (SIZE_OAM - 2), gba->video.oam.raw);
			if (oldValue != half) {
				STORE_16(half, address & (SIZE_OAM - 2), gba->video.oam.raw);
				renderer->writeOAM(renderer, (address & (SIZE_OAM - 2)) >> 1);
			}
			break;
		}
	}
}

// Units of a DMA are separate events, but while the CPU is blocked nothing
// else runs between them except other events. If none is due before the next
// unit would be, and the transfer only moves memory (no IO, no save chips, so
// nothing that looks at the time), running that unit now is indistinguishable
// from running it later, and saves an event dispatch per unit.
static bool _batchRegion(uint32_t region) {
	return region >= REGION_WORKING_RAM && region <= REGION_CART2 && region != REGION_IO;
}

static bool _canBatch(struct GBA* gba, int number, struct GBADMA* info) {
	struct mTiming* timing = &gba->timing;
	if (timing->reroot) {
		return false;
	}
	struct mTimingEvent* next = timing->root;
	if (next == &gba->memory.dmaEvent) {
		next = next->next;
	}
	if (next && (int32_t) (next->when - info->when) <= 0) {
		return false;
	}
	uint32_t sourceRegion = info->nextSource >> BASE_OFFSET;
	uint32_t destRegion = info->nextDest >> BASE_OFFSET;
	if (!info->nextSource || !_batchRegion(sourceRegion) || !_batchRegion(destRegion) || destRegion >= REGION_CART0) {
		return false;
	}
	int i;
	for (i = 0; i < 4; ++i) {
		if (i != number && GBADMARegisterIsEnable(gba->memory.dma[i].reg) && gba->memory.dma[i].nextCount) {
			return false;
		}
	}
	return true;
}

// A run of sequential units from plain memory into RAM or video memory, done as
// one loop. Covers exactly the units _canBatch would let through one by one:
// n is capped at the first unit another event would come before, and at the
// end of either memory block. Returns how many units it ran, 0 if this
// transfer isn't the simple kind.
static int32_t _bulk(struct GBA* gba, int number, struct GBADMA* info) {
	struct GBAMemory* memory = &gba->memory;
	struct mTiming* timing = &gba->timing;
	struct GBAVideoRenderer* renderer = gba->video.renderer;
	uint32_t width = 2 << GBADMARegisterGetWidth(info->reg);
	uint32_t source = info->nextSource;
	uint32_t dest = info->nextDest;
	uint32_t sourceRegion = source >> BASE_OFFSET;
	uint32_t destRegion = dest >> BASE_OFFSET;
	if (timing->reroot || !source) {
		return 0;
	}
	int i;
	for (i = 0; i < 4; ++i) {
		if (i != number && GBADMARegisterIsEnable(memory->dma[i].reg) && memory->dma[i].nextCount) {
			return 0;
		}
	}

	const void* sourceBase;
	uint32_t sourceOffset;
	uint32_t sourceSize;
	switch (sourceRegion) {
	case REGION_WORKING_RAM:
		sourceBase = memory->wram;
		sourceOffset = source & (SIZE_WORKING_RAM - width);
		sourceSize = SIZE_WORKING_RAM;
		break;
	case REGION_WORKING_IRAM:
		sourceBase = memory->iwram;
		sourceOffset = source & (SIZE_WORKING_IRAM - width);
		sourceSize = SIZE_WORKING_IRAM;
		break;
	case REGION_CART0:
	case REGION_CART0_EX:
	case REGION_CART1:
	case REGION_CART1_EX:
	case REGION_CART2:
		sourceBase = memory->rom;
		sourceOffset = source & (SIZE_CART0 - width);
		sourceSize = memory->romSize & -width;
		break;
	default:
		return 0;
	}
	void* destBase;
	uint32_t destOffset;
	uint32_t destSize;
	bool plain = false;
	switch (destRegion) {
	case REGION_WORKING_RAM:
		destBase = memory->wram;
		destOffset = dest & (SIZE_WORKING_RAM - width);
		destSize = SIZE_WORKING_RAM;
		plain = true;
		break;
	case REGION_WORKING_IRAM:
		destBase = memory->iwram;
		destOffset = dest & (SIZE_WORKING_IRAM - width);
		destSize = SIZE_WORKING_IRAM;
		plain = true;
		break;
	case REGION_PALETTE_RAM:
		destBase = gba->video.palette;
		destOffset = dest & (SIZE_PALETTE_RAM - width);
		destSize = SIZE_PALETTE_RAM;
		break;
	case REGION_VRAM:
		destBase = gba->video.vram;
		destOffset = dest & (0x00020000 - width);
		destSize = SIZE_VRAM;
		break;
	case REGION_OAM:
		destBase = gba->video.oam.raw;
		destOffset = dest & (SIZE_OAM - width);
		destSize = SIZE_OAM;
		break;
	default:
		return 0;
	}

	int sourceStep;
	if (source >= BASE_CART0 && source < BASE_CART_SRAM && GBADMARegisterGetSrcControl(info->reg) < 3) {
		sourceStep = width;
	} else {
		sourceStep = DMA_OFFSET[GBADMARegisterGetSrcControl(info->reg)] * width;
	}
	int destStep = DMA_OFFSET[GBADMARegisterGetDestControl(info->reg)] * width;

	int32_t n = info->nextCount;
	if (sourceOffset >= sourceSize || destOffset >= destSize) {
		return 0;
	}
	int32_t room;
	if (sourceStep) {
		room = sourceStep > 0 ? (sourceSize - sourceOffset) / width : sourceOffset / width + 1;
		if (room < n) {
			n = room;
		}
	}
	if (destStep) {
		room = destStep > 0 ? (destSize - destOffset) / width : destOffset / width + 1;
		if (room < n) {
			n = room;
		}
	}
	int32_t cycles = 2;
	if (width == 4) {
		cycles += memory->waitstatesSeq32[sourceRegion] + memory->waitstatesSeq32[destRegion];
	} else {
		cycles += memory->waitstatesSeq16[sourceRegion] + memory->waitstatesSeq16[destRegion];
	}
	struct mTimingEvent* next = timing->root;
	if (next == &memory->dmaEvent) {
		next = next->next;
	}
	if (next) {
		int32_t until = next->when - info->when;
		room = until <= 0 ? 1 : (until + cycles - 1) / cycles;
		if (room < n) {
			n = room;
		}
	}
	if (n < 2) {
		return 0;
	}

	gba->cpuBlocked = true;
	uint32_t value = 0;
	bool changed = false;
	uint32_t last = 0;
	int32_t left;
	if (plain && width == 4) {
		for (left = n; left; --left, sourceOffset += sourceStep, destOffset += destStep) {
			LOAD_32(value, sourceOffset, sourceBase);
			STORE_32(value, destOffset, destBase);
		}
	} else if (plain) {
		for (left = n; left; --left, sourceOffset += sourceStep, destOffset += destStep) {
			LOAD_16(value, sourceOffset, sourceBase);
			STORE_16(value, destOffset, destBase);
		}
		value = (value & 0xFFFF) | (value << 16);
	} else if (width == 4) {
		for (left = n; left; --left, sourceOffset += sourceStep, destOffset += destStep) {
			uint32_t oldValue;
			LOAD_32(value, sourceOffset, sourceBase);
			LOAD_32(oldValue, destOffset, destBase);
			if (oldValue == value) {
				continue;
			}
			STORE_32(value, destOffset, destBase);
			if (destRegion == REGION_PALETTE_RAM) {
				renderer->writePalette(renderer, destOffset + 2, value >> 16);
				renderer->writePalette(renderer, destOffset, value);
			} else if (destRegion == REGION_OAM) {
				renderer->writeOAM(renderer, destOffset >> 1);
				renderer->writeOAM(renderer, (destOffset >> 1) + 1);
			} else if (renderer->coarseVRAM && !renderer->cache) {
				changed = true;
				last = destOffset;
			} else {
				renderer->writeVRAM(renderer, destOffset + 2);
				renderer->writeVRAM(renderer, destOffset);
			}
		}
	} else {
		for (left = n; left; --left, sourceOffset += sourceStep, destOffset += destStep) {
			uint16_t oldValue;
			LOAD_16(value, sourceOffset, sourceBase);
			LOAD_16(oldValue, destOffset, destBase);
			if (oldValue == (uint16_t) value) {
				continue;
			}
			STORE_16(value, destOffset, destBase);
			if (destRegion == REGION_PALETTE_RAM) {
				renderer->writePalette(renderer, destOffset, value);
			} else if (destRegion == REGION_OAM) {
				renderer->writeOAM(renderer, destOffset >> 1);
			} else if (renderer->coarseVRAM && !renderer->cache) {
				changed = true;
				last = destOffset;
			} else {
				renderer->writeVRAM(renderer, destOffset);
			}
		}
		value = (value & 0xFFFF) | (value << 16);
	}
	if (changed) {
		renderer->writeVRAM(renderer, last);
	}
	memory->dmaTransferRegister = value;
	gba->bus = value;
	gba->performingDMA = 0;

	info->when += cycles * n;
	info->nextCount -= n;
	info->nextSource = source + sourceStep * n;
	info->nextDest = dest + destStep * n;
	return n;
}

void GBADMAService(struct GBA* gba, int number, struct GBADMA* info) {
	struct GBAMemory* memory = &gba->memory;
	struct ARMCore* cpu = gba->cpu;
	uint32_t width;
	int32_t wordsRemaining;
	uint32_t source;
	uint32_t dest;
	uint32_t sourceRegion;
	uint32_t destRegion;
	do {
		if ((info->nextDest >> BASE_OFFSET) >= REGION_PALETTE_RAM && (info->nextDest >> BASE_OFFSET) <= REGION_OAM) {
			GBA_VIDEO_TOUCH(gba->video.renderer);
		}
		if (info->count != info->nextCount) {
			sourceRegion = info->nextSource >> BASE_OFFSET;
			destRegion = info->nextDest >> BASE_OFFSET;
			if (_bulk(gba, number, info)) {
				wordsRemaining = info->nextCount;
				continue;
			}
		}
		width = 2 << GBADMARegisterGetWidth(info->reg);
		wordsRemaining = info->nextCount;
		source = info->nextSource;
		dest = info->nextDest;
		sourceRegion = source >> BASE_OFFSET;
		destRegion = dest >> BASE_OFFSET;
		int32_t cycles = 2;

		gba->cpuBlocked = true;
		if (info->count == info->nextCount) {
			if (width == 4) {
				cycles += memory->waitstatesNonseq32[sourceRegion] + memory->waitstatesNonseq32[destRegion];
			} else {
				cycles += memory->waitstatesNonseq16[sourceRegion] + memory->waitstatesNonseq16[destRegion];
			}
		} else {
			if (width == 4) {
				cycles += memory->waitstatesSeq32[sourceRegion] + memory->waitstatesSeq32[destRegion];
			} else {
				cycles += memory->waitstatesSeq16[sourceRegion] + memory->waitstatesSeq16[destRegion];
			}
		}
		info->when += cycles;

		gba->performingDMA = 1 | (number << 1);
		uint32_t value;
		if (source && _fastStoreOK(dest) && _fastLoad(memory, source, width, &value)) {
			if (width == 2) {
				value = (value & 0xFFFF) | (value << 16);
			}
			memory->dmaTransferRegister = value;
			gba->bus = value;
			_fastStore(gba, dest, width, value);
		} else if (width == 4) {
			if (source) {
				memory->dmaTransferRegister = cpu->memory.load32(cpu, source, 0);
			}
			gba->bus = memory->dmaTransferRegister;
			cpu->memory.store32(cpu, dest, memory->dmaTransferRegister, 0);
		} else {
			if (sourceRegion == REGION_CART2_EX && (memory->savedata.type == SAVEDATA_EEPROM || memory->savedata.type == SAVEDATA_EEPROM512)) {
				memory->dmaTransferRegister = GBASavedataReadEEPROM(&memory->savedata);
				memory->dmaTransferRegister |= memory->dmaTransferRegister << 16;
			} else if (source) {
				memory->dmaTransferRegister = cpu->memory.load16(cpu, source, 0);
				memory->dmaTransferRegister |= memory->dmaTransferRegister << 16;
			}
			if (destRegion == REGION_CART2_EX) {
				if (memory->savedata.type == SAVEDATA_AUTODETECT) {
					mLOG(GBA_MEM, INFO, "Detected EEPROM savegame");
					GBASavedataInitEEPROM(&memory->savedata);
				}
				if (memory->savedata.type == SAVEDATA_EEPROM512 || memory->savedata.type == SAVEDATA_EEPROM) {
					GBASavedataWriteEEPROM(&memory->savedata, memory->dmaTransferRegister, wordsRemaining);
				}
			} else {
				cpu->memory.store16(cpu, dest, memory->dmaTransferRegister, 0);

			}
			gba->bus = memory->dmaTransferRegister;
		}

		int sourceOffset;
		if (info->nextSource >= BASE_CART0 && info->nextSource < BASE_CART_SRAM && GBADMARegisterGetSrcControl(info->reg) < 3) {
			sourceOffset = width;
		} else {
			sourceOffset = DMA_OFFSET[GBADMARegisterGetSrcControl(info->reg)] * width;
		}
		int destOffset = DMA_OFFSET[GBADMARegisterGetDestControl(info->reg)] * width;
		if (source) {
			source += sourceOffset;
		}
		dest += destOffset;
		--wordsRemaining;
		gba->performingDMA = 0;

		info->nextCount = wordsRemaining;
		info->nextSource = source;
		info->nextDest = dest;
	} while (wordsRemaining && _canBatch(gba, number, info));

	int i;
	for (i = 0; i < 4; ++i) {
		struct GBADMA* dma = &memory->dma[i];
		int32_t time = dma->when - info->when;
		if (time < 0 && GBADMARegisterIsEnable(dma->reg) && dma->nextCount) {
			dma->when = info->when;
		}
	}

	if (!wordsRemaining) {
		info->nextCount |= 0x80000000;
		if (sourceRegion < REGION_CART0 || destRegion < REGION_CART0) {
			info->when += 2;
		}
	}
	GBADMAUpdate(gba);
}
