#ifndef DC_PVR_GBA_H
#define DC_PVR_GBA_H

#include <mgba-util/common.h>

#include <dc/pvr.h>

struct GBA;

#define PVR_GBA_MODE DM_320x240
/* The offline harness renders 1:1 (240x160) to compare pixels */
#ifndef PVR_GBA_WIDTH
#define PVR_GBA_WIDTH 320
#define PVR_GBA_HEIGHT 240
#endif

extern const pvr_init_params_t PVRGBAInitParams;

bool PVRGBAInit(struct GBA* gba);
void PVRGBAFrame(struct GBA* gba, uint64_t* uploadTime, uint64_t* submitTime);
/* The upload's parts in microseconds, added up: dirty bits and palette,
 * BG layers, sprites */
extern uint64_t PVRGBAUploadSplit[3];

#endif
