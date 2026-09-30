/* Host stand-in for KOS <arch/arch.h>: PVR_RAM_SIZE asks for the system type. */
#ifndef __ARCH_ARCH_H
#define __ARCH_ARCH_H
#include <stdint.h>
#define HW_TYPE_RETAIL 0x0
#define HW_TYPE_SET5   0x1
static inline int hardware_sys_mode(int *region) { if (region) *region = 0; return HW_TYPE_RETAIL; }
#endif
