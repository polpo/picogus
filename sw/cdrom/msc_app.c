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
extern cdrom_t cdrom[CDROM_NUM];

//------------- Elm Chan FatFS -------------//
static FATFS fatfs[CFG_TUH_DEVICE_MAX]; // for simplicity only support 1 LUN per device
static volatile bool _disk_busy[CFG_TUH_DEVICE_MAX];

// define the buffer to be place in USB/DMA memory with correct alignment/cache line size
CFG_TUH_MEM_SECTION static struct {
  TUH_EPBUF_TYPE_DEF(scsi_inquiry_resp_t, inquiry);
} scsi_resp;


bool msc_app_init(void)
{
  for(size_t i=0; i<CFG_TUH_DEVICE_MAX; i++) {
    _disk_busy[i] = false;
  }

  // disable stdout buffered for echoing typing command
  #ifndef __ICCARM__ // TODO IAR doesn't support stream control ?
  setbuf(stdout, NULL);
  #endif

  return true;
}

void msc_app_task(void)
{
}

static bool inquiry_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const * cb_data) {
  msc_cbw_t const* cbw = cb_data->cbw;
  msc_csw_t const* csw = cb_data->csw;

  if (csw->status != 0)
  {
    printf("Inquiry failed\r\n");
    return false;
  }

  // Print out Vendor ID, Product ID and Rev
  printf("%.8s %.16s rev %.4s\r\n", scsi_resp.inquiry.vendor_id, scsi_resp.inquiry.product_id, scsi_resp.inquiry.product_rev);

  // Get capacity of device
  uint32_t const block_count = tuh_msc_get_block_count(dev_addr, cbw->lun);
  uint32_t const block_size = tuh_msc_get_block_size(dev_addr, cbw->lun);

  printf("Disk Size: %" PRIu32 " MB\r\n", block_count / ((1024*1024)/block_size));
  // printf("Block Count = %lu, Block Size: %lu\r\n", block_count, block_size);

  // For simplicity: we only mount 1 LUN per device
  uint8_t const drive_num = dev_addr-1;
  char drive_path[3] = "0:";
  drive_path[0] += drive_num;

  if ( f_mount(&fatfs[drive_num], drive_path, 1) != FR_OK )
  {
    puts("mount failed");
    return false;
  }

  // change to newly mounted drive
  if (f_chdir(drive_path) != FR_OK) {
    puts("chdir failed");
    return false;
  }

  // print the drive label
  uint32_t serial;
  if ( FR_OK != f_getlabel(drive_path, NULL, &serial) ) {
      return false;
  }
  cdman_set_serial(serial);
  /* cdman_load_image_index(&cdrom[0], 1); */

  return true;
}

//------------- IMPLEMENTATION -------------//
void tuh_msc_mount_cb(uint8_t dev_addr)
{
  printf("A MassStorage device is mounted\r\n");

  uint8_t const lun = 0;
  tuh_msc_inquiry(dev_addr, lun, &scsi_resp.inquiry, inquiry_complete_cb, 0);
}

void tuh_msc_umount_cb(uint8_t dev_addr)
{
  printf("A MassStorage device is unmounted\r\n");

  uint8_t const drive_num = dev_addr-1;
  char drive_path[3] = "0:";
  drive_path[0] += drive_num;

  f_unmount(drive_path);

  cdman_unload_image(&cdrom[0]);

//  if ( phy_disk == f_get_current_drive() )
//  { // active drive is unplugged --> change to other drive
//    for(uint8_t i=0; i<CFG_TUH_DEVICE_MAX; i++)
//    {
//      if ( disk_is_ready(i) )
//      {
//        f_chdrive(i);
//        cli_init(); // refractor, rename
//      }
//    }
//  }
}

//--------------------------------------------------------------------+
// DiskIO
//--------------------------------------------------------------------+

static void wait_for_disk_io(BYTE pdrv)
{
  while(_disk_busy[pdrv])
  {
    tuh_task();
  }
}

static bool disk_io_complete(uint8_t dev_addr, tuh_msc_complete_data_t const * cb_data)
{
  (void) dev_addr; (void) cb_data;
  _disk_busy[dev_addr-1] = false;
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
	uint8_t const dev_addr = pdrv + 1;
	uint8_t const lun = 0;

	_disk_busy[pdrv] = true;
	tuh_msc_read10(dev_addr, lun, buff, sector, (uint16_t) count, disk_io_complete, 0);
	wait_for_disk_io(pdrv);

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
	uint8_t const dev_addr = pdrv + 1;
	uint8_t const lun = 0;

	_disk_busy[pdrv] = true;
	tuh_msc_write10(dev_addr, lun, buff, sector, (uint16_t) count, disk_io_complete, 0);
	wait_for_disk_io(pdrv);

	return RES_OK;
}

#endif

DRESULT disk_ioctl (
	BYTE pdrv,		/* Physical drive nmuber (0..) */
	BYTE cmd,		/* Control code */
	void *buff		/* Buffer to send/receive control data */
)
{
  uint8_t const dev_addr = pdrv + 1;
  uint8_t const lun = 0;
  switch ( cmd )
  {
    case CTRL_SYNC:
      // nothing to do since we do blocking
      return RES_OK;

    case GET_SECTOR_COUNT:
      *((DWORD*) buff) = (WORD) tuh_msc_get_block_count(dev_addr, lun);
      return RES_OK;

    case GET_SECTOR_SIZE:
      *((WORD*) buff) = (WORD) tuh_msc_get_block_size(dev_addr, lun);
      return RES_OK;

    case GET_BLOCK_SIZE:
      *((DWORD*) buff) = 1;    // erase block size in units of sector size
      return RES_OK;

    default:
      return RES_PARERR;
  }

	return RES_OK;
}
