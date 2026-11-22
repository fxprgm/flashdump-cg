/* Host test for fatimg: hosttest <flash dump> <out image>. Writes every sector of the generated
 * disk to <out image>; check it with fsck_msdos and by mounting it (tests/test.sh). */
#include "fatimg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *flash;
static void flash_read(uint32_t off, uint8_t *dst, uint32_t len) { memcpy(dst, flash + off, len); }

int main(int argc, char **argv)
{
	if(argc != 3) { fprintf(stderr, "usage: hosttest <flash dump> <out image>\n"); return 2; }
	FILE *f = fopen(argv[1], "rb");
	if(!f) { perror(argv[1]); return 1; }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);
	flash = malloc(n);
	if(fread(flash, 1, n, f) != (size_t)n) { perror("read"); return 1; }
	fclose(f);

	static const char info[] =
		"fx-CG50 flash dump made by FlashDump (host test)\r\n"
		"FLASH.BIN: the whole flash chip, 33554432 bytes\r\n";
	fatimg_cfg_t c = {
		.flash_size = (uint32_t)n, .flash_read = flash_read,
		.info = info, .info_len = sizeof info - 1,
		.fat_date = fatimg_date(2026, 11, 17), .fat_time = fatimg_time(22, 30, 0),
		.serial = 0xC6501234,
	};
	fatimg_init(&c);
	uint32_t ns = fatimg_sectors();
	FILE *o = fopen(argv[2], "wb");
	if(!o) { perror(argv[2]); return 1; }
	uint8_t s[FATIMG_SECTOR];
	for(uint32_t i = 0; i < ns; i++) { fatimg_read(i, s); fwrite(s, 1, sizeof s, o); }
	fclose(o);
	printf("%u sectors (%.1f MB) written to %s\n", ns, ns / 2048.0, argv[2]);
	return 0;
}
