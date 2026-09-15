/*
 *  PGDFS: PicoGUS DOS file system redirector service.
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
#pragma once

/*
 * Transport layer between the ISA control port (core 0) and the file server
 * running in the core 1 loop. See PROTOCOL.md for the wire protocol.
 *
 * Threading model:
 *   - Everything under "core 0" is called from handle_iow()/handle_ior() and
 *     must be O(1), non-blocking, and never touch FatFs or USB.
 *   - dfs_tasks() and the drive hooks run on core 1 only, next to
 *     cdrom_tasks()/tuh_task(). FatFs is single-threaded and lives on core 1.
 *   - The single frame buffer is owned by core 0 while the status is
 *     RECEIVING or READY, and by core 1 while it is BUSY.
 *
 * Data port: two consecutive I/O ports at an even base (settings.DFS.basePort,
 * default DFS_DEFAULT_DATA_PORT, 0 = disabled). Both feed the same byte
 * stream, so a 16-bit rep insw/outsw from DOS, which the motherboard splits
 * into two 8-bit cycles for this 8-bit card, delivers stream byte n through
 * the base port and n+1 through base+1. picogus.cpp decodes the window and
 * calls dfs_data_write()/dfs_data_read() once per 8-bit cycle whichever of
 * the two ports it hits.
 */

#include <stdint.h>
#include <stdbool.h>
#include "../../common/picogus.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DFS_MAX_PAYLOAD
#define DFS_MAX_PAYLOAD DFS_DEFAULT_MAX_PAYLOAD
#endif
#define DFS_BUF_SIZE (DFS_HDR_LEN + DFS_MAX_PAYLOAD)

/* ---- core 0: ISA control port side ------------------------------------- */
void    dfs_init(void);                 /* once at boot, before core 1 starts */
void    dfs_ctl_select_req(void);       /* CMD_DFSREQ selected                 */
void    dfs_ctl_select_resp(void);      /* CMD_DFSRESP selected                */
void    dfs_ctl_exec(void);             /* CMD_DFSEXEC written                 */
void    dfs_ctl_abort(void);            /* CMD_DFSSTAT written                 */
uint8_t dfs_ctl_status(void);           /* CMD_DFSSTAT read (dfs_status_t)     */
void    dfs_data_write(uint8_t value);  /* data window write, either port      */
uint8_t dfs_data_read(void);            /* data window read, either port       */
void    dfs_ctl_info_rewind(void);      /* CMD_DFSINFO selected                */
uint8_t dfs_ctl_info_read(void);        /* CMD_DFSINFO read, 0 ends + rewinds  */
void    dfs_ctl_time_rewind(void);      /* CMD_DFSTIME selected                */
void    dfs_ctl_time_write(uint8_t v);  /* CMD_DFSTIME write, 4 bytes          */
static inline uint16_t dfs_ctl_max_payload(void) { return DFS_MAX_PAYLOAD; }

/* ---- core 1: USB / FatFs side ------------------------------------------- */
void dfs_tasks(void);                   /* call every core 1 loop iteration    */
void dfs_on_drive_mounted(void);        /* from msc glue after f_mount() ok    */
void dfs_on_drive_unmounted(void);      /* from msc glue around f_unmount()    */

#ifdef __cplusplus
}
#endif
