/*
 * FlashDump: makes an fx-CG50 show up on a computer or phone as a read-only USB drive with one
 * file, FLASH.BIN, the whole 32 MB flash chip (plus INFO.TXT). The computer needs no software:
 * it uses its own USB mass storage driver. FlashDump only reads; it writes nothing to the flash.
 *
 *   usbdev.c   the USB function controller (polled, full speed, our own driver)
 *   msc.c      USB mass storage (bulk-only transport) and the SCSI commands of a read-only disk
 *   fatimg.c   the FAT16 disk, generated sector by sector when the computer reads it
 *
 * Build with the fxSDK: fxsdk build-cg -> FLASHDMP.g3a
 *
 * The add-in owns the USB block while it runs: it powers it up the way the OS does, attaches,
 * serves the disk, and on EXIT detaches and puts the power registers back as it found them, so
 * the OS's own USB (USB Flash mode, data transfer) works as before. MENU is ignored on purpose:
 * the OS must not get control with the USB block set up for FlashDump.
 *
 * gint_setrestart(1): after EXIT the add-in can be opened again straight from the MAIN MENU
 * (without it the OS tries to resume the finished add-in, which just drops back to the menu).
 * A restart begins at main() again, so main() resets every counter itself.
 *
 * F1 detaches and attaches again, as if the cable had been replugged: after an eject the
 * computer mounts the drive again without touching the cable.
 */
#include <gint/display.h>
#include <gint/keyboard.h>
#include <gint/rtc.h>
#include <gint/gint.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "usbdev.h"
#include "msc.h"
#include "fatimg.h"

#define VERSION "0.3.0"
#define FLASH_SIZE 0x02000000u   /* the fx-CG50's flash: 32 MB at 0x80000000 */

#define R8(a)  (*(volatile uint8_t  *)(a))
#define R16(a) (*(volatile uint16_t *)(a))
#define R32(a) (*(volatile uint32_t *)(a))

/* the registers usbdev_power_on() changes, put back on exit */
#define PFC180  0xA4050180u
#define PFC182  0xA4050182u
#define UNK1D4  0xA40501D4u
#define CPG014  0xA4150014u
#define MSTPCR2 0xA4150038u
#define USB     0xA4D80000u
#define R64CNT  0xA413FEC0u   /* RTC 128 Hz count: the clock for the screen */

/* colours: a teal for headings, terracotta for warnings, grey for notes */
#define C_TEAL  C_RGB(3, 12, 13)
#define C_TERRA C_RGB(25, 11, 6)
#define C_GREY  C_RGB(12, 12, 12)
#define C_OK    C_RGB(2, 18, 4)

/* ---- the disk -------------------------------------------------------------------------------- */

static void flash_read(uint32_t off, uint8_t *dst, uint32_t len)
{
	memcpy(dst, (const void *)(0x80000000u + off), len);   /* the flash through P1 (cached) */
}

/* The OS version from the flash header ("CASIOWIN" at 0x80020000, the version at 0x80020020),
   e.g. "03.60.0000"; "unknown" without a valid header. */
static void os_version(char out[11])
{
	const char *v = (const char *)0x80020020;
	strcpy(out, "unknown");
	if(memcmp((const char *)0x80020000, "CASIOWIN", 8)) return;
	for(int i = 0; i < 10; i++) {
		char c = v[i];
		if((i == 2 || i == 5) ? c != '.' : (c < '0' || c > '9')) return;
	}
	memcpy(out, v, 10);
	out[10] = 0;
}

/* The build date, for the files' dates when the calculator's clock is not set (a calculator
   that was never set counts from 2010). */
static void build_date(int *y, int *m, int *d)
{
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	const char *s = __DATE__;            /* "Nov 22 2026" */
	*m = 1;
	for(int i = 0; i < 12; i++) if(!memcmp(s, months + 3 * i, 3)) *m = i + 1;
	*d = (s[4] == ' ' ? 0 : s[4] - '0') * 10 + (s[5] - '0');
	*y = (s[7] - '0') * 1000 + (s[8] - '0') * 100 + (s[9] - '0') * 10 + (s[10] - '0');
}

static char os[11], info[600];
static int clock_set;

static void disk_init(void)
{
	rtc_time_t t;
	rtc_get_time(&t);
	int y = t.year, mo = t.month + 1, d = t.month_day, h = t.hours, mi = t.minutes, s = t.seconds;
	clock_set = y >= 2020 && y <= 2099;
	char when[64];
	if(clock_set) snprintf(when, sizeof when, "%04d-%02d-%02d %02d:%02d (calculator clock)", y, mo, d, h, mi);
	else {
		build_date(&y, &mo, &d); h = 12; mi = s = 0;
		strcpy(when, "unknown (the calculator's clock is not set)");
	}

	char id[17];
	usbdev_serial(id);
	os_version(os);
	int n = snprintf(info, sizeof info,
		"FlashDump " VERSION ", fx-CG50\r\n"
		"\r\n"
		"FLASH.BIN  the calculator's whole flash chip, 33554432 bytes:\r\n"
		"           boot code, OS, system area, storage memory\r\n"
		"OS         %s\r\n"
		"Read on    %s\r\n"
		"Calculator %s\r\n"
		"\r\n"
		"FLASH.BIN holds your own files and settings as well as\r\n"
		"Casio's OS: keep it to yourself.\r\n",
		os, when, id);
	fatimg_cfg_t c = {
		.flash_size = FLASH_SIZE, .flash_read = flash_read,
		.info = info, .info_len = n,
		.fat_date = fatimg_date(y, mo, d),
		.fat_time = fatimg_time(h, mi, s),
		.serial = 0xC6500000u ^ ((uint32_t)R8(0xA001FFD6u) << 8 | R8(0xA001FFD7u)),
	};
	fatimg_init(&c);
}

/* ---- the screen ------------------------------------------------------------------------------ */

static int y;

static void line(color_t c, const char *text) { dtext(8, y, c, text); y += 15; }

static uint32_t ticks;
static int last_r64;

static void tick(void)
{
	int r = R8(R64CNT) & 0x7F;
	ticks += (r - last_r64) & 0x7F;
	last_r64 = r;
}

static uint32_t t_rate, s_rate, rate;      /* KB/s over the last second */

static void draw(void)
{
	if(ticks - t_rate >= 128) {
		rate = (msc_sectors - s_rate) / 2 * 128 / (ticks - t_rate);
		t_rate = ticks; s_rate = msc_sectors;
	}

	dclear(C_WHITE);
	y = 6;
	line(C_TEAL, "FlashDump " VERSION ": the flash as a USB drive");
	drect(8, y - 3, 387, y - 2, C_TEAL);
	y += 4;
	dprint(8, y, C_BLACK, "OS %s", os); y += 15;
	if(!clock_set) line(C_GREY, "(clock not set: the files get the build date)");
	y += 6;
	line(C_BLACK, "1. Connect the USB cable to a computer or phone.");
	line(C_BLACK, "2. Open the drive CG50 FLASH, copy FLASH.BIN.");
	line(C_BLACK, "3. Eject the drive, then press EXIT here.");
	y += 8;

	int vbus = (R16(USB + 0x40) >> 7) & 1, st = usbdev_state();
	if(msc_ejected)
		line(C_OK, "Ejected: unplug, or F1 to connect again.");
	else if(!vbus)
		line(C_TERRA, msc_ncmd ? "Cable unplugged." : "Waiting for the USB cable...");
	else if(st != 3 || !msc_ncmd)
		line(C_TERRA, "Connected: waiting for the computer...");
	else
		line(C_OK, "The drive CG50 FLASH is ready.");

	/* progress: sectors the computer has read, against the size of FLASH.BIN */
	uint32_t kb = msc_sectors / 2, total = FLASH_SIZE >> 10;
	if(kb > total) kb = total;
	drect_border(8, y, 387, y + 12, C_NONE, 1, C_TEAL);
	int w = (int)(378u * kb / total);
	if(w > 0) drect(9, y + 1, 9 + w, y + 11, C_TEAL);
	y += 18;
	dprint(8, y, C_GREY, "Read %d.%d of 32 MB", (int)(kb >> 10), (int)((kb & 1023) * 10 >> 10));
	if(rate && !msc_ejected) dprint(200, y, C_GREY, "%d KB/s", (int)rate);
	y += 22;

	line(C_GREY, "FlashDump only reads: nothing on the calculator");
	line(C_GREY, "changes. The copy takes about 40 seconds.");
	dtext(8, 204, C_BLACK, "[F1] Connect again     [EXIT] Quit");
	dupdate();
}

/* ---- main ------------------------------------------------------------------------------------ */

static const usbdev_cb_t cb = { .log = NULL, .bulk_out = msc_bulk_out, .reset = msc_reset };

int main(void)
{
	uint16_t pfc180 = R16(PFC180), pfc182 = R16(PFC182), unk1d4 = R16(UNK1D4);
	uint32_t cpg014 = R32(CPG014), mstpcr2 = R32(MSTPCR2);

	gint_setrestart(1);
	ticks = 0; last_r64 = R8(R64CNT) & 0x7F;
	t_rate = s_rate = rate = 0;
	msc_init();
	disk_init();
	draw();

	if(!usbdev_running()) usbdev_power_on();
	usbdev_start();              /* always from scratch, whatever the OS left behind */
	usbdev_attach(1);

	while(1) {
		uint32_t t0 = ticks;
		for(uint32_t g = 0; ticks - t0 < 12 && g < 3000000u; g++) { tick(); usbdev_poll(&cb); }
		draw();

		key_event_t e;
		int quit = 0;
		while((e = pollevent()).type != KEYEV_NONE) {
			if(e.type != KEYEV_DOWN) continue;
			if(e.key == KEY_EXIT) quit = 1;
			if(e.key == KEY_F1) {    /* look unplugged, then plugged in again: the computer mounts the drive anew */
				msc_init();
				t_rate = ticks; s_rate = rate = 0;
				usbdev_attach(0);    /* the computer sees the device leave... */
				for(uint32_t t0 = ticks; ticks - t0 < 64; ) tick();   /* 0.5 s */
				usbdev_start();      /* ...a fresh controller... */
				usbdev_attach(1);    /* ...and a new device arrive */
			}
		}
		if(quit) break;
	}

	/* detach, controller off, power registers as at launch */
	if(usbdev_running()) R16(USB) = 0;
	R16(UNK1D4) = unk1d4; R32(MSTPCR2) = mstpcr2; R32(CPG014) = cpg014;
	R16(PFC182) = pfc182; R16(PFC180) = pfc180;
	return 1;
}
