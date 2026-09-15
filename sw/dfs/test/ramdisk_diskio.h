/* RAM disk behind FatFs diskio.h for the PGDFS host tests. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define RAMDISK_SECTOR_SIZE 512

bool     ramdisk_init(uint32_t sectors);   /* allocates and zeroes the image */
void     ramdisk_free(void);
uint32_t ramdisk_sectors(void);

/* Fault injection: while set, every disk_write() / disk_read() fails with
 * RES_ERROR the way a USB drive answering CSW status 1 would, and the
 * telemetry (msc_app_get_stats(), the host stand-in for usb_msc/msc_app.c)
 * records it like the firmware does. */
void     ramdisk_set_write_fault(bool on);
void     ramdisk_set_read_fault(bool on);
void     ramdisk_reset_stats(void);        /* zero the msc_stats_t counters */
