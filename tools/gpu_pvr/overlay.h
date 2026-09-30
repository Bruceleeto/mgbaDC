// SPDX-License-Identifier: GPL-2.0-only
#ifndef OVERLAY_H
#define OVERLAY_H

#include "bloom-config.h"

#if WITH_FPS_OVERLAY

/* On-screen text (fps counter). Font and glyph loop lifted from DMS-Engine. */
void overlay_init(void);
/* Text to display; '\n' starts a new line. */
void overlay_set(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Submit the text into the currently open TR list, at depth z. */
void overlay_draw(float z);

#else

static inline void overlay_init(void) {}
static inline void overlay_draw(float z) { (void)z; }
#define overlay_set(...) do { } while (0)

#endif /* WITH_FPS_OVERLAY */

#endif
