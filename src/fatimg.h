/* fatimg: a read-only FAT16 disk generated on the fly, sector by sector, for FlashDump's USB
 * drive. Nothing is stored: every 512-byte sector is computed when the host asks for it.
 *
 *   LBA 0                 MBR, one partition (type 0x0E, FAT16 LBA) starting at PART_START
 *   PART_START            FAT16 boot sector (volume label "CG50 FLASH")
 *   + 1                   FAT #1, then FAT #2 (identical)
 *   then                  root directory (512 entries): label, FLASH.BIN, INFO.TXT
 *   then                  data: FLASH.BIN (contiguous clusters), then INFO.TXT
 *
 * FLASH.BIN's bytes come from a callback (on the calculator: the flash itself). Plain C99,
 * no allocation, builds for the SH-4 and on the Mac (host tests). */
#ifndef FATIMG_H
#define FATIMG_H

#include <stdint.h>

#define FATIMG_SECTOR 512u

typedef struct {
	uint32_t flash_size;                  /* bytes, a multiple of 4096 (32 MB on the fx-CG50) */
	void (*flash_read)(uint32_t offset, uint8_t *dst, uint32_t len);
	const char *info;                     /* INFO.TXT contents (<= 4096 bytes) */
	uint32_t info_len;
	uint16_t fat_date, fat_time;          /* FAT-format timestamp for all entries */
	uint32_t serial;                      /* volume serial number */
} fatimg_cfg_t;

void fatimg_init(const fatimg_cfg_t *cfg);
uint32_t fatimg_sectors(void);            /* disk size in sectors */
void fatimg_read(uint32_t lba, uint8_t *dst);   /* one sector */

/* FAT date/time from calendar values */
uint16_t fatimg_date(int year, int month, int day);
uint16_t fatimg_time(int hour, int min, int sec);

#endif
