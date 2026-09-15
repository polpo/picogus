/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

/*
 * USB mass storage glue: mounts the single USB drive as the FatFs volume ""
 * for CD-ROM image emulation and/or the PGDFS file service.
 *
 * TinyUSB delivers mount/unmount events from tuh_task(), and tuh_task() is
 * also pumped from wait_for_disk_io() underneath a live FatFs call. So the
 * callbacks only record what happened; the FatFs mount/unmount work and the
 * consumer hooks run from msc_app_task(), which every core 1 loop calls at
 * top level, never inside FatFs. A transfer whose device disappears fails
 * immediately instead of waiting for the timeout.
 */

#include <ctype.h>
#include "tusb.h"
/* #include "bsp/board_api.h" */
#include "pico/stdlib.h"
#include "../include/pg_debug.h"

#include "ff.h"
#include "diskio.h"

#include "msc_app.h"

#ifdef CDROM
#include "cdrom_image_manager.h"
#endif
#ifdef PGDFS
#include "dfs/dfs.h"
#include "dfs/dfs_fs.h"     /* dfs_platform_fatfs() */
#endif

//------------- Elm Chan FatFS -------------//
static FATFS fatfs; // for simplicity only support 1 device
static volatile bool _disk_busy;
static volatile bool _disk_error;
static volatile uint8_t _disk_cause;      // msc_io_cause_t of the transfer in flight
static bool _disk_is_write;               // direction of the transfer in flight
static uintptr_t _xfer_gen;               // tag of the transfer in flight (user_arg of the callback)
static msc_stats_t stats;                 // see msc_app.h; core 1 only
static volatile uint8_t mounted_dev;      // USB address of the attached drive, 0 = none
static volatile uint8_t pending_mount;    // drive whose INQUIRY completed, waiting for f_mount()
static volatile bool pending_unmount;     // the mounted drive went away, f_unmount() pending
static bool fs_mounted;                   // FatFs volume state, core 1 loop only

// define the buffer to be place in USB/DMA memory with correct alignment/cache line size
CFG_TUH_MEM_SECTION static struct {
  TUH_EPBUF_TYPE_DEF(scsi_inquiry_resp_t, inquiry);
} scsi_resp;


bool msc_app_init(void)
{
    _disk_busy = false;
    _disk_error = false;
    return true;
}

static bool inquiry_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const * cb_data) {
    msc_csw_t const* csw = cb_data->csw;

    if (csw->status != 0) {
        // printf("Inquiry failed\r\n");
        return false;
    }
    if (dev_addr != mounted_dev) {
        return false;                         // gone again before we got here
    }
    // Mounting the volume touches FatFs and the disk: defer it to msc_app_task()
    pending_mount = dev_addr;
    return true;
}

//------------- IMPLEMENTATION -------------//
void tuh_msc_mount_cb(uint8_t dev_addr)
{
    // printf("A MassStorage device is mounted\r\n");
    if (mounted_dev) {
        // Only handle a single drive
        // printf("Only a single USB drive is supported\n");
        return;
    }
    mounted_dev = dev_addr;  // may not actually be mounted, but does indicate the drive is inserted
    uint8_t const lun = 0;
    tuh_msc_inquiry(dev_addr, lun, &scsi_resp.inquiry, inquiry_complete_cb, 0);
}

void tuh_msc_umount_cb(uint8_t dev_addr)
{
    /* Only tear down the filesystem for the drive we actually mounted.
     * A second MSC device (e.g. a USB hub with two drives) disconnecting
     * must not unmount the still-active drive. */
    if (dev_addr != mounted_dev)
        return;

    // printf("A MassStorage device is unmounted\r\n");
    mounted_dev = 0;
    pending_mount = 0;
    pending_unmount = true;
    // Fail any transfer that is waiting on this device right now
    _disk_error = true;
    _disk_busy = false;
}

/* Core 1 loop: apply recorded mount/unmount events outside of any FatFs call */
void msc_app_task(void)
{
    if (pending_unmount) {
        pending_unmount = false;
        if (fs_mounted) {
            fs_mounted = false;
#ifdef PGDFS
            // Invalidate the file server's open handles before the volume goes away
            dfs_on_drive_unmounted();
#endif
            f_unmount("");
#ifdef CDROM
            cdman_unload_image(&cdrom);
#endif
        }
    }

    uint8_t dev = pending_mount;
    if (dev && dev == mounted_dev) {
        pending_mount = 0;
        if (f_mount(&fatfs, "", 1) != FR_OK) {
            ERR_PUTS("mount failed");
            return;
        }
        fs_mounted = true;

        // get the drive serial so we can detect if it is reinserted
        uint32_t serial = 0;
        bool have_serial = (FR_OK == f_getlabel("", NULL, &serial));
#ifdef CDROM
        if (have_serial) {
            cdman_set_serial(&cdrom, serial);
        }
#else
        (void)have_serial;
#endif
#ifdef PGDFS
        // The volume is mounted: let the file server refresh its drive info
        dfs_on_drive_mounted();
#endif
    }
}

//--------------------------------------------------------------------+
// DiskIO
//--------------------------------------------------------------------+

static inline void count_sat(uint32_t *c)
{
    if (*c != 0xFFFFFFFFu) (*c)++;
}

const msc_stats_t *msc_app_get_stats(void)
{
    return &stats;
}

#ifdef PGDFS
/* The mounted volume for PGDFS diagnostics; fs_type is 0 while unmounted. */
FATFS *dfs_platform_fatfs(void)
{
    return &fatfs;
}
#endif

static void wait_for_disk_io(uint8_t dev)
{
    /* Timeout — prevents a hung or disconnected USB drive from locking the
     * firmware forever.  2 s is far longer than any legitimate sector read
     * takes over USB Full Speed; writes get 10 s because a flash drive may
     * NAK for a long time while it programs and erases (the DOS driver's own
     * deadline is 30 s per request). */
    uint32_t deadline = time_us_32() + (_disk_is_write ? 10000000u : 2000000u);
    while (_disk_busy) {
        tuh_task();
        if (mounted_dev != dev) {
            /* the drive went away (or was replaced) under this transfer */
            _disk_busy = false;
            _disk_error = true;
            _disk_cause = MSC_IO_GONE;
            count_sat(&stats.device_gone);
            return;
        }
        if ((int32_t)(time_us_32() - deadline) >= 0) {
            DBG_PRINTF("disk_io: timeout waiting for USB transfer\n");
            _disk_busy = false;
            _disk_error = true;
            _disk_cause = MSC_IO_TIMEOUT;
            count_sat(&stats.timeouts);
            return;
        }
    }
}

static bool disk_io_complete(uint8_t dev_addr, tuh_msc_complete_data_t const * cb_data)
{
    (void) dev_addr;
    /* A completion for a transfer wait_for_disk_io() already gave up on
     * (timeout or device change) must not be charged to whatever runs now. */
    if (cb_data->user_arg != _xfer_gen) {
        count_sat(&stats.stale);
        return true;
    }
    /* Propagate SCSI command status — non-zero CSW status means the drive
     * reported an error (e.g. medium error, illegal request). */
    uint8_t status = cb_data->csw->status;
    _disk_error = (status != 0);
    if (_disk_error) {
        DBG_PRINTF("disk_io: SCSI error status %u\n", status);
        _disk_cause = MSC_IO_CSW;
        stats.last_csw_status = status;
        stats.last_csw_residue = cb_data->csw->data_residue;
        count_sat(_disk_is_write ? &stats.write_csw_err : &stats.read_csw_err);
    }
    _disk_busy = false;
    return true;
}

DSTATUS disk_status (
	BYTE pdrv		/* Physical drive nmuber to identify the drive */
)
{
  (void) pdrv;
  uint8_t dev = mounted_dev;
  return (dev && tuh_msc_mounted(dev)) ? 0 : STA_NODISK;
}

DSTATUS disk_initialize (
	BYTE pdrv				/* Physical drive nmuber to identify the drive */
)
{
  (void) pdrv;
	return 0; // nothing to do
}

DRESULT disk_read (
	BYTE pdrv,		/* Physical drive nmuber to identify the drive */
	BYTE *buff,		/* Data buffer to store read data */
	LBA_t sector,	/* Start sector in LBA */
	UINT count		/* Number of sectors to read */
)
{
    (void)pdrv;
    uint8_t const lun = 0;
    uint8_t dev = mounted_dev;
    DRESULT res;

    count_sat(&stats.reads);
    if (!dev) {
        stats.last_read_res = RES_NOTRDY;
        stats.last_read_cause = MSC_IO_NODEV;
        return RES_NOTRDY;
    }

    _disk_busy = true;
    _disk_error = false;
    _disk_cause = MSC_IO_OK;
    _disk_is_write = false;
    if (!tuh_msc_read10(dev, lun, buff, sector, (uint16_t) count, disk_io_complete, ++_xfer_gen)) {
        _disk_busy = false;
        _disk_cause = MSC_IO_REFUSED;
        count_sat(&stats.read_refused);
        res = RES_ERROR;
    } else {
        wait_for_disk_io(dev);
        res = _disk_error ? RES_ERROR : RES_OK;
    }
    stats.last_read_res = (uint8_t) res;
    stats.last_read_cause = _disk_cause;
    return res;
}

#if FF_FS_READONLY == 0

DRESULT disk_write (
	BYTE pdrv,			/* Physical drive nmuber to identify the drive */
	const BYTE *buff,	/* Data to be written */
	LBA_t sector,		/* Start sector in LBA */
	UINT count			/* Number of sectors to write */
)
{
    (void)pdrv;
    uint8_t const lun = 0;
    uint8_t dev = mounted_dev;
    uint32_t t0 = time_us_32();
    DRESULT res;

    count_sat(&stats.writes);
    stats.last_write_lba = (uint32_t) sector;
    stats.last_write_count = (uint16_t) count;
    if (!dev) {
        stats.last_write_res = RES_NOTRDY;
        stats.last_write_cause = MSC_IO_NODEV;
        stats.last_write_us = 0;
        return RES_NOTRDY;
    }

    _disk_busy = true;
    _disk_error = false;
    _disk_cause = MSC_IO_OK;
    _disk_is_write = true;
    if (!tuh_msc_write10(dev, lun, buff, sector, (uint16_t) count, disk_io_complete, ++_xfer_gen)) {
        _disk_busy = false;
        _disk_cause = MSC_IO_REFUSED;
        count_sat(&stats.write_refused);
        res = RES_ERROR;
    } else {
        wait_for_disk_io(dev);
        res = _disk_error ? RES_ERROR : RES_OK;
    }
    stats.last_write_us = time_us_32() - t0;
    stats.last_write_res = (uint8_t) res;
    stats.last_write_cause = _disk_cause;
    return res;
}

#endif

#if FF_FS_READONLY == 0 && FF_FS_NORTC == 0
/* FatFs needs a clock for timestamps in read-write builds. The PGDFS server
 * provides the real one (DOS time from the driver); this weak fallback keeps
 * builds without PGDFS linking and gives them a fixed 2026-01-01 stamp. */
__attribute__((weak)) DWORD get_fattime(void)
{
    return ((DWORD)(2026 - 1980) << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16);
}
#endif

DRESULT disk_ioctl (
	BYTE pdrv,		/* Physical drive nmuber (0..) */
	BYTE cmd,		/* Control code */
	void *buff		/* Control code */
)
{
    (void)pdrv;
    uint8_t const lun = 0;
    uint8_t dev = mounted_dev;
    if (!dev) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:
        // nothing to do since we do blocking
        return RES_OK;

    case GET_SECTOR_COUNT:
        *((DWORD*) buff) = (WORD) tuh_msc_get_block_count(dev, lun);
        return RES_OK;

    case GET_SECTOR_SIZE:
        *((WORD*) buff) = (WORD) tuh_msc_get_block_size(dev, lun);
        return RES_OK;

    case GET_BLOCK_SIZE:
        *((DWORD*) buff) = 1;    // erase block size in units of sector size
        return RES_OK;

    default:
        return RES_PARERR;
    }

    return RES_OK;
}
