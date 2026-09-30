/* KOS PVR API on libpvr: the pieces the glue (gpulib_if.c) drives directly. */
#ifndef KOS_PVR_H
#define KOS_PVR_H
#include <stdint.h>
#include <sys/cdefs.h>
__BEGIN_DECLS
int  kos_pvr_init(void);
void kos_pvr_shutdown(void);
/* Copy the front buffer (the last completed non-RTT render) out as 24-bit
 * RGB rows of `stride` bytes.  Returns 0 if nothing has been rendered yet. */
int  kos_pvr_read_front(uint8_t *rgb, int stride, int w, int h);

/* implemented by the gpulib shim; called after every libpvr render */
void kos_pvr_present(void);
__END_DECLS
#endif
