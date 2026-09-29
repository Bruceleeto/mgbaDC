/* Copyright (c) 2013-2016 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef M_PROFILE_H
#define M_PROFILE_H

#include <mgba-util/common.h>

CXX_GUARD_START

/* Hierarchical section profiler, compiled in only with M_PROFILE.
 *
 * mPROFILE_START(var, "name") ... mPROFILE_STOP(var) times a section. Sections
 * nest at runtime: a section's parent is whichever section was open when it
 * started, so the same name under different parents is tracked separately.
 * Names are compared by pointer, so pass string literals or other stable
 * pointers.
 *
 * The frontend supplies mProfileClock, a free-running 32-bit tick counter
 * (wraparound is fine), and optionally mProfileEvents, a second hardware
 * event counter (e.g. cache-miss stall cycles) accumulated alongside. */
#ifdef M_PROFILE
#define mPROFILE_MAX_ENTRIES 96

struct mProfileEntry {
	const char* name;
	struct mProfileEntry* parent;
	uint64_t ticks;
	uint64_t events;
	uint32_t count;
};

struct mProfileScope {
	struct mProfileEntry* entry;
	struct mProfileEntry* parent;
	uint32_t ticks;
	uint32_t events;
};

extern uint32_t (*mProfileClock)(void);
extern uint32_t (*mProfileEvents)(void);
extern struct mProfileEntry* mProfileCurrent;

struct mProfileEntry* mProfileLookup(const char* name, struct mProfileEntry* parent);

/* Prints the tree as per-frame averages, then resets the totals. coreMs is
 * the frontend's own per-frame measurement of the code the profiled sections
 * run inside; time outside every top-level section is reported against it.
 * eventsLabel names the mProfileEvents column, or NULL to omit it. */
void mProfilePrint(double coreMs, unsigned frames, double ticksPerMs, const char* eventsLabel);
void mProfileReset(void);

static inline void mProfileBegin(struct mProfileScope* scope, const char* name, struct mProfileEntry** cache) {
	struct mProfileEntry* parent = mProfileCurrent;
	struct mProfileEntry* entry = *cache;
	if (!entry || entry->name != name || entry->parent != parent) {
		entry = mProfileLookup(name, parent);
		*cache = entry;
	}
	scope->entry = entry;
	scope->parent = parent;
	mProfileCurrent = entry;
	scope->events = mProfileEvents();
	scope->ticks = mProfileClock();
}

static inline void mProfileEnd(struct mProfileScope* scope) {
	uint32_t ticks = mProfileClock() - scope->ticks;
	uint32_t events = mProfileEvents() - scope->events;
	struct mProfileEntry* entry = scope->entry;
	entry->ticks += ticks;
	entry->events += events;
	++entry->count;
	mProfileCurrent = scope->parent;
}

#define mPROFILE_START(VAR, NAME) \
	static struct mProfileEntry* VAR ## Cache; \
	struct mProfileScope VAR; \
	mProfileBegin(&VAR, NAME, &VAR ## Cache)
#define mPROFILE_STOP(VAR) mProfileEnd(&VAR)
#else
#define mPROFILE_START(VAR, NAME)
#define mPROFILE_STOP(VAR) do {} while (0)
#endif

CXX_GUARD_END

#endif
