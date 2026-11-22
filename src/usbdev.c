/* usbdev: see usbdev.h. Register names and bits follow the Renesas USB 2.0 function module
 * manuals (SH7262/SH7264 R01UH0134 §26, R8A66597); the power-up order is the calculator OS's. */
#include "usbdev.h"
#include <string.h>

#define R8(a)  (*(volatile uint8_t  *)(a))
#define R16(a) (*(volatile uint16_t *)(a))
#define R32(a) (*(volatile uint32_t *)(a))

/* power (OS 3.60 0x80208198 / 0x80208232) */
#define PFC180   0xA4050180u
#define PFC182   0xA4050182u
#define UNK1D4   0xA40501D4u
#define CPG014   0xA4150014u
#define MSTPCR2  0xA4150038u
#define R64CNT   0xA413FEC0u

/* the controller */
#define U(o)      R16(0xA4D80000u + (o))
#define SYSCFG    0x00
#define BUSWAIT   0x02
#define DVSTCTR0  0x08
#define CFIFO     0x14
#define CFIFOSEL  0x20
#define CFIFOCTR  0x22
#define INTENB0   0x30
#define BRDYENB   0x36
#define NRDYENB   0x38
#define BEMPENB   0x3A
#define INTSTS0   0x40
#define BRDYSTS   0x46
#define NRDYSTS   0x48
#define BEMPSTS   0x4A
#define USBREQ    0x54
#define USBVAL    0x56
#define USBINDX   0x58
#define USBLENG   0x5A
#define DCPCFG    0x5C
#define DCPMAXP   0x5E
#define DCPCTR    0x60
#define PIPESEL   0x64
#define PIPECFG   0x68
#define PIPEBUF   0x6A
#define PIPEMAXP  0x6C
#define PIPEPERI  0x6E
#define PIPECTR(n) (0x70 + 2 * ((n) - 1))
#define CFIFO8    R8(0xA4D80000u + CFIFO)   /* BIGEND = 1: byte access at the port address */

/* INTSTS0 bits (write 0 to clear the W0C ones) */
#define VBINT 0x8000
#define DVST  0x1000
#define CTRT  0x0800
#define VALID 0x0008
/* CFIFOSEL / CFIFOCTR */
#define BIGEND 0x0100
#define ISEL   0x0020
#define FRDY   0x2000
#define BVAL   0x8000
#define BCLR   0x4000
/* PID */
#define PID_NAK   0
#define PID_BUF   1
#define PID_STALL 2
#define SQCLR 0x0100
#define SQSET 0x0080
#define CCPL  0x0004
#define ACLRM 0x0200

#define VID 0x07CF        /* Casio Computer Co., Ltd. */
#define PID 0x6102
#define PIPE_IN  1        /* EP1 IN  */
#define PIPE_OUT 2        /* EP2 OUT */
#define MPS 64

int usbdev_nsetup, usbdev_nreset, usbdev_nstall;
static const usbdev_cb_t *cbs;
static uint8_t config;

#define LOG(...) do { if(cbs && cbs->log) cbs->log(__VA_ARGS__); } while(0)

static void spin_ms(int ms)                  /* busy wait on the RTC's 128 Hz count */
{
	int ticks = (ms * 128 + 999) / 1000 + 1, last = R8(R64CNT) & 0x7F, seen = 0;
	for(uint32_t g = 0; seen < ticks && g < 50000000u; g++) {
		int r = R8(R64CNT) & 0x7F;
		if(r != last) { seen += (r - last) & 0x7F; last = r; }
	}
}

/* ---- power and start ------------------------------------------------------------------------ */

int usbdev_running(void) { return !(R32(CPG014) & 0x100) && !(R32(MSTPCR2) & 0x100000); }

void usbdev_power_on(void)
{
	R16(PFC180) &= 0xFF3F;
	R16(PFC182) &= 0x3FFF;
	R32(CPG014) &= ~0x100u;
	spin_ms(110);
	R32(MSTPCR2) &= ~0x100000u;
	R16(UNK1D4) = 0x0600;
}

void usbdev_start(void)
{
	spin_ms(100);
	U(BUSWAIT) = 4;
	U(SYSCFG) = 0;      spin_ms(2);
	U(SYSCFG) = 0x400;  spin_ms(10);      /* SCKE */
	U(SYSCFG) = 0x401;  spin_ms(2);       /* USBE, full speed (HSE = 0) */
	U(0xC2) = 0x20;                        /* as the OS (reads 0x20 already) */
	U(DVSTCTR0) = 0;
	U(INTENB0) = 0; U(BRDYENB) = 0; U(NRDYENB) = 0; U(BEMPENB) = 0;   /* polled */
	U(DCPCFG) = 0;
	U(DCPMAXP) = MPS;
	config = 0;
}

void usbdev_attach(int on)
{
	if(on) U(SYSCFG) |= 0x10;
	else   U(SYSCFG) &= ~0x10;
}

void usbdev_power_off(void)
{
	if(usbdev_running()) U(SYSCFG) = 0;
	R16(UNK1D4) = 0;
	R32(MSTPCR2) |= 0x100000u;
	R32(CPG014) |= 0x100u;
	R16(PFC182) |= 0xC000;
	R16(PFC180) = (R16(PFC180) & 0xFF3F) | 0x40;
}

int usbdev_state(void) { return usbdev_running() ? (U(INTSTS0) >> 4) & 7 : -1; }

/* ---- descriptors ---------------------------------------------------------------------------- */

static const uint8_t dev_desc[18] = {
	18, 1, 0x00, 0x02,            /* USB 2.0 */
	0, 0, 0, MPS,                 /* class per interface, EP0 64 bytes */
	VID & 0xFF, VID >> 8, PID & 0xFF, PID >> 8,
	0x00, 0x01,                   /* bcdDevice 1.00 */
	1, 2, 3,                      /* strings: manufacturer, product, serial */
	1,                            /* one configuration */
};
static const uint8_t cfg_desc[32] = {
	9, 2, 32, 0, 1, 1, 0, 0x80, 50,           /* 1 interface, config 1, bus powered, 100 mA */
	9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 0,       /* mass storage, SCSI transparent, bulk-only */
	7, 5, 0x80 | PIPE_IN, 2, MPS, 0, 0,       /* EP1 IN bulk 64 */
	7, 5, PIPE_OUT, 2, MPS, 0, 0,             /* EP2 OUT bulk 64 */
};
static uint8_t str_buf[2 + 2 * 32];

void usbdev_serial(char out[17])
{
	static const char hex[] = "0123456789ABCDEF";
	const volatile uint8_t *id = (const volatile uint8_t *)0xA001FFD0u;
	for(int i = 0; i < 8; i++) { out[2*i] = hex[id[i] >> 4]; out[2*i+1] = hex[id[i] & 15]; }
	out[16] = 0;
}

static int str_desc(int index)
{
	static const char *const s[] = { NULL, "FlashDump", "fx-CG50 flash", NULL };
	char serial[17];
	const char *t;
	if(index == 0) { str_buf[0] = 4; str_buf[1] = 3; str_buf[2] = 0x09; str_buf[3] = 0x04; return 4; }
	if(index == 3) { usbdev_serial(serial); t = serial; }
	else if(index < 3) t = s[index];
	else return -1;
	int n = 0;
	while(t[n] && n < 32) { str_buf[2 + 2*n] = t[n]; str_buf[3 + 2*n] = 0; n++; }
	str_buf[0] = 2 + 2 * n; str_buf[1] = 3;
	return 2 + 2 * n;
}

/* ---- the default control pipe --------------------------------------------------------------- */

static void dcp_pid(int pid) { U(DCPCTR) = pid; }

static void dcp_stall(void) { dcp_pid(PID_STALL); usbdev_nstall++; }

static void dcp_ack(void) { U(DCPCTR) = PID_BUF | CCPL; }   /* no-data request: status stage */

/* control read, Casio's way (OS 3.60 0x8073D2FE): NAK, select the DCP for writing, clear its
 * buffer (BCLR: a request the host abandoned can leave data in it, and then FRDY never comes
 * back), write the answer, BVAL if short, then PID = BUF and CCPL together. No waiting: the
 * hardware sends the packet and runs the status stage. All our answers fit one packet. */
static int dcp_send(const uint8_t *d, int len, int wlength)
{
	if(len > wlength) len = wlength;
	if(len > MPS) { LOG("  dcp: answer cut to %d bytes\n", MPS); len = MPS; }
	U(DCPCTR) = SQSET;                           /* PID = NAK, DATA1 */
	U(CFIFOSEL) = BIGEND | ISEL;
	uint32_t g;
	for(g = 0; g < 100000 && (U(CFIFOSEL) & 0x2F) != ISEL; g++) {}
	U(CFIFOCTR) = BCLR;
	for(g = 0; g < 1000000 && !(U(CFIFOCTR) & FRDY); g++) {}
	if(!(U(CFIFOCTR) & FRDY)) { LOG("  dcp: FIFO not ready\n"); dcp_stall(); return -1; }
	for(int i = 0; i < len; i++) CFIFO8 = d[i];
	if(len < MPS) U(CFIFOCTR) = BVAL;            /* (a full 64-byte packet goes by itself) */
	U(DCPCTR) = PID_BUF | CCPL;
	return 0;
}

/* ---- bulk pipes ----------------------------------------------------------------------------- */

/* Bulk data goes through the two DMA-capable FIFO ports, as Casio's driver does: IN on D1FIFO
 * (32-bit words for sector data), OUT on D0FIFO. The CFIFO stays with the DCP, so a control
 * request in the middle of a transfer can't change the port the data uses. */
#define D0FIFO    0x18
#define D1FIFO    0x1C
#define D0FIFOSEL 0x28
#define D0FIFOCTR 0x2A
#define D1FIFOSEL 0x2C
#define D1FIFOCTR 0x2E
#define MBW32     0x0800
#define DBLB      0x0200

static void pipe_setup(int pipe, uint16_t cfg, uint16_t buf)
{
	U(PIPECTR(pipe)) = PID_NAK;
	U(PIPESEL) = pipe;
	U(PIPECFG) = cfg;
	U(PIPEBUF) = buf;
	U(PIPEMAXP) = MPS;
	U(PIPEPERI) = 0;
	U(PIPESEL) = 0;
	U(PIPECTR(pipe)) = ACLRM;
	U(PIPECTR(pipe)) = 0;
	U(PIPECTR(pipe)) = SQCLR;
}

static int dsel(int port_sel, uint16_t v)        /* select a pipe on D0/D1FIFO, read back */
{
	U(port_sel) = v;
	for(uint32_t g = 0; g < 100000; g++) if((U(port_sel) & 0x0F) == (v & 0x0F)) return 1;
	return 0;
}

static void configure(int c)
{
	config = c;
	U(D0FIFOSEL) = 0; U(D1FIFOSEL) = 0;
	if(!c) { U(PIPECTR(PIPE_IN)) = PID_NAK; U(PIPECTR(PIPE_OUT)) = PID_NAK; return; }
	pipe_setup(PIPE_IN, 0x4000 | DBLB | 0x0010 | 1, (0 << 10) | 8);   /* bulk IN EP1, 2 x 64 B, blocks 8-9 */
	pipe_setup(PIPE_OUT, 0x4000 | 0x0080 | 2, (0 << 10) | 10);        /* bulk OUT EP2, SHTNAK, block 10 */
	dsel(D1FIFOSEL, BIGEND | MBW32 | PIPE_IN);
	dsel(D0FIFOSEL, BIGEND | PIPE_OUT);
	U(BRDYSTS) = 0xFFFF & ~(1 << PIPE_OUT);
	U(PIPECTR(PIPE_OUT)) = PID_BUF;
}

void usbdev_out_rearm(void) { if(config) U(PIPECTR(PIPE_OUT)) = PID_BUF; }

/* wait until the CPU side of the IN pipe can take a packet; give up on a new SETUP, a bus reset
 * or a lost configuration (the caller then abandons the transfer) */
static int in_wait(void)
{
	for(uint32_t g = 0; g < 30000000u; g++) {
		if(U(D1FIFOCTR) & FRDY) return 0;
		if(!config || (U(INTSTS0) & (VALID | DVST))) return -1;
	}
	return -2;
}

int usbdev_in_send(const uint8_t *d, int len)     /* one packet, bytes, len <= 64 */
{
	if(!config) return -1;
	U(D1FIFOSEL) = BIGEND | PIPE_IN;              /* 8-bit for odd sizes */
	int rc = in_wait();
	if(rc) return rc;
	volatile uint8_t *port = (volatile uint8_t *)(0xA4D80000u + D1FIFO);
	for(int i = 0; i < len; i++) *port = d[i];
	if(len < MPS) U(D1FIFOCTR) = BVAL;
	U(PIPECTR(PIPE_IN)) = PID_BUF;
	return 0;
}

int usbdev_in_send64w(const uint32_t *w)           /* one full 64-byte packet, 16 words */
{
	if(!config) return -1;
	U(D1FIFOSEL) = BIGEND | MBW32 | PIPE_IN;
	int rc = in_wait();
	if(rc) return rc;
	volatile uint32_t *port = (volatile uint32_t *)(0xA4D80000u + D1FIFO);
	for(int i = 0; i < 16; i++) *port = w[i];
	U(PIPECTR(PIPE_IN)) = PID_BUF;
	return 0;
}

static void halt_clear(int ep)
{
	int pipe = ep == (0x80 | PIPE_IN) ? PIPE_IN : ep == PIPE_OUT ? PIPE_OUT : 0;
	if(!pipe) return;
	U(PIPECTR(pipe)) = PID_STALL; U(PIPECTR(pipe)) = PID_NAK;
	U(PIPECTR(pipe)) = SQCLR;
	if(pipe == PIPE_OUT) U(PIPECTR(pipe)) = PID_BUF;
}

static void setup(void)
{
	uint16_t req = U(USBREQ), val = U(USBVAL), idx = U(USBINDX), len = U(USBLENG);
	U(INTSTS0) = 0xFFFF & ~VALID;
	for(int g = 0; g < 1000 && (U(INTSTS0) & VALID); g++) {}   /* else the next poll sees it again */
	usbdev_nsetup++;
	LOG("SETUP %04X %04X %04X %04X\n", req, val, idx, len);

	switch(req) {
	case 0x0000: break;                                  /* empty VALID after a bus reset */
	case 0x0500: break;                                  /* SET_ADDRESS: done by the hardware */
	case 0x0680: {                                       /* GET_DESCRIPTOR */
		int type = val >> 8, n;
		if(type == 1) dcp_send(dev_desc, sizeof dev_desc, len);
		else if(type == 2) dcp_send(cfg_desc, sizeof cfg_desc, len);
		else if(type == 3 && (n = str_desc(val & 0xFF)) > 0) dcp_send(str_buf, n, len);
		else dcp_stall();                                /* qualifier (FS only), BOS, ... */
		break; }
	case 0x0080: case 0x0081: case 0x0082: {             /* GET_STATUS */
		uint8_t st[2] = { 0, 0 };
		if(req == 0x0082) {
			int pipe = idx == (0x80 | PIPE_IN) ? PIPE_IN : idx == PIPE_OUT ? PIPE_OUT : 0;
			if(pipe && (U(PIPECTR(pipe)) & 2)) st[0] = 1;
		}
		dcp_send(st, 2, len); break; }
	case 0x0880: dcp_send(&config, 1, len); break;       /* GET_CONFIGURATION */
	case 0x0900:                                         /* SET_CONFIGURATION */
		if(val > 1) { dcp_stall(); break; }
		configure(val); dcp_ack(); break;
	case 0x0A81: { uint8_t z = 0; dcp_send(&z, 1, len); break; }   /* GET_INTERFACE */
	case 0x0B01: if(val) dcp_stall(); else dcp_ack(); break;       /* SET_INTERFACE */
	case 0x0102: if(val == 0) { halt_clear(idx & 0xFF); dcp_ack(); } else dcp_stall(); break;
	case 0x0302:                                         /* SET_FEATURE(ENDPOINT_HALT) */
		if(val == 0 && (idx & 0xFF) == (0x80 | PIPE_IN)) { U(PIPECTR(PIPE_IN)) = PID_STALL; dcp_ack(); }
		else if(val == 0 && (idx & 0xFF) == PIPE_OUT) { U(PIPECTR(PIPE_OUT)) = PID_STALL; dcp_ack(); }
		else dcp_stall();
		break;
	case 0xFEA1: { uint8_t lun = 0; dcp_send(&lun, 1, len); break; }   /* Get Max LUN */
	case 0xFF21:                                         /* Bulk-Only Mass Storage Reset */
		if(config) configure(config);
		if(cbs && cbs->reset) cbs->reset();
		dcp_ack(); break;
	default: dcp_stall(); break;
	}
}

static void bulk_out(void)
{
	static uint8_t buf[MPS];
	U(BRDYSTS) = 0xFFFF & ~(1 << PIPE_OUT);
	uint32_t g;
	for(g = 0; g < 1000000 && !(U(D0FIFOCTR) & FRDY); g++) {}
	if(!(U(D0FIFOCTR) & FRDY)) { LOG("bulk OUT: FIFO not ready\n"); return; }
	int n = U(D0FIFOCTR) & 0x0FFF;
	if(n > MPS) n = MPS;
	volatile uint8_t *port = (volatile uint8_t *)(0xA4D80000u + D0FIFO);
	for(int i = 0; i < n; i++) buf[i] = *port;
	if(n == 0) U(D0FIFOCTR) = BCLR;
	if(cbs && cbs->bulk_out) cbs->bulk_out(buf, n);
}

void usbdev_poll(const usbdev_cb_t *cb)
{
	cbs = cb;
	if(!usbdev_running()) return;
	uint16_t i = U(INTSTS0);
	if(i & VBINT) { U(INTSTS0) = 0xFFFF & ~VBINT; LOG("VBUS %d\n", (i >> 7) & 1); }
	if(i & DVST) {
		U(INTSTS0) = 0xFFFF & ~DVST;
		int st = (i >> 4) & 7;
		LOG("state %d (DVSTCTR0 %04X)\n", st, U(DVSTCTR0));
		if(st == 1) {                                      /* bus reset: back to Default */
			usbdev_nreset++; config = 0;
			if(cbs && cbs->reset) cbs->reset();
		}
	}
	if(i & CTRT) {
		U(INTSTS0) = 0xFFFF & ~CTRT;
		if((i & 7) == 6) LOG("control sequence error\n");
	}
	if(i & VALID) setup();
	if(config && (U(BRDYSTS) & (1 << PIPE_OUT))) bulk_out();
}
