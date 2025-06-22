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

#include <ctype.h>
#include "tusb.h"
/* #include "bsp/board_api.h" */

#include "ff.h"
#include "diskio.h"

#include "msc_app.h"

#include "cdrom_image_manager.h"

//------------- Elm Chan FatFS -------------//
static FATFS fatfs; // for simplicity only support 1 device
static volatile bool _disk_busy;
static uint8_t mounted_dev;

// define the buffer to be place in USB/DMA memory with correct alignment/cache line size
CFG_TUH_MEM_SECTION static struct {
  TUH_EPBUF_TYPE_DEF(scsi_inquiry_resp_t, inquiry);
} scsi_resp;


bool msc_app_init(void)
{
    _disk_busy = false;
    return true;
}

static bool inquiry_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const * cb_data) {
    msc_cbw_t const* cbw = cb_data->cbw;
    msc_csw_t const* csw = cb_data->csw;

    if (csw->status != 0) {
        // printf("Inquiry failed\r\n");
        return false;
    }

    // Print out Vendor ID, Product ID and Rev
    // printf("%.8s %.16s rev %.4s\r\n", scsi_resp.inquiry.vendor_id, scsi_resp.inquiry.product_id, scsi_resp.inquiry.product_rev);

    // Get capacity of device
    uint32_t const block_count = tuh_msc_get_block_count(dev_addr, cbw->lun);
    uint32_t const block_size = tuh_msc_get_block_size(dev_addr, cbw->lun);

    // printf("Disk Size: %" PRIu32 " MB\r\n", block_count / ((1024*1024)/block_size));
    // printf("Block Count = %lu, Block Size: %lu\r\n", block_count, block_size);

    // For simplicity: we only mount 1 device
    if (f_mount(&fatfs, "", 1) != FR_OK) {
        puts("mount failed");
        return false;
    }

    // get the drive serial so we can detect if it is reinserted
    uint32_t serial;
    if (FR_OK != f_getlabel("", NULL, &serial)) {
        return false;
    }
    cdman_set_serial(&cdrom, serial);

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
    (void)dev_addr;
    // printf("A MassStorage device is unmounted\r\n");
    mounted_dev = 0;

    f_unmount("");

    cdman_unload_image(&cdrom);
}

//--------------------------------------------------------------------+
// DiskIO
//--------------------------------------------------------------------+

static void wait_for_disk_io(void)
{
    while (_disk_busy) {
        tuh_task();
    }
}

static bool disk_io_complete(uint8_t dev_addr, tuh_msc_complete_data_t const * cb_data)
{
  (void) dev_addr; (void) cb_data;
  _disk_busy = false;
  return true;
}

DSTATUS disk_status (
	BYTE pdrv		/* Physical drive nmuber to identify the drive */
)
{
  uint8_t dev_addr = pdrv + 1;
  return tuh_msc_mounted(dev_addr) ? 0 : STA_NODISK;
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

    _disk_busy = true;
    tuh_msc_read10(mounted_dev, lun, buff, sector, (uint16_t) count, disk_io_complete, 0);
    wait_for_disk_io();

    return RES_OK;
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

    _disk_busy = true;
    tuh_msc_write10(mounted_dev, lun, buff, sector, (uint16_t) count, disk_io_complete, 0);
    wait_for_disk_io();

    return RES_OK;
}

#endif

DRESULT disk_ioctl (
	BYTE pdrv,		/* Physical drive nmuber (0..) */
	BYTE cmd,		/* Control code */
	void *buff		/* Buffer to send/receive control data */
)
{
    (void)pdrv;
    uint8_t const lun = 0;
    switch (cmd) {
    case CTRL_SYNC:
        // nothing to do since we do blocking
        return RES_OK;

    case GET_SECTOR_COUNT:
        *((DWORD*) buff) = (WORD) tuh_msc_get_block_count(mounted_dev, lun);
        return RES_OK;

    case GET_SECTOR_SIZE:
        *((WORD*) buff) = (WORD) tuh_msc_get_block_size(mounted_dev, lun);
        return RES_OK;

    case GET_BLOCK_SIZE:
        *((DWORD*) buff) = 1;    // erase block size in units of sector size
        return RES_OK;

    default:
        return RES_PARERR;
    }

    return RES_OK;
}
