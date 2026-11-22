/* msc: see msc.h. Bulk-only transport (USB Mass Storage Class Bulk-Only Transport 1.0) with a
 * read-only SCSI block device. Error cases follow the spec's no-stall options: when the host
 * expects IN data we can't give, a short (or zero-length) packet ends the data stage and the
 * CSW reports the residue; data the host sends for a refused WRITE is read and dropped. */
#include "msc.h"
#include "usbdev.h"
#include "fatimg.h"
#include <string.h>

uint32_t msc_ncmd, msc_sectors, msc_errors;
uint8_t msc_lastop;
int msc_ejected;
void (*msc_log)(const char *fmt, ...);
#define LOG(...) do { if(msc_log) msc_log(__VA_ARGS__); } while(0)

static uint8_t tag[4];
static uint8_t sense_key, sense_asc, sense_ascq;
static uint32_t discard_left, discard_residue;      /* refused WRITE: OUT bytes to drop */
static int nread_logged;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void sense(uint8_t key, uint8_t asc, uint8_t ascq) { sense_key = key; sense_asc = asc; sense_ascq = ascq; }

static void csw(uint32_t residue, uint8_t status)
{
	uint8_t c[13] = { 'U', 'S', 'B', 'S', tag[0], tag[1], tag[2], tag[3],
		residue, residue >> 8, residue >> 16, residue >> 24, status };
	if(status) msc_errors++;
	usbdev_in_send(c, 13);
	usbdev_out_rearm();
}

/* send a small answer (<= dlen bytes), then the CSW */
static void reply(const uint8_t *d, uint32_t n, uint32_t dlen)
{
	if(n > dlen) n = dlen;
	uint32_t sent = 0;
	while(sent < n) {
		uint32_t k = n - sent > 64 ? 64 : n - sent;
		if(usbdev_in_send(d + sent, k) < 0) return;
		sent += k;
	}
	if(n < dlen && n % 64 == 0) usbdev_in_send(d, 0);   /* end the data stage early */
	sense(0, 0, 0);
	csw(dlen - n, 0);
}

static void fail(uint32_t dlen, int dir_in, uint8_t key, uint8_t asc, uint8_t ascq)
{
	sense(key, asc, ascq);
	if(dlen && dir_in) { usbdev_in_send(tag, 0); csw(dlen, 1); }
	else if(dlen) { discard_left = discard_residue = dlen; usbdev_out_rearm(); }
	else csw(0, 1);
}

static void read10(const uint8_t *cb, uint32_t dlen)
{
	uint32_t lba = be32(cb + 2), cnt = cb[7] << 8 | cb[8];
	if(msc_ejected) { fail(dlen, 1, 2, 0x3A, 0); return; }
	if(lba + cnt > fatimg_sectors() || cnt * FATIMG_SECTOR != dlen) {
		LOG("READ(10) %lu+%lu refused (dlen %lu)\n", (unsigned long)lba, (unsigned long)cnt, (unsigned long)dlen);
		fail(dlen, 1, 5, 0x21, 0);
		return;
	}
	if(nread_logged < 12) { nread_logged++; LOG("READ(10) lba %lu count %lu\n", (unsigned long)lba, (unsigned long)cnt); }
	static uint32_t sec[FATIMG_SECTOR / 4];
	for(uint32_t i = 0; i < cnt; i++) {
		fatimg_read(lba + i, (uint8_t *)sec);
		for(int p = 0; p < 8; p++)
			if(usbdev_in_send64w(sec + 16 * p) < 0) { LOG("READ(10) abandoned at sector %lu\n", (unsigned long)(lba + i)); return; }
		msc_sectors++;
	}
	sense(0, 0, 0);
	csw(0, 0);
}

static void command(const uint8_t *cb, uint32_t dlen, int dir_in)
{
	uint8_t a[36];
	msc_lastop = cb[0];
	if(cb[0] != 0x28) LOG("SCSI %02X %02X %02X %02X %02X %02X  dlen %lu %s\n", cb[0], cb[1], cb[2],
		cb[3], cb[4], cb[5], (unsigned long)dlen, dir_in ? "IN" : "OUT");
	switch(cb[0]) {
	case 0x00:                                          /* TEST UNIT READY */
		if(msc_ejected) fail(dlen, dir_in, 2, 0x3A, 0); else { sense(0, 0, 0); csw(0, 0); }
		break;
	case 0x03:                                          /* REQUEST SENSE */
		memset(a, 0, 18);
		a[0] = 0x70; a[2] = sense_key; a[7] = 10; a[12] = sense_asc; a[13] = sense_ascq;
		reply(a, 18, dlen);
		break;
	case 0x12:                                          /* INQUIRY */
		if(cb[1] & 1) {                                 /* VPD pages (Windows asks for 0x80) */
			memset(a, 0, 20);
			a[1] = cb[2];
			if(cb[2] == 0x00) { a[3] = 2; a[4] = 0x00; a[5] = 0x80; reply(a, 6, dlen); }
			else if(cb[2] == 0x80) { char s[17]; usbdev_serial(s); a[3] = 16; memcpy(a + 4, s, 16); reply(a, 20, dlen); }
			else fail(dlen, dir_in, 5, 0x24, 0);
			break;
		}
		memset(a, ' ', 36);
		a[0] = 0x00; a[1] = 0x80; a[2] = 0x04; a[3] = 0x02; a[4] = 31;   /* disk, removable */
		a[5] = a[6] = a[7] = 0;
		memcpy(a + 8, "FlashDmp", 8);
		memcpy(a + 16, "fx-CG50 flash", 13);
		memcpy(a + 32, MSC_REVISION, 4);
		reply(a, 36, dlen);
		break;
	case 0x1A:                                          /* MODE SENSE(6): header, write-protected */
		a[0] = 3; a[1] = 0; a[2] = 0x80; a[3] = 0;
		reply(a, 4, dlen);
		break;
	case 0x5A:                                          /* MODE SENSE(10) */
		memset(a, 0, 8); a[1] = 6; a[3] = 0x80;
		reply(a, 8, dlen);
		break;
	case 0x1B:                                          /* START STOP UNIT */
		if((cb[4] & 3) == 2) { msc_ejected = 1; LOG("ejected by the host\n"); }
		else if((cb[4] & 3) == 3) msc_ejected = 0;
		sense(0, 0, 0); csw(0, 0);
		break;
	case 0x1E: case 0x2F: case 0x35:                    /* PREVENT/ALLOW, VERIFY, SYNC CACHE */
		sense(0, 0, 0); csw(0, 0);
		break;
	case 0x23:                                          /* READ FORMAT CAPACITIES */
		memset(a, 0, 12);
		a[3] = 8; put_be32(a + 4, fatimg_sectors()); a[8] = 0x02; a[10] = FATIMG_SECTOR >> 8;
		reply(a, 12, dlen);
		break;
	case 0x25:                                          /* READ CAPACITY(10) */
		put_be32(a, fatimg_sectors() - 1); put_be32(a + 4, FATIMG_SECTOR);
		reply(a, 8, dlen);
		break;
	case 0x28: read10(cb, dlen); break;
	case 0x2A: case 0xAA: case 0x8A:                    /* WRITE(10/12/16): write protected */
		fail(dlen, dir_in, 7, 0x27, 0);
		break;
	default:
		fail(dlen, dir_in, 5, 0x20, 0);                 /* invalid command */
		break;
	}
}

void msc_bulk_out(const uint8_t *d, int n)
{
	if(discard_left) {                                  /* data of a refused WRITE */
		discard_left = (uint32_t)n >= discard_left ? 0 : discard_left - n;
		if(!discard_left) csw(discard_residue, 1); else usbdev_out_rearm();
		return;
	}
	if(n != 31 || d[0] != 'U' || d[1] != 'S' || d[2] != 'B' || d[3] != 'C') {
		LOG("bulk OUT: %d bytes, not a CBW: ignored\n", n);
		usbdev_out_rearm();
		return;
	}
	msc_ncmd++;
	memcpy(tag, d + 4, 4);
	uint32_t dlen = d[8] | d[9] << 8 | d[10] << 16 | (uint32_t)d[11] << 24;
	command(d + 15, dlen, (d[12] & 0x80) != 0);
}

void msc_reset(void)
{
	discard_left = 0;
}

void msc_init(void)
{
	msc_ncmd = msc_sectors = msc_errors = 0;
	msc_lastop = 0;
	msc_ejected = 0;
	nread_logged = 0;
	sense(0, 0, 0);
	msc_reset();
}
