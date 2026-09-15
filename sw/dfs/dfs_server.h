/*
 *  PGDFS file server: EtherDFS (EDF5) request processing on FatFs.
 *
 *  Derived from ethersrv-linux, Copyright (C) 2017, 2018 Mateusz Viste,
 *  MIT License, and its FatFs adaptation in the PicoMEM project.
 *  PicoGUS integration Copyright (C) 2026 PicoGUS contributors, GPL v2+.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../../common/picogus.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pure request processor, host-testable. buf holds a request frame
 * (DFS_HDR_LEN header + payload, see PROTOCOL.md); on return it holds the
 * answer frame. Returns the total answer length (>= DFS_HDR_LEN). The
 * answer never exceeds buf_size. Runs on core 1 only.
 */
uint16_t dfs_process(uint8_t *buf, uint16_t req_len, uint16_t buf_size);

/*
 * Core notes: dfs_server_init() runs on core 0 at boot. dfs_server_drive_present()
 * and dfs_server_info_string() are called from core 0 inside an ISA bus cycle:
 * O(1), no FatFs, no loops. The info string may be replaced by core 1 while
 * core 0 iterates over it (a torn read is harmless). dfs_server_set_dos_time()
 * is called from dfs_tasks() on core 1, so the server's clock state is core 1 only.
 * dfs_server_drive_unmounted() can be invoked from inside dfs_process() (a USB
 * unplug surfaces through tuh_task() while FatFs waits on disk I/O), so it must
 * only mark state and never touch the frame being processed.
 * dfs_fs.c owns the strong DWORD get_fattime(void); usb_msc/msc_app.c carries a
 * weak fallback for builds without PGDFS.
 */
void dfs_server_init(void);                 /* reset tables, no FatFs access */
void dfs_server_drive_mounted(void);        /* volume "" is mounted: refresh info string */
void dfs_server_drive_unmounted(void);      /* invalidate every open file/dir handle */
bool dfs_server_drive_present(void);

/* DOS packed time/date (FAT format) as sent by the driver. Used by get_fattime(). */
void dfs_server_set_dos_time(uint16_t dos_time, uint16_t dos_date);

/* "LABEL|FAT32|<size MB>|<serial hex>" or "" when nothing is mounted. Stable storage. */
const char *dfs_server_info_string(void);

/* Provided by the platform (dfs_transport.c on the Pico, the test harness on host).
 * The server also needs dfs_platform_fatfs() (dfs_fs.h; msc_app.c on the Pico) and
 * msc_app_get_stats() (usb_msc/msc_app.h; ramdisk_diskio.c on host) for DFS_AL_DIAG. */
uint32_t dfs_platform_millis(void);

/* PGDFS-specific subfunctions on top of the EDF5 set (AL values 0x00-0x2E). */
#define DFS_AL_ECHO     0xF0 /* answer payload = request payload, AX = 0 */
#define DFS_AL_LONGNAME 0xF1 /* request: path as DOS sees it ("\\DIR\\FILE~1.TXT", 8.3);
                              * answer: the entry's long file name in the FatFs code page
                              * (the same as the short name when it has none), AX = 0,
                              * or AX = 2/3 when the path does not exist. Lets tools show
                              * long names although the redirector interface is 8.3-only. */
#define DFS_AL_DIAG     0xF2 /* request: empty; answer: the DFS_DIAG_LEN-byte record below,
                              * AX = 0 always, drive or no drive (zeros for what is absent).
                              * Disk I/O and FatFs telemetry for the field: which layer a
                              * failing write died in. Mirrored in pgusdfs/dfsdiag.c. */

/*
 * DFS_AL_DIAG answer record, little-endian, DFS_DIAG_LEN bytes (PROTOCOL.md
 * has the same table). u8/u16/u32 = unsigned of that width. The u16 event
 * counters saturate at FFFFh; the u32 ones count since power-on.
 */
#define DFS_DIAG_VERSION       2
#define DFS_DIAG_LEN           60
#define DFS_DIAG_OFF_VERSION    0 /* u8  DFS_DIAG_VERSION                                  */
#define DFS_DIAG_OFF_FLAGS      1 /* u8  bit 0: the server considers a drive present        */
#define DFS_DIAG_OFF_FSTYPE     2 /* u8  0 none, 1 FAT12, 2 FAT16, 3 FAT32, 4 exFAT (FatFs) */
#define DFS_DIAG_OFF_RSVD3      3 /* u8  0                                                  */
#define DFS_DIAG_OFF_FREECLST   4 /* u32 free clusters as FatFs believes; > n_fatent-2 = unknown (FAT not scanned yet; FatFs starts at 0xFFFFFFFF) */
#define DFS_DIAG_OFF_NFATENT    8 /* u32 FAT entries = clusters + 2                         */
#define DFS_DIAG_OFF_CSIZE     12 /* u16 sectors per cluster                                */
#define DFS_DIAG_OFF_LASTFR    14 /* u8  last non-OK FRESULT of any FatFs call (0 = none)   */
#define DFS_DIAG_OFF_LASTCALL  15 /* u8  DFS_CALL_* id of that call (dfs_fs.h)              */
#define DFS_DIAG_OFF_HARDFR    16 /* u8  last FRESULT other than a lookup miss (0 = none)   */
#define DFS_DIAG_OFF_HARDCALL  17 /* u8  DFS_CALL_* id of that call                         */
#define DFS_DIAG_OFF_RDRES     18 /* u8  DRESULT of the last disk_read()                    */
#define DFS_DIAG_OFF_WRRES     19 /* u8  DRESULT of the last disk_write()                   */
#define DFS_DIAG_OFF_RDCAUSE   20 /* u8  msc_io_cause_t of the last disk_read()             */
#define DFS_DIAG_OFF_WRCAUSE   21 /* u8  msc_io_cause_t of the last disk_write()            */
#define DFS_DIAG_OFF_CSWSTAT   22 /* u8  bCSWStatus of the last completion with status != 0 */
#define DFS_DIAG_OFF_RSVD23    23 /* u8  0                                                  */
#define DFS_DIAG_OFF_READS     24 /* u32 disk_read() calls                                  */
#define DFS_DIAG_OFF_WRITES    28 /* u32 disk_write() calls                                 */
#define DFS_DIAG_OFF_RDREFUSED 32 /* u16 tuh_msc_read10() returned false                    */
#define DFS_DIAG_OFF_WRREFUSED 34 /* u16 tuh_msc_write10() returned false                   */
#define DFS_DIAG_OFF_RDCSWERR  36 /* u16 READ(10) completions with CSW status != 0          */
#define DFS_DIAG_OFF_WRCSWERR  38 /* u16 WRITE(10) completions with CSW status != 0         */
#define DFS_DIAG_OFF_TIMEOUTS  40 /* u16 transfers abandoned at the deadline (2 s rd, 10 s wr) */
#define DFS_DIAG_OFF_GONE      42 /* u16 transfers abandoned because the drive vanished     */
#define DFS_DIAG_OFF_WRUS      44 /* u32 microseconds the last disk_write() took            */
#define DFS_DIAG_OFF_WRLBA     48 /* u32 first sector of the last disk_write()              */
#define DFS_DIAG_OFF_WRCOUNT   52 /* u16 sectors in the last disk_write()                   */
#define DFS_DIAG_OFF_STALE     54 /* u16 completions of already abandoned transfers, ignored */
#define DFS_DIAG_OFF_CSWRESID  56 /* u32 dCSWDataResidue of the last completion with status != 0 */

#ifdef __cplusplus
}
#endif
