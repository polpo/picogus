/*
 *  PGDFS transport: ISA control-port side (core 0) <-> file server (core 1).
 *
 *  Copyright (C) 2026  PicoGUS contributors
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/*
 * See PROTOCOL.md for the wire protocol and dfs.h for the threading model.
 *
 * Every dfs_ctl_* / dfs_data_* function runs on core 0 from inside an ISA
 * bus cycle while IOCHRDY is held. They are O(1): no loops, no FatFs, no
 * USB, no printf, no flash. The firmware is built with PICO_COPY_TO_RAM, so
 * this whole file (like picogus.cpp) executes from RAM; nothing here needs
 * __not_in_flash_func.
 *
 * Buffer ownership follows the status byte:
 *   RECEIVING, READY : core 0 (bytes in / bytes out on the data port window)
 *   BUSY             : core 1 (dfs_process())
 * The hand-over in each direction is "write everything, barrier, write
 * status"; the reader does "read status, barrier, read everything".
 *
 * Abort while BUSY: core 0 cannot stop core 1, so it bumps a generation
 * counter, sets a sticky "aborted" flag and leaves the status at BUSY: the
 * buffer still belongs to core 1 and the driver's next request must wait
 * for it (the driver polls BUSY). When dfs_process() returns, core 1
 * compares the generation it snapshotted, drops the result on a mismatch
 * and moves the status to ABORTED itself. The sticky flag also covers a
 * READY written by core 1 in the tiny window between its generation check
 * and its status write: the status reads ABORTED and the data port stays
 * dead until the next CMD_DFSREQ.
 *
 * The DOS clock (CMD_DFSTIME) is collected on core 0 but handed to the
 * server from dfs_tasks() on core 1, where the FatFs clock state lives.
 */

#include <string.h>
#include "dfs.h"
#include "dfs_server.h"

#ifdef DFS_HOST_TEST
/* Host build for sw/dfs/test: no Pico SDK. */
#define DFS_DMB() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#else
#include "pico/time.h"
#include "hardware/sync.h"
#define DFS_DMB() __dmb()
#endif

/* ---- shared state -------------------------------------------------------- */

static uint8_t dfs_buf[DFS_BUF_SIZE];       /* request frame in, answer frame out */
static volatile uint8_t  dfs_state;         /* dfs_status_t (IDLE/RECEIVING/BUSY/READY/ABORTED) */
static volatile uint16_t dfs_frame_len;     /* request length (core 0 -> 1), answer length (core 1 -> 0) */
static volatile uint32_t dfs_gen;           /* transaction generation, see header comment */
static volatile bool     dfs_aborted;       /* sticky until the next CMD_DFSREQ */

/* core 0 only */
static uint16_t dfs_wr;                     /* next byte to write while RECEIVING */
static uint16_t dfs_rd;                     /* next byte to read while READY */
static bool     dfs_overflow;               /* a byte arrived with the buffer full */
static uint8_t  dfs_info_idx;               /* CMD_DFSINFO string cursor */
static uint8_t  dfs_time_idx;               /* CMD_DFSTIME byte counter, 0..3 */
static uint8_t  dfs_time_bytes[4];          /* time lo, time hi, date lo, date hi */
static volatile bool dfs_time_pending;      /* a complete time/date pair waits for core 1 */
static uint16_t dfs_time_val, dfs_date_val; /* the pair (core 0 -> core 1) */

/* ---- core 0: boot ---------------------------------------------------------- */

void dfs_init(void) {
    dfs_state = DFS_STATUS_IDLE;
    dfs_frame_len = 0;
    dfs_gen = 0;
    dfs_aborted = false;
    dfs_wr = 0;
    dfs_rd = 0;
    dfs_overflow = false;
    dfs_info_idx = 0;
    dfs_time_idx = 0;
    memset(dfs_time_bytes, 0, sizeof(dfs_time_bytes));
    dfs_time_pending = false;
    dfs_time_val = dfs_date_val = 0;
    dfs_server_init();
}

/* ---- core 0: transaction registers ---------------------------------------- */

/* CMD_DFSREQ selected: start a new transaction unless core 1 owns the buffer. */
void dfs_ctl_select_req(void) {
    if (dfs_state == DFS_STATUS_BUSY) {
        return;                             /* driver sees BUSY and waits/aborts */
    }
    dfs_wr = 0;
    dfs_rd = 0;
    dfs_overflow = false;
    dfs_aborted = false;
    dfs_gen++;
    dfs_state = DFS_STATUS_RECEIVING;
}

/* CMD_DFSRESP selected: rewind the answer read pointer (only meaningful in READY). */
void dfs_ctl_select_resp(void) {
    dfs_rd = 0;
}

/* CMD_DFSEXEC written: validate the frame and hand the buffer to core 1. */
void dfs_ctl_exec(void) {
    if (dfs_state == DFS_STATUS_BUSY) {
        return;                             /* core 1 owns the buffer; BUSY stands */
    }
    if (dfs_state != DFS_STATUS_RECEIVING || dfs_overflow || dfs_wr < DFS_HDR_LEN) {
        dfs_state = DFS_STATUS_ABORTED;
        return;
    }
    uint16_t declared = (uint16_t)(dfs_buf[0] | ((uint16_t)dfs_buf[1] << 8));
    if (declared != dfs_wr || declared > DFS_BUF_SIZE) {
        dfs_state = DFS_STATUS_ABORTED;
        return;
    }
    dfs_frame_len = dfs_wr;
    DFS_DMB();                              /* frame + length visible before BUSY */
    dfs_state = DFS_STATUS_BUSY;
}

/* CMD_DFSSTAT written: abort. Sticky ABORTED until the next CMD_DFSREQ. */
void dfs_ctl_abort(void) {
    dfs_aborted = true;
    if (dfs_state == DFS_STATUS_BUSY) {
        /* Core 1 owns the buffer until dfs_process() returns; it sees the
         * generation change, drops its result and moves to ABORTED itself.
         * BUSY stays in place so the next CMD_DFSREQ cannot open the buffer
         * while core 1 is still writing into it. */
        dfs_gen++;
        return;
    }
    dfs_state = DFS_STATUS_ABORTED;
}

/* CMD_DFSSTAT read. */
uint8_t dfs_ctl_status(void) {
    uint8_t s = dfs_state;
    if (s == DFS_STATUS_BUSY) {
        return s;                           /* BUSY wins, even after an abort: the driver waits */
    }
    if (dfs_aborted) {
        s = DFS_STATUS_ABORTED;
    }
    if ((s == DFS_STATUS_IDLE || s == DFS_STATUS_ABORTED) && !dfs_server_drive_present()) {
        return DFS_STATUS_NODRIVE;
    }
    return s;
}

/* Data window write (either port): append to the request frame while RECEIVING. */
void dfs_data_write(uint8_t value) {
    if (dfs_state != DFS_STATUS_RECEIVING) {
        return;
    }
    if (dfs_wr >= DFS_BUF_SIZE) {
        dfs_overflow = true;
        return;
    }
    dfs_buf[dfs_wr++] = value;
}

/* Data window read (either port): stream the answer frame while READY, 0xFF otherwise. */
uint8_t dfs_data_read(void) {
    if (dfs_state != DFS_STATUS_READY || dfs_aborted) {
        return 0xFF;
    }
    DFS_DMB();                              /* READY seen: answer + length are visible */
    if (dfs_rd >= dfs_frame_len) {
        return 0xFF;
    }
    return dfs_buf[dfs_rd++];
}

/* ---- core 0: info string --------------------------------------------------- */

void dfs_ctl_info_rewind(void) {
    dfs_info_idx = 0;
}

uint8_t dfs_ctl_info_read(void) {
    const char *s = dfs_server_info_string();
    uint8_t c = (uint8_t)s[dfs_info_idx];
    if (dfs_info_idx == 0xFF) {
        c = 0;                              /* defensive: never walk past a 255-byte string */
    }
    if (c == 0) {
        dfs_info_idx = 0;                   /* terminator rewinds */
    } else {
        dfs_info_idx++;
    }
    return c;
}

/* ---- core 0: DOS time ------------------------------------------------------ */

void dfs_ctl_time_rewind(void) {
    dfs_time_idx = 0;
}

void dfs_ctl_time_write(uint8_t v) {
    dfs_time_bytes[dfs_time_idx++] = v;
    if (dfs_time_idx >= 4) {
        dfs_time_idx = 0;
        dfs_time_val = (uint16_t)(dfs_time_bytes[0] | ((uint16_t)dfs_time_bytes[1] << 8));
        dfs_date_val = (uint16_t)(dfs_time_bytes[2] | ((uint16_t)dfs_time_bytes[3] << 8));
        DFS_DMB();
        dfs_time_pending = true;            /* applied by core 1 in dfs_tasks() */
    }
}

/* ---- core 1: serving ------------------------------------------------------- */

void dfs_tasks(void) {
    if (dfs_time_pending) {
        DFS_DMB();
        dfs_server_set_dos_time(dfs_time_val, dfs_date_val);
        dfs_time_pending = false;
    }
    if (dfs_state != DFS_STATUS_BUSY) {
        return;
    }
    uint32_t gen = dfs_gen;
    DFS_DMB();
    /* An abort between the two status reads bumped the generation and set
     * ABORTED; re-checking the status after the snapshot catches it. */
    if (dfs_state != DFS_STATUS_BUSY) {
        return;
    }
    uint16_t req_len = dfs_frame_len;
    uint16_t ans_len = dfs_process(dfs_buf, req_len, DFS_BUF_SIZE);
    if (dfs_gen != gen) {
        DFS_DMB();                          /* aborted meanwhile: drop the result, buffer back to core 0 */
        dfs_state = DFS_STATUS_ABORTED;
        return;
    }
    dfs_frame_len = ans_len;
    DFS_DMB();                              /* answer + length visible before READY */
    dfs_state = DFS_STATUS_READY;
}

void dfs_on_drive_mounted(void) {
    dfs_server_drive_mounted();
}

void dfs_on_drive_unmounted(void) {
    dfs_server_drive_unmounted();
}

/* ---- platform -------------------------------------------------------------- */

#ifdef DFS_HOST_TEST
/* The host test harness normally provides this; a weak default keeps a
 * harness that does not care about time linking. */
__attribute__((weak)) uint32_t dfs_platform_millis(void) {
    return 0;
}
#else
uint32_t dfs_platform_millis(void) {
    return to_ms_since_boot(get_absolute_time());
}
#endif
