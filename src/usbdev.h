/* usbdev: a small polled USB device driver for the fx-CG50's USB function controller
 * (0xA4D80000, Renesas USB 2.0 function module family), written from experiments on the
 * calculator, the way its OS drives the block, and Renesas's public manuals. No interrupts:
 * call usbdev_poll() often. Full speed only. One configuration with one interface
 * (mass storage, bulk-only) and two bulk endpoints: EP1 IN on pipe 1 (D1FIFO, double-buffered),
 * EP2 OUT on pipe 2 (D0FIFO); the DCP uses the CFIFO. */
#ifndef USBDEV_H
#define USBDEV_H

#include <stdint.h>

typedef struct {
	void (*log)(const char *fmt, ...);              /* event log, may be NULL */
	/* a packet arrived on the bulk OUT endpoint (len <= 64); the pipe NAKs until
	   usbdev_out_rearm() is called */
	void (*bulk_out)(const uint8_t *data, int len);
	void (*reset)(void);         /* bus reset or Bulk-Only reset: drop the transfer in progress */
} usbdev_cb_t;

void usbdev_power_on(void);      /* pins, clock, module start, VBUS sense (the OS's sequence) */
void usbdev_start(void);         /* controller on, no pull-up (needs usbdev_power_on) */
void usbdev_attach(int on);      /* D+ pull-up: the host sees the device */
void usbdev_power_off(void);     /* the OS's power-off sequence */
int  usbdev_running(void);       /* the block is clocked and not module-stopped */
void usbdev_poll(const usbdev_cb_t *cb);
int  usbdev_state(void);         /* DVSQ: 0 powered, 1 default, 2 address, 3 configured, 4+ suspended */
void usbdev_out_rearm(void);     /* let the bulk OUT pipe receive the next packet */
/* queue one bulk IN packet; waits for buffer space (double-buffered), not for the send.
   0 = ok, < 0 = abandoned (new SETUP, bus reset, deconfigured) or timeout */
int  usbdev_in_send(const uint8_t *data, int len);   /* bytes, len <= 64 (short = end) */
int  usbdev_in_send64w(const uint32_t *words);       /* a full 64-byte packet as 16 words */

/* the calculator's 8-byte ID (boot area 0xA001FFD0) in hex: the USB and SCSI serial number */
void usbdev_serial(char out[17]);

/* counters for the screen */
extern int usbdev_nsetup, usbdev_nreset, usbdev_nstall;

#endif
