/*
 * RAM disk implementing FatFs diskio.h for the CD image host test harness.
 * One malloc'd image, 512-byte sectors, physical drive 0 only.
 */
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "diskio.h"
#include "ramdisk_diskio.h"

static uint8_t *image;
static uint32_t n_sectors;

bool ramdisk_init(uint32_t sectors) {
    ramdisk_free();
    image = calloc(sectors, RAMDISK_SECTOR_SIZE);
    if (!image) return false;
    n_sectors = sectors;
    return true;
}

void ramdisk_free(void) {
    free(image);
    image = NULL;
    n_sectors = 0;
}

uint32_t ramdisk_sectors(void) {
    return n_sectors;
}

DSTATUS disk_status(BYTE pdrv) {
    return (pdrv == 0 && image) ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (disk_status(pdrv)) return RES_NOTRDY;
    if (sector + count > n_sectors) return RES_PARERR;
    memcpy(buff, image + (size_t)sector * RAMDISK_SECTOR_SIZE, (size_t)count * RAMDISK_SECTOR_SIZE);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (disk_status(pdrv)) return RES_NOTRDY;
    if (sector + count > n_sectors) return RES_PARERR;
    memcpy(image + (size_t)sector * RAMDISK_SECTOR_SIZE, buff, (size_t)count * RAMDISK_SECTOR_SIZE);
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (disk_status(pdrv)) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:        return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t *)buff = n_sectors; return RES_OK;
    case GET_SECTOR_SIZE:  *(WORD *)buff = RAMDISK_SECTOR_SIZE; return RES_OK;
    case GET_BLOCK_SIZE:   *(DWORD *)buff = 1; return RES_OK;
    default:               return RES_PARERR;
    }
}

/* FF_FS_NORTC is 0 in the firmware configuration; FatFs asks for a
   timestamp when it creates files on the host. */
DWORD get_fattime(void) {
    return ((DWORD)(2026 - 1980) << 25) | (1UL << 21) | (1UL << 16);
}
