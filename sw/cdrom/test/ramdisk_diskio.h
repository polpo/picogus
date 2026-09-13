/* RAM disk behind FatFs diskio.h for the CD image host tests. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define RAMDISK_SECTOR_SIZE 512

bool     ramdisk_init(uint32_t sectors);   /* allocates and zeroes the image */
void     ramdisk_free(void);
uint32_t ramdisk_sectors(void);
