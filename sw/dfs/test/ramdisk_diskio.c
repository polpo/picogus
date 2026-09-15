/*
 * RAM disk implementing FatFs diskio.h for the PGDFS host test harness.
 * One malloc'd image, 512-byte sectors, physical drive 0 only.
 *
 * It also stands in for usb_msc/msc_app.c's telemetry: the same msc_stats_t
 * the firmware keeps for DFS_AL_DIAG is maintained here, with a fault
 * injection switch so the tests can watch a failing write travel through
 * FatFs, dfs_fs.c and the DIAG record.
 */
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "diskio.h"
#include "ramdisk_diskio.h"
#include "../../usb_msc/msc_app.h"

static uint8_t *image;
static uint32_t n_sectors;
static msc_stats_t stats;
static bool write_fault, read_fault;

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

void ramdisk_set_write_fault(bool on) {
    write_fault = on;
}

void ramdisk_set_read_fault(bool on) {
    read_fault = on;
}

void ramdisk_reset_stats(void) {
    memset(&stats, 0, sizeof(stats));
}

const msc_stats_t *msc_app_get_stats(void) {
    return &stats;
}

/* what the firmware records for a drive answering CSW status 1 (failed) */
static void fail_like_csw(bool is_write) {
    stats.last_csw_status = 1;
    stats.last_csw_residue = RAMDISK_SECTOR_SIZE;
    if (is_write) stats.write_csw_err++; else stats.read_csw_err++;
}

DSTATUS disk_status(BYTE pdrv) {
    return (pdrv == 0 && image) ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    DRESULT res = RES_OK;
    uint8_t cause = MSC_IO_OK;
    stats.reads++;
    if (disk_status(pdrv)) {
        res = RES_NOTRDY;
        cause = MSC_IO_NODEV;
    } else if (sector + count > n_sectors) {
        res = RES_PARERR;
        cause = MSC_IO_REFUSED;
        stats.read_refused++;
    } else if (read_fault) {
        res = RES_ERROR;
        cause = MSC_IO_CSW;
        fail_like_csw(false);
    } else {
        memcpy(buff, image + (size_t)sector * RAMDISK_SECTOR_SIZE, (size_t)count * RAMDISK_SECTOR_SIZE);
    }
    stats.last_read_res = (uint8_t)res;
    stats.last_read_cause = cause;
    return res;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    DRESULT res = RES_OK;
    uint8_t cause = MSC_IO_OK;
    stats.writes++;
    stats.last_write_lba = (uint32_t)sector;
    stats.last_write_count = (uint16_t)count;
    stats.last_write_us = 0;
    if (disk_status(pdrv)) {
        res = RES_NOTRDY;
        cause = MSC_IO_NODEV;
    } else if (sector + count > n_sectors) {
        res = RES_PARERR;
        cause = MSC_IO_REFUSED;
        stats.write_refused++;
    } else if (write_fault) {
        res = RES_ERROR;
        cause = MSC_IO_CSW;
        fail_like_csw(true);
    } else {
        memcpy(image + (size_t)sector * RAMDISK_SECTOR_SIZE, buff, (size_t)count * RAMDISK_SECTOR_SIZE);
        stats.last_write_us = 100u * count;   /* a pretend transfer time */
    }
    stats.last_write_res = (uint8_t)res;
    stats.last_write_cause = cause;
    return res;
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
