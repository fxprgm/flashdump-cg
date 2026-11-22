/* msc: USB mass storage, bulk-only transport + the SCSI commands a host needs to mount a
 * read-only disk. The disk is fatimg (fatimg.c). Runs from usbdev's callbacks. */
#ifndef MSC_H
#define MSC_H

#include <stdint.h>

#ifndef MSC_REVISION
#define MSC_REVISION "0.3 "                        /* INQUIRY product revision, 4 characters */
#endif

void msc_bulk_out(const uint8_t *data, int len);   /* usbdev bulk_out callback */
void msc_reset(void);                              /* usbdev reset callback */
void msc_init(void);                               /* everything back to the start state */

/* the host's view, for the screen */
extern uint32_t msc_ncmd, msc_sectors, msc_errors;
extern uint8_t msc_lastop;
extern int msc_ejected;                            /* the host ejected the disk */

/* optional log (NULL = none); READ(10) is not logged after the first few */
extern void (*msc_log)(const char *fmt, ...);

#endif
