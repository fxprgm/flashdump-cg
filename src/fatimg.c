/* fatimg: see fatimg.h. Constant-only divisions (shifts) on the hot path: the SH-4 has no
 * divide instruction. */
#include "fatimg.h"
#include <string.h>

#define PART_START   2048u          /* 1 MiB-aligned partition, as disk tools make them */
#define SPC          8u             /* sectors per cluster: 4 KB clusters */
#define SPC_SHIFT    3u
#define ROOT_ENTRIES 512u
#define ROOT_SECS    (ROOT_ENTRIES * 32u / FATIMG_SECTOR)
#define NFATS        2u

static fatimg_cfg_t cfg;
static uint32_t flash_clusters;     /* FLASH.BIN: clusters 2 .. 1 + flash_clusters */
static uint32_t info_cluster;       /* INFO.TXT: the one after */
static uint32_t nclusters, fat_secs, vol_secs;
static uint32_t fat_start, root_start, data_start;   /* relative to PART_START */

static void w16(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static void w32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

uint16_t fatimg_date(int y, int m, int d) { return ((y - 1980) << 9) | (m << 5) | d; }
uint16_t fatimg_time(int h, int m, int s) { return (h << 11) | (m << 5) | (s >> 1); }

void fatimg_init(const fatimg_cfg_t *c)
{
	cfg = *c;
	if(cfg.info_len > SPC * FATIMG_SECTOR) cfg.info_len = SPC * FATIMG_SECTOR;
	flash_clusters = cfg.flash_size >> (SPC_SHIFT + 9);
	info_cluster = 2 + flash_clusters;
	nclusters = flash_clusters + 1;               /* >= 4085 for 16 MB and up: FAT16 */
	fat_secs = ((nclusters + 2) * 2 + FATIMG_SECTOR - 1) / FATIMG_SECTOR;
	fat_start = 1;
	root_start = fat_start + NFATS * fat_secs;
	data_start = root_start + ROOT_SECS;
	vol_secs = data_start + nclusters * SPC;
}

uint32_t fatimg_sectors(void) { return PART_START + vol_secs; }

static void mbr(uint8_t *s)
{
	uint8_t *p = s + 0x1BE;
	p[0] = 0x00;                                   /* not bootable */
	p[1] = 0xFE; p[2] = 0xFF; p[3] = 0xFF;          /* CHS: "use LBA" */
	p[4] = 0x0E;                                   /* FAT16, LBA */
	p[5] = 0xFE; p[6] = 0xFF; p[7] = 0xFF;
	w32(p + 8, PART_START);
	w32(p + 12, vol_secs);
	w32(s + 0x1B8, cfg.serial);                    /* disk signature */
	s[510] = 0x55; s[511] = 0xAA;
}

static void boot(uint8_t *s)
{
	static const uint8_t jmp[3] = { 0xEB, 0x3C, 0x90 };
	memcpy(s, jmp, 3);
	memcpy(s + 3, "MSDOS5.0", 8);
	w16(s + 11, FATIMG_SECTOR);
	s[13] = SPC;
	w16(s + 14, fat_start);                        /* reserved sectors */
	s[16] = NFATS;
	w16(s + 17, ROOT_ENTRIES);
	w16(s + 19, vol_secs < 65536 ? vol_secs : 0);
	s[21] = 0xF8;                                  /* fixed disk */
	w16(s + 22, fat_secs);
	w16(s + 24, 63);
	w16(s + 26, 255);
	w32(s + 28, PART_START);                       /* hidden sectors */
	w32(s + 32, vol_secs < 65536 ? 0 : vol_secs);
	s[36] = 0x80;
	s[38] = 0x29;
	w32(s + 39, cfg.serial);
	memcpy(s + 43, "CG50 FLASH ", 11);
	memcpy(s + 54, "FAT16   ", 8);
	s[62] = 0xF4; s[63] = 0xEB; s[64] = 0xFD;      /* boot code: hlt; jmp $-1 */
	s[510] = 0x55; s[511] = 0xAA;
}

static uint32_t fat_entry(uint32_t e)
{
	if(e == 0) return 0xFFF8;
	if(e == 1) return 0xFFFF;                      /* clean, no errors */
	if(e < info_cluster) return e + 1 == info_cluster ? 0xFFFF : e + 1;
	if(e == info_cluster) return 0xFFFF;
	return 0;
}

static void dirent(uint8_t *p, const char *name11, uint8_t attr, uint32_t cluster, uint32_t size)
{
	memcpy(p, name11, 11);
	p[11] = attr;
	w16(p + 14, cfg.fat_time); w16(p + 16, cfg.fat_date); w16(p + 18, cfg.fat_date);
	w16(p + 22, cfg.fat_time); w16(p + 24, cfg.fat_date);
	w16(p + 26, cluster);
	w32(p + 28, size);
}

void fatimg_read(uint32_t lba, uint8_t *s)
{
	memset(s, 0, FATIMG_SECTOR);
	if(lba == 0) { mbr(s); return; }
	if(lba < PART_START || lba >= PART_START + vol_secs) return;
	uint32_t r = lba - PART_START;

	if(r == 0) { boot(s); return; }
	if(r < root_start) {
		uint32_t k = r - fat_start;
		if(k >= fat_secs) k -= fat_secs;               /* FAT #2 = FAT #1 */
		uint32_t e = k << 8;                           /* 256 entries per sector */
		for(uint32_t i = 0; i < 256; i++) w16(s + 2 * i, fat_entry(e + i));
		return;
	}
	if(r < data_start) {
		if(r != root_start) return;
		dirent(s,      "CG50 FLASH ", 0x08, 0, 0);
		dirent(s + 32, "FLASH   BIN", 0x21, 2, cfg.flash_size);
		dirent(s + 64, "INFO    TXT", 0x21, info_cluster, cfg.info_len);
		return;
	}
	uint32_t d = r - data_start;                      /* sector within the data area */
	uint32_t fsecs = flash_clusters << SPC_SHIFT;
	if(d < fsecs) { cfg.flash_read(d << 9, s, FATIMG_SECTOR); return; }
	uint32_t off = (d - fsecs) << 9;
	if(off < cfg.info_len) {
		uint32_t n = cfg.info_len - off;
		memcpy(s, cfg.info + off, n < FATIMG_SECTOR ? n : FATIMG_SECTOR);
	}
}
