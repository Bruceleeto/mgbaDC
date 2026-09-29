/* Copyright (c) 2013-2016 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/core/timing.h>

#include <mgba/core/profile.h>

#ifdef M_PROFILE
static uint32_t _noCounter(void) {
	return 0;
}

uint32_t (*mProfileClock)(void) = _noCounter;
uint32_t (*mProfileEvents)(void) = _noCounter;
struct mProfileEntry* mProfileCurrent;
uint32_t mProfileArmInstructions;
uint32_t mProfileThumbInstructions;

static struct mProfileEntry _profileEntries[mPROFILE_MAX_ENTRIES];
static unsigned _profileNumEntries;
static struct mProfileEntry _profileOverflow = { .name = "(profiler table full)" };

struct mProfileEntry* mProfileLookup(const char* name, struct mProfileEntry* parent) {
	unsigned i;
	for (i = 0; i < _profileNumEntries; ++i) {
		if (_profileEntries[i].name == name && _profileEntries[i].parent == parent) {
			return &_profileEntries[i];
		}
	}
	if (_profileNumEntries == mPROFILE_MAX_ENTRIES) {
		return &_profileOverflow;
	}
	struct mProfileEntry* entry = &_profileEntries[_profileNumEntries];
	++_profileNumEntries;
	entry->name = name;
	entry->parent = parent;
	return entry;
}

void mProfileReset(void) {
	unsigned i;
	for (i = 0; i < _profileNumEntries; ++i) {
		_profileEntries[i].ticks = 0;
		_profileEntries[i].events = 0;
		_profileEntries[i].count = 0;
	}
	_profileOverflow.ticks = 0;
	_profileOverflow.events = 0;
	_profileOverflow.count = 0;
	mProfileArmInstructions = 0;
	mProfileThumbInstructions = 0;
}

/* Cost of one empty begin/end pair, in ticks. Nested pairs are included in
 * their parent's time, so this shows how much to discount busy sections. */
static uint32_t _profileScopeCost(void) {
	static const char name[] = "(calibration)";
	static struct mProfileEntry* cache;
	struct mProfileEntry* saved = mProfileCurrent;
	uint32_t best = UINT32_MAX;
	int i;
	for (i = 0; i < 64; ++i) {
		struct mProfileScope scope;
		uint32_t start = mProfileClock();
		mProfileBegin(&scope, name, &cache);
		mProfileEnd(&scope);
		uint32_t cost = mProfileClock() - start;
		if (cost < best) {
			best = cost;
		}
	}
	mProfileCurrent = saved;
	cache->ticks = 0;
	cache->events = 0;
	cache->count = 0;
	return best;
}

static void _profilePrintLine(int depth, const char* name, uint64_t ticks, uint64_t events, double count,
                              unsigned frames, double ticksPerMs, const char* eventsLabel) {
	printf("  %*s%-*s %7.2f ms", depth * 2, "", 34 - depth * 2, name, ticks / ticksPerMs / frames);
	if (count >= 0) {
		printf(" %8.1f calls", count / frames);
	} else {
		printf("               ");
	}
	if (eventsLabel && ticks) {
		printf(" %5.1f%% %s", events * 100.0 / ticks, eventsLabel);
	}
	printf("\n");
}

static void _profilePrintChildren(struct mProfileEntry* parent, int depth, unsigned frames, double ticksPerMs,
                                  const char* eventsLabel) {
	unsigned i;
	for (i = 0; i < _profileNumEntries; ++i) {
		struct mProfileEntry* entry = &_profileEntries[i];
		if (entry->parent != parent || !entry->count) {
			continue;
		}
		_profilePrintLine(depth, entry->name, entry->ticks, entry->events, entry->count, frames, ticksPerMs, eventsLabel);

		uint64_t childTicks = 0;
		uint64_t childEvents = 0;
		bool hasChildren = false;
		unsigned j;
		for (j = 0; j < _profileNumEntries; ++j) {
			if (_profileEntries[j].parent == entry && _profileEntries[j].count) {
				childTicks += _profileEntries[j].ticks;
				childEvents += _profileEntries[j].events;
				hasChildren = true;
			}
		}
		if (hasChildren) {
			_profilePrintChildren(entry, depth + 1, frames, ticksPerMs, eventsLabel);
			uint64_t selfTicks = entry->ticks > childTicks ? entry->ticks - childTicks : 0;
			uint64_t selfEvents = entry->events > childEvents ? entry->events - childEvents : 0;
			_profilePrintLine(depth + 1, "(self)", selfTicks, selfEvents, -1, frames, ticksPerMs, eventsLabel);
		}
	}
}

void mProfilePrint(double coreMs, unsigned frames, double ticksPerMs, const char* eventsLabel) {
	static uint32_t scopeCost;
	if (!frames) {
		return;
	}
	if (!scopeCost) {
		scopeCost = _profileScopeCost();
	}
	uint64_t topTicks = 0;
	uint64_t scopes = 0;
	unsigned i;
	for (i = 0; i < _profileNumEntries; ++i) {
		if (!_profileEntries[i].parent) {
			topTicks += _profileEntries[i].ticks;
		}
		scopes += _profileEntries[i].count;
	}
	printf("  %-34s %7.2f ms\n", "outside sections (CPU etc.)", coreMs - topTicks / ticksPerMs / frames);
	uint32_t instructions = mProfileArmInstructions + mProfileThumbInstructions;
	if (instructions) {
		printf("  %-34s %7.0fk ARM %7.0fk Thumb per frame (%.0f%% Thumb)\n", "instructions",
		       mProfileArmInstructions / 1000.0 / frames, mProfileThumbInstructions / 1000.0 / frames,
		       mProfileThumbInstructions * 100.0 / instructions);
	}
	_profilePrintChildren(NULL, 0, frames, ticksPerMs, eventsLabel);
	if (_profileOverflow.count) {
		_profilePrintLine(0, _profileOverflow.name, _profileOverflow.ticks, _profileOverflow.events,
		                  _profileOverflow.count, frames, ticksPerMs, eventsLabel);
	}
	printf("  %-34s %7.2f ms (%.0f ticks x %.0f scopes/frame, spread over the tree)\n", "profiler overhead est.",
	       (double) scopeCost * scopes / ticksPerMs / frames, (double) scopeCost, (double) scopes / frames);
	mProfileReset();
}
#endif

void mTimingInit(struct mTiming* timing, int32_t* relativeCycles, int32_t* nextEvent) {
	timing->root = NULL;
	timing->reroot = NULL;
	timing->globalCycles = 0;
	timing->masterCycles = 0;
	timing->relativeCycles = relativeCycles;
	timing->nextEvent = nextEvent;
}

void mTimingDeinit(struct mTiming* timing) {
	UNUSED(timing);
}

void mTimingClear(struct mTiming* timing) {
	timing->root = NULL;
	timing->reroot = NULL;
	timing->globalCycles = 0;
	timing->masterCycles = 0;
}

void mTimingInterrupt(struct mTiming* timing) {
	if (!timing->root) {
		return;
	}
	timing->reroot = timing->root;
	timing->root = NULL;
}

void mTimingSchedule(struct mTiming* timing, struct mTimingEvent* event, int32_t when) {
	int32_t nextEvent = when + *timing->relativeCycles;
	event->when = nextEvent + timing->masterCycles;
	if (nextEvent < *timing->nextEvent) {
		*timing->nextEvent = nextEvent;
	}
	if (timing->reroot) {
		timing->root = timing->reroot;
		timing->reroot = NULL;
	}
	struct mTimingEvent** previous = &timing->root;
	struct mTimingEvent* next = timing->root;
	unsigned priority = event->priority;
	while (next) {
		int32_t nextWhen = next->when - timing->masterCycles;
		if (nextWhen > nextEvent || (nextWhen == nextEvent && next->priority > priority)) {
			break;
		}
		previous = &next->next;
		next = next->next;
	}
	event->next = next;
	*previous = event;
}

void mTimingScheduleAbsolute(struct mTiming* timing, struct mTimingEvent* event, int32_t when) {
	mTimingSchedule(timing, event, when - mTimingCurrentTime(timing));
}

void mTimingDeschedule(struct mTiming* timing, struct mTimingEvent* event) {
	if (timing->reroot) {
		timing->root = timing->reroot;
		timing->reroot = NULL;
	}
	struct mTimingEvent** previous = &timing->root;
	struct mTimingEvent* next = timing->root;
	while (next) {
		if (next == event) {
			*previous = next->next;
			return;
		}
		previous = &next->next;
		next = next->next;
	}
}

bool mTimingIsScheduled(const struct mTiming* timing, const struct mTimingEvent* event) {
	const struct mTimingEvent* next = timing->root;
	if (!next) {
		next = timing->reroot;
	}
	while (next) {
		if (next == event) {
			return true;
		}
		next = next->next;
	}
	return false;
}

int32_t mTimingTick(struct mTiming* timing, int32_t cycles) {
	timing->masterCycles += cycles;
	uint32_t masterCycles = timing->masterCycles;
	while (timing->root) {
		struct mTimingEvent* next = timing->root;
		int32_t nextWhen = next->when - masterCycles;
		if (nextWhen > 0) {
			return nextWhen;
		}
		timing->root = next->next;
		mPROFILE_START(profileEvent, next->name ? next->name : "(unnamed event)");
		next->callback(timing, next->context, -nextWhen);
		mPROFILE_STOP(profileEvent);
	}
	if (timing->reroot) {
		timing->root = timing->reroot;
		timing->reroot = NULL;
		*timing->nextEvent = mTimingNextEvent(timing);
		if (*timing->nextEvent <= 0) {
			return mTimingTick(timing, 0);
		}
	}
	return *timing->nextEvent;
}

int32_t mTimingCurrentTime(const struct mTiming* timing) {
	return timing->masterCycles + *timing->relativeCycles;
}

uint64_t mTimingGlobalTime(const struct mTiming* timing) {
	return timing->globalCycles + *timing->relativeCycles;
}

int32_t mTimingNextEvent(struct mTiming* timing) {
	struct mTimingEvent* next = timing->root;
	if (!next) {
		return INT_MAX;
	}
	return next->when - timing->masterCycles - *timing->relativeCycles;
}

int32_t mTimingUntil(const struct mTiming* timing, const struct mTimingEvent* event) {
	return event->when - timing->masterCycles - *timing->relativeCycles;
}
