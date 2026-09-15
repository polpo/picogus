/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2025 Ha Thach (tinyusb.org)
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
 * This file is part of the TinyUSB stack.
 */
#ifndef MSC_APP_H
#define MSC_APP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool msc_app_init(void);
/* Core 1 loop: applies USB drive mount/unmount events outside of any FatFs call */
void msc_app_task(void);

/*
 * Disk I/O telemetry. Updated inside disk_read()/disk_write() (core 1, under
 * a FatFs call) and read by the PGDFS DIAG request (core 1 as well), so no
 * locking is involved. Counters saturate rather than wrap. The host test
 * harness (sw/dfs/test/ramdisk_diskio.c) provides the same getter over its
 * RAM disk.
 */

/* Why the last transfer ended the way it did */
typedef enum {
    MSC_IO_OK      = 0,  /* completed, CSW status 0 */
    MSC_IO_NODEV   = 1,  /* no drive mounted: RES_NOTRDY without touching USB */
    MSC_IO_REFUSED = 2,  /* tuh_msc_read10()/write10() returned false (endpoint busy, not mounted) */
    MSC_IO_CSW     = 3,  /* the drive answered with CSW status != 0 */
    MSC_IO_TIMEOUT = 4,  /* no completion within the deadline (2 s reads, 10 s writes) */
    MSC_IO_GONE    = 5   /* the drive disappeared during the transfer */
} msc_io_cause_t;

typedef struct {
    uint32_t reads;             /* disk_read() calls */
    uint32_t writes;            /* disk_write() calls */
    uint32_t read_refused;      /* tuh_msc_read10() returned false */
    uint32_t write_refused;     /* tuh_msc_write10() returned false */
    uint32_t read_csw_err;      /* READ(10) completions with CSW status != 0 */
    uint32_t write_csw_err;     /* WRITE(10) completions with CSW status != 0 */
    uint32_t timeouts;          /* transfers abandoned without a completion (2 s reads, 10 s writes) */
    uint32_t device_gone;       /* transfers abandoned because the drive vanished */
    uint32_t last_write_us;     /* elapsed microseconds of the last disk_write(), refusal included */
    uint32_t last_write_lba;    /* first sector of the last disk_write() */
    uint32_t last_csw_residue;  /* dCSWDataResidue of the last completion with status != 0 */
    uint32_t stale;             /* completions of an already abandoned transfer, ignored */
    uint16_t last_write_count;  /* sectors in the last disk_write() */
    uint8_t  last_csw_status;   /* bCSWStatus of the last completion with status != 0 (0: none yet) */
    uint8_t  last_read_res;     /* DRESULT of the last disk_read() */
    uint8_t  last_write_res;    /* DRESULT of the last disk_write() */
    uint8_t  last_read_cause;   /* msc_io_cause_t of the last disk_read() */
    uint8_t  last_write_cause;  /* msc_io_cause_t of the last disk_write() */
} msc_stats_t;

/* Stable storage, never NULL. Plain counters: no printf, no USB access. */
const msc_stats_t *msc_app_get_stats(void);

#ifdef __cplusplus
}
#endif


#endif
