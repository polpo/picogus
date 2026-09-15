/*
 *  PGDFS file server: EtherDFS (EDF5) request processing on FatFs.
 *
 *  dfs_process() takes one request frame (4-byte header + EDF5 payload,
 *  see PROTOCOL.md), serves it through dfs_fs.c and writes the answer frame
 *  in place. Runs on core 1 only; every buffer it needs is static so the
 *  4 KB core 1 stack is left to FatFs.
 *
 *  Derived from ethersrv-linux, Copyright (C) 2017, 2018 Mateusz Viste,
 *  MIT License; long-file-name modifications by Eric Voirin (oerg866);
 *  FatFs adaptation in the PicoMEM project by Freddy Vetele.
 *  PicoGUS integration Copyright (C) 2026 PicoGUS contributors.
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
#include <string.h>
#include "dfs_server.h"
#include "dfs_fs.h"
#include "../usb_msc/msc_app.h"     /* msc_app_get_stats() for DFS_AL_DIAG */

/* INT 2Fh/11h subfunctions carried in the AL byte of the header */
enum {
    AL_RMDIR     = 0x01,
    AL_MKDIR     = 0x03,
    AL_CHDIR     = 0x05,
    AL_CLSFIL    = 0x06,
    AL_READFIL   = 0x08,
    AL_WRITEFIL  = 0x09,
    AL_LOCKFIL   = 0x0A,
    AL_UNLOCKFIL = 0x0B,
    AL_DISKSPACE = 0x0C,
    AL_SETATTR   = 0x0E,
    AL_GETATTR   = 0x0F,
    AL_RENAME    = 0x11,
    AL_DELETE    = 0x13,
    AL_OPEN      = 0x16,
    AL_CREATE    = 0x17,
    AL_FINDFIRST = 0x1B,
    AL_FINDNEXT  = 0x1C,
    AL_SKFMEND   = 0x21,
    AL_SETFTIME  = 0x24,
    AL_SPOPNFIL  = 0x2E
};

/* SPOPNFIL result codes (CX of INT 21h/AX=6C00h) */
#define SPOP_OPENED    1
#define SPOP_CREATED   2
#define SPOP_TRUNCATED 3

static char        info_str[48] = "";
static bool        drive_present;

/* scratch, static on purpose (core 1 stack) */
static char        path_a[DFS_PATH_MAX];
static char        path_b[DFS_PATH_MAX];
static char        fcb[11];
static dfs_finfo_t info;

/* ---- little-endian access, byte-wise: buf is not aligned ----------------- */

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

/* ---- path helpers ---------------------------------------------------------- */

/* copy a non-terminated payload string; false when it does not fit */
static bool copy_path(char *dst, const uint8_t *src, uint16_t len) {
    if (len >= DFS_PATH_MAX) return false;
    memcpy(dst, src, len);
    dst[len] = 0;
    return true;
}

static void strip_trailing_sep(char *p) {
    size_t n = strlen(p);
    while (n > 0 && dfs_is_sep(p[n - 1])) p[--n] = 0;
}

static bool has_wildcard(const uint8_t *s, uint16_t len) {
    while (len--) {
        if (*s == '?' || *s == '*') return true;
        s++;
    }
    return false;
}

/* "\DIR\FILE????.???" -> dir "\DIR" ("" for the root) and the FCB mask.
 * tmp receives the name part on the way. */
static bool split_dir_mask(const uint8_t *src, uint16_t len, char *dir, char *tmp, char *mask) {
    uint16_t i, dirlen = 0;
    for (i = 0; i < len; i++) {
        if (dfs_is_sep((char)src[i])) dirlen = (uint16_t)(i + 1);
    }
    if (!copy_path(dir, src, dirlen)) return false;
    if (!copy_path(tmp, src + dirlen, (uint16_t)(len - dirlen))) return false;
    strip_trailing_sep(dir);
    dfs_name2fcb(mask, tmp);
    return true;
}

/* DOS access mode (INT 21h/3Dh AL bits 0-2) -> FatFs flags */
static uint8_t access_to_fa(uint16_t mode) {
    switch (mode & 3) {
    case 0:  return FA_READ;
    case 1:  return FA_WRITE;
    default: return FA_READ | FA_WRITE;
    }
}

/* A fff...(11) tt dd ssss: common head of the OPEN and FIND answers, 20 bytes */
static void put_finfo(uint8_t *p, const dfs_finfo_t *fi) {
    p[0] = fi->attr;
    memcpy(p + 1, fi->fcb, 11);
    put16(p + 12, fi->time);
    put16(p + 14, fi->date);
    put32(p + 16, fi->size);
}

/* ---- OPEN / CREATE / SPOPNFIL ---------------------------------------------- */

static uint16_t do_open(uint8_t al, const uint8_t *pl, uint16_t plen, uint8_t *answ, uint16_t *alen) {
    uint16_t stackw, action, omode, ax, id = 0, result = SPOP_OPENED;
    uint8_t resmode;

    if (plen < 7) return DFS_ERR_FUNC;
    stackw = get16(pl);         /* OPEN: open mode; CREATE/SPOPNFIL: attributes */
    action = get16(pl + 2);     /* SPOPNFIL action code */
    omode  = get16(pl + 4);     /* SPOPNFIL open mode */
    if (!copy_path(path_a, pl + 6, (uint16_t)(plen - 6))) return DFS_ERR_PATH;
    DFS_LOG("open al=%02X stack=%04X action=%04X mode=%04X '%s'\n", al, stackw, action, omode, path_a);

    switch (al) {
    case AL_OPEN:
        ax = dfs_fs_open(path_a, access_to_fa(stackw), 0, &id, &info);
        if (ax == DFS_ERR_FILE && dfs_fs_stat(path_a, &info) == DFS_ERR_OK && (info.attr & DFS_ATTR_DIR)) {
            ax = DFS_ERR_ACCESS;    /* opening a directory */
        }
        resmode = (uint8_t)stackw;
        break;
    case AL_CREATE:
        ax = dfs_fs_open(path_a, FA_READ | FA_WRITE | FA_CREATE_ALWAYS, (uint8_t)stackw, &id, &info);
        result = SPOP_CREATED;
        resmode = 2;                /* created files are read/write */
        break;
    default: /* AL_SPOPNFIL: RBIL INT 21h/AX=6C00h action code */
        ax = dfs_fs_stat(path_a, &info);
        if (ax == DFS_ERR_FILE) {
            if ((action & 0xF0) == 0x10) {          /* create if it does not exist */
                ax = dfs_fs_open(path_a, access_to_fa(omode) | FA_CREATE_NEW, (uint8_t)stackw, &id, &info);
                result = SPOP_CREATED;
            }                                       /* else: fail, file not found */
        } else if (ax == DFS_ERR_OK) {
            if (info.attr & (DFS_ATTR_DIR | DFS_ATTR_VOL)) {
                ax = DFS_ERR_ACCESS;
            } else {
                switch (action & 0x0F) {
                case 1:                             /* open if it exists */
                    ax = dfs_fs_open(path_a, access_to_fa(omode), 0, &id, &info);
                    break;
                case 2:                             /* replace/truncate if it exists */
                    ax = dfs_fs_open(path_a, access_to_fa(omode) | FA_CREATE_ALWAYS, (uint8_t)stackw, &id, &info);
                    result = SPOP_TRUNCATED;
                    break;
                default:                            /* fail if it exists */
                    ax = DFS_ERR_EXISTS;
                    break;
                }
            }
        }
        resmode = (uint8_t)(omode & 0x7F);          /* what PHANTOM.C does */
        break;
    }
    if (ax != DFS_ERR_OK) return ax;

    put_finfo(answ, &info);
    put16(answ + 20, id);
    put16(answ + 22, result);
    answ[24] = resmode;
    *alen = 25;
    return DFS_ERR_OK;
}

/* ---- FINDFIRST / FINDNEXT -------------------------------------------------- */

static uint16_t do_findfirst(const uint8_t *pl, uint16_t plen, uint8_t *answ, uint16_t *alen) {
    uint16_t ax, dir_id = 0xFFFF, pos = 0;
    uint8_t attr;

    if (plen < 2) return DFS_ERR_FUNC;
    attr = pl[0];
    if (!split_dir_mask(pl + 1, (uint16_t)(plen - 1), path_b, path_a, fcb)) return DFS_ERR_PATH;
    DFS_LOG("findfirst attr=%02X dir='%s' mask='%.11s'\n", attr, path_b, fcb);

    if (attr == DFS_ATTR_VOL) {
        /* volume label only: the label lives in the root and there is one */
        if (!dfs_path_is_root(path_b) || !dfs_fs_label(info.fcb) || !dfs_fcb_match(fcb, info.fcb)) {
            return DFS_ERR_NOMORE;
        }
        info.attr = DFS_ATTR_VOL;
        info.size = 0;
        info.time = 0;
        info.date = 0;
        ax = DFS_ERR_OK;
    } else {
        ax = dfs_fs_find_first(path_b, attr, fcb, &info, &dir_id, &pos);
    }
    if (ax != DFS_ERR_OK) return ax;

    put_finfo(answ, &info);
    put16(answ + 20, dir_id);
    put16(answ + 22, pos);
    *alen = 24;
    return DFS_ERR_OK;
}

static uint16_t do_findnext(const uint8_t *pl, uint16_t plen, uint8_t *answ, uint16_t *alen) {
    uint16_t ax, dir_id, pos, newpos = 0;
    uint8_t attr;

    if (plen < 16) return DFS_ERR_FUNC;
    dir_id = get16(pl);
    pos = get16(pl + 2);
    attr = pl[4];
    memcpy(fcb, pl + 5, 11);            /* the answer overwrites the request */
    DFS_LOG("findnext id=%u pos=%u attr=%02X mask='%.11s'\n", dir_id, pos, attr, fcb);
    if (attr == DFS_ATTR_VOL) return DFS_ERR_NOMORE;    /* one label at most */

    ax = dfs_fs_find_next(dir_id, pos, attr, fcb, &info, &newpos);
    if (ax != DFS_ERR_OK) return ax;

    put_finfo(answ, &info);
    put16(answ + 20, dir_id);
    put16(answ + 22, newpos);
    *alen = 24;
    return DFS_ERR_OK;
}

/* ---- DIAG ------------------------------------------------------------------ */

static uint16_t sat16(uint32_t v) {
    return (v > 0xFFFF) ? 0xFFFF : (uint16_t)v;
}

/* Builds the DFS_DIAG_LEN-byte telemetry record (layout in dfs_server.h and
 * PROTOCOL.md) into answ, clipped to maxpl; returns the length written. No disk
 * access: everything comes from counters and the FATFS object as they are. */
static uint16_t do_diag(uint8_t *answ, uint16_t maxpl) {
    static uint8_t rec[DFS_DIAG_LEN];
    const msc_stats_t *ms = msc_app_get_stats();
    uint32_t free_clst, n_fatent;
    uint16_t csize, n;
    uint8_t fs_type, fr, call, hard_fr, hard_call;

    dfs_fs_volume_stats(&free_clst, &n_fatent, &csize, &fs_type);
    dfs_fs_last_error(&fr, &call, &hard_fr, &hard_call);

    memset(rec, 0, sizeof(rec));
    rec[DFS_DIAG_OFF_VERSION] = DFS_DIAG_VERSION;
    rec[DFS_DIAG_OFF_FLAGS] = drive_present ? 1 : 0;
    rec[DFS_DIAG_OFF_FSTYPE] = fs_type;
    put32(rec + DFS_DIAG_OFF_FREECLST, free_clst);
    put32(rec + DFS_DIAG_OFF_NFATENT, n_fatent);
    put16(rec + DFS_DIAG_OFF_CSIZE, csize);
    rec[DFS_DIAG_OFF_LASTFR] = fr;
    rec[DFS_DIAG_OFF_LASTCALL] = call;
    rec[DFS_DIAG_OFF_HARDFR] = hard_fr;
    rec[DFS_DIAG_OFF_HARDCALL] = hard_call;
    rec[DFS_DIAG_OFF_RDRES] = ms->last_read_res;
    rec[DFS_DIAG_OFF_WRRES] = ms->last_write_res;
    rec[DFS_DIAG_OFF_RDCAUSE] = ms->last_read_cause;
    rec[DFS_DIAG_OFF_WRCAUSE] = ms->last_write_cause;
    rec[DFS_DIAG_OFF_CSWSTAT] = ms->last_csw_status;
    put32(rec + DFS_DIAG_OFF_READS, ms->reads);
    put32(rec + DFS_DIAG_OFF_WRITES, ms->writes);
    put16(rec + DFS_DIAG_OFF_RDREFUSED, sat16(ms->read_refused));
    put16(rec + DFS_DIAG_OFF_WRREFUSED, sat16(ms->write_refused));
    put16(rec + DFS_DIAG_OFF_RDCSWERR, sat16(ms->read_csw_err));
    put16(rec + DFS_DIAG_OFF_WRCSWERR, sat16(ms->write_csw_err));
    put16(rec + DFS_DIAG_OFF_TIMEOUTS, sat16(ms->timeouts));
    put16(rec + DFS_DIAG_OFF_GONE, sat16(ms->device_gone));
    put32(rec + DFS_DIAG_OFF_WRUS, ms->last_write_us);
    put32(rec + DFS_DIAG_OFF_WRLBA, ms->last_write_lba);
    put16(rec + DFS_DIAG_OFF_WRCOUNT, ms->last_write_count);
    put16(rec + DFS_DIAG_OFF_STALE, sat16(ms->stale));
    put32(rec + DFS_DIAG_OFF_CSWRESID, ms->last_csw_residue);

    n = (maxpl < DFS_DIAG_LEN) ? maxpl : DFS_DIAG_LEN;
    memcpy(answ, rec, n);
    return n;
}

/* ---- dispatcher ------------------------------------------------------------ */

uint16_t dfs_process(uint8_t *buf, uint16_t req_len, uint16_t buf_size) {
    uint8_t *pl = buf + DFS_HDR_LEN;
    uint16_t plen, ax = DFS_ERR_OK, alen = 0, total;
    uint16_t maxpl = (buf_size > DFS_HDR_LEN) ? (uint16_t)(buf_size - DFS_HDR_LEN) : 0;
    uint8_t al = 0;

    if (req_len < DFS_HDR_LEN || req_len > buf_size) {
        ax = DFS_ERR_FUNC;
        goto out;
    }
    plen = (uint16_t)(req_len - DFS_HDR_LEN);
    al = buf[3];

    if (al == DFS_AL_ECHO) {            /* payload is already in place */
        alen = plen;
        goto out;
    }
    if (al == DFS_AL_DIAG) {            /* telemetry, with or without a drive */
        alen = do_diag(pl, maxpl);
        goto out;
    }
    if ((buf[2] & 0x1F) != 0) {
        ax = DFS_ERR_DRIVE;
        goto out;
    }
    if (maxpl < 32) {                   /* the fixed answers need up to 25 bytes */
        ax = DFS_ERR_FUNC;
        goto out;
    }
    if (!drive_present) {
        ax = DFS_ERR_NOTREADY;
        goto out;
    }

    switch (al) {
    case AL_RMDIR:
    case AL_MKDIR:
    case AL_CHDIR:
        if (!copy_path(path_a, pl, plen)) { ax = DFS_ERR_PATH; break; }
        strip_trailing_sep(path_a);
        DFS_LOG("dir op %02X '%s'\n", al, path_a);
        if (al == AL_MKDIR)      ax = dfs_fs_mkdir(path_a);
        else if (al == AL_RMDIR) ax = dfs_fs_rmdir(path_a);
        else                     ax = dfs_fs_chdir(path_a);
        break;

    case AL_CLSFIL:
        if (plen < 2) { ax = DFS_ERR_FUNC; break; }
        ax = dfs_fs_close(get16(pl));
        break;

    case AL_READFIL: {
        uint32_t offset;
        uint16_t id, len, got = 0;
        if (plen < 8) { ax = DFS_ERR_FUNC; break; }
        offset = get32(pl);
        id = get16(pl + 4);
        len = get16(pl + 6);
        if (len > maxpl) len = maxpl;
        ax = dfs_fs_read(id, offset, pl, len, &got);
        if (ax == DFS_ERR_OK) alen = got;
        break;
    }

    case AL_WRITEFIL: {
        uint32_t offset;
        uint16_t id, done = 0;
        if (plen < 6) { ax = DFS_ERR_FUNC; break; }
        offset = get32(pl);
        id = get16(pl + 4);
        ax = dfs_fs_write(id, offset, pl + 6, (uint16_t)(plen - 6), &done);
        if (ax == DFS_ERR_OK) {
            put16(pl, done);
            alen = 2;
        }
        break;
    }

    case AL_LOCKFIL:
    case AL_UNLOCKFIL:
        break;                          /* single client: always succeeds */

    case AL_DISKSPACE: {
        uint16_t total_units = 0, free_units = 0;
        ax = dfs_fs_diskspace(&total_units, &free_units);
        if (ax == DFS_ERR_OK) {
            put16(pl, total_units);     /* BX: total clusters */
            put16(pl + 2, 32768);       /* CX: bytes per sector */
            put16(pl + 4, free_units);  /* DX: free clusters */
            ax = 1;                     /* AX: sectors per cluster, MS-DOS wants 1 */
            alen = 6;
        }
        break;
    }

    case AL_SETATTR:
        if (plen < 2) { ax = DFS_ERR_FUNC; break; }
        if (!copy_path(path_a, pl + 1, (uint16_t)(plen - 1))) { ax = DFS_ERR_PATH; break; }
        ax = dfs_fs_chmod(path_a, pl[0]);
        break;

    case AL_GETATTR:
        if (plen < 1) { ax = DFS_ERR_FUNC; break; }
        if (!copy_path(path_a, pl, plen)) { ax = DFS_ERR_PATH; break; }
        ax = dfs_fs_stat(path_a, &info);
        if (ax == DFS_ERR_OK) {
            put16(pl, info.time);
            put16(pl + 2, info.date);
            put32(pl + 4, info.size);
            pl[8] = info.attr;
            alen = 9;
        }
        break;

    case AL_RENAME: {
        uint16_t srclen;
        if (plen < 3) { ax = DFS_ERR_FUNC; break; }
        srclen = pl[0];
        if ((uint16_t)(srclen + 2) > plen) { ax = DFS_ERR_FUNC; break; }
        if (!copy_path(path_a, pl + 1, srclen) ||
            !copy_path(path_b, pl + 1 + srclen, (uint16_t)(plen - 1 - srclen))) {
            ax = DFS_ERR_PATH;
            break;
        }
        DFS_LOG("rename '%s' -> '%s'\n", path_a, path_b);
        ax = dfs_fs_rename(path_a, path_b);
        break;
    }

    case AL_DELETE:
        if (plen < 1) { ax = DFS_ERR_FUNC; break; }
        if (has_wildcard(pl, plen)) {
            if (!split_dir_mask(pl, plen, path_b, path_a, fcb)) { ax = DFS_ERR_PATH; break; }
            ax = dfs_fs_delete_wild(path_b, fcb);
        } else {
            if (!copy_path(path_a, pl, plen)) { ax = DFS_ERR_PATH; break; }
            ax = dfs_fs_unlink(path_a);
        }
        break;

    case AL_OPEN:
    case AL_CREATE:
    case AL_SPOPNFIL:
        ax = do_open(al, pl, plen, pl, &alen);
        break;

    case AL_FINDFIRST:
        ax = do_findfirst(pl, plen, pl, &alen);
        break;

    case AL_FINDNEXT:
        ax = do_findnext(pl, plen, pl, &alen);
        break;

    case AL_SKFMEND: {
        uint32_t size = 0, back;
        int32_t offs;
        if (plen < 6) { ax = DFS_ERR_FUNC; break; }
        offs = (int32_t)get32(pl);
        if (offs > 0) offs = 0;         /* only "from end backwards" makes sense */
        back = (uint32_t)(-offs);
        ax = dfs_fs_size(get16(pl + 4), &size);
        if (ax == DFS_ERR_OK) {
            put32(pl, (back > size) ? 0 : size - back);
            alen = 4;
        }
        break;
    }

    case AL_SETFTIME:
        if (plen < 6) { ax = DFS_ERR_FUNC; break; }
        ax = dfs_fs_utime(get16(pl + 4), get16(pl), get16(pl + 2));
        break;

    case DFS_AL_LONGNAME: {         /* PGDFS: long name of an 8.3 path, no terminator */
        const char *name = "";
        size_t n;
        if (plen < 1) { ax = DFS_ERR_FUNC; break; }
        if (has_wildcard(pl, plen)) { ax = DFS_ERR_PATH; break; }    /* as f_stat(): invalid name */
        if (!split_dir_mask(pl, plen, path_b, path_a, fcb)) { ax = DFS_ERR_PATH; break; }
        DFS_LOG("longname dir='%s' name='%s'\n", path_b, path_a);
        if (path_a[0] == 0) {           /* the root has no name; "\DIR\" is not a name */
            ax = dfs_path_is_root(path_b) ? DFS_ERR_OK : DFS_ERR_PATH;
        } else if (path_a[0] == '.') {  /* '.' and '..' are their own long names */
            name = path_a;
            ax = (strcmp(path_a, ".") == 0 || strcmp(path_a, "..") == 0) ? DFS_ERR_OK : DFS_ERR_FILE;
        } else {
            ax = dfs_fs_longname(path_b, fcb, &name);
        }
        if (ax == DFS_ERR_OK) {
            n = strlen(name);
            if (n > maxpl) n = maxpl;   /* FF_LFN_BUF (255) fits any real frame */
            memcpy(pl, name, n);        /* name is dfs_fs scratch, not the frame */
            alen = (uint16_t)n;
        }
        break;
    }

    default:
        ax = DFS_ERR_FUNC;
        break;
    }

out:
    if (alen > maxpl) alen = maxpl;
    total = (uint16_t)(DFS_HDR_LEN + alen);
    if (total > buf_size) total = buf_size;     /* buf_size < 4: nothing sane to do */
    if (buf_size >= DFS_HDR_LEN) {
        put16(buf, total);
        put16(buf + 2, ax);
    }
    DFS_LOG("al=%02X -> ax=%04X len=%u\n", al, ax, total);
    return total;
}

/* ---- server API ------------------------------------------------------------ */

void dfs_server_init(void) {
    dfs_fs_reset();
    drive_present = false;
    info_str[0] = 0;
}

void dfs_server_drive_mounted(void) {
    dfs_fs_invalidate();                /* nothing from an earlier drive may survive */
    if (!dfs_fs_volume_info(info_str, sizeof(info_str))) info_str[0] = 0;
    drive_present = true;
}

void dfs_server_drive_unmounted(void) {
    dfs_fs_invalidate();
    drive_present = false;
    info_str[0] = 0;
}

bool dfs_server_drive_present(void) {
    return drive_present;
}

void dfs_server_set_dos_time(uint16_t dos_time, uint16_t dos_date) {
    dfs_fs_set_dos_time(dos_time, dos_date);
}

const char *dfs_server_info_string(void) {
    return info_str;
}
