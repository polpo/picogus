/*
 * PGDFS file server host tests.
 *
 * Formats a RAM disk (FAT32, no partition table), mounts it, and drives
 * dfs_process() with hand-built EDF5 frames. Prints a pass/fail summary and
 * exits non-zero on any failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ff.h"
#include "diskio.h"      /* DRESULT values reported by DIAG */
#include "dfs.h"          /* DFS_BUF_SIZE, DFS_MAX_PAYLOAD */
#include "dfs_server.h"
#include "dfs_fs.h"        /* DFS_MAX_DIRS, DFS_CALL_* */
#include "ramdisk_diskio.h"
#include "../../usb_msc/msc_app.h"   /* msc_stats_t, MSC_IO_* (the RAM disk keeps them) */

/* ---- platform stubs -------------------------------------------------------- */

static uint32_t test_clock_ms = 5000000;
static FATFS fatfs;

uint32_t dfs_platform_millis(void) {
    return test_clock_ms;
}

FATFS *dfs_platform_fatfs(void) {
    return &fatfs;
}

/* ---- check infrastructure -------------------------------------------------- */

static int n_checks, n_fails;

#define CHECK(cond, ...) do { \
    n_checks++; \
    if (!(cond)) { \
        n_fails++; \
        printf("  FAIL line %d: ", __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

#define SECTION(name) printf("== %s\n", name)

/* ---- EDF5 frame helpers ---------------------------------------------------- */

enum {
    AL_RMDIR = 0x01, AL_MKDIR = 0x03, AL_CHDIR = 0x05, AL_CLSFIL = 0x06,
    AL_READFIL = 0x08, AL_WRITEFIL = 0x09, AL_LOCKFIL = 0x0A, AL_UNLOCKFIL = 0x0B,
    AL_DISKSPACE = 0x0C, AL_SETATTR = 0x0E, AL_GETATTR = 0x0F, AL_RENAME = 0x11,
    AL_DELETE = 0x13, AL_OPEN = 0x16, AL_CREATE = 0x17, AL_FINDFIRST = 0x1B,
    AL_FINDNEXT = 0x1C, AL_SKFMEND = 0x21, AL_SETFTIME = 0x24, AL_SPOPNFIL = 0x2E
};

static uint8_t  buf[DFS_BUF_SIZE];      /* the shared request/answer buffer */
static uint8_t  req[DFS_BUF_SIZE];      /* payload under construction */
static uint8_t *const pl = buf + DFS_HDR_LEN;
static uint16_t last_len;               /* answer payload length */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | ((uint32_t)rd16(p + 2) << 16); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }

static uint16_t run_frame(uint16_t req_len, uint16_t buf_size) {
    uint16_t alen = dfs_process(buf, req_len, buf_size);
    uint16_t hdr = rd16(buf);
    CHECK(alen == hdr, "answer length: header says %u, dfs_process returned %u", hdr, alen);
    CHECK(alen >= DFS_HDR_LEN && alen <= buf_size, "answer length %u out of range", alen);
    last_len = (uint16_t)(alen - DFS_HDR_LEN);
    return rd16(buf + 2);
}

static uint16_t run_drv(uint8_t drive, uint8_t al, const void *payload, uint16_t plen) {
    uint16_t len = (uint16_t)(DFS_HDR_LEN + plen);
    wr16(buf, len);
    buf[2] = drive;
    buf[3] = al;
    if (plen) memcpy(pl, payload, plen);
    return run_frame(len, sizeof(buf));
}

static uint16_t run(uint8_t al, const void *payload, uint16_t plen) {
    return run_drv(0, al, payload, plen);
}

static uint16_t run_str(uint8_t al, const char *s) {
    return run(al, s, (uint16_t)strlen(s));
}

static uint16_t op_open(uint8_t al, uint16_t stackw, uint16_t action, uint16_t mode,
                        const char *path, uint16_t *id, uint16_t *result) {
    uint16_t ax;
    size_t n = strlen(path);
    wr16(req, stackw);
    wr16(req + 2, action);
    wr16(req + 4, mode);
    memcpy(req + 6, path, n);
    ax = run(al, req, (uint16_t)(6 + n));
    if (ax == 0) {
        CHECK(last_len == 25, "open answer length %u != 25", last_len);
        if (id) *id = rd16(pl + 20);
        if (result) *result = rd16(pl + 22);
    }
    return ax;
}

static uint16_t op_close(uint16_t id) {
    wr16(req, id);
    return run(AL_CLSFIL, req, 2);
}

static uint16_t op_read(uint16_t id, uint32_t offset, uint16_t len) {
    wr32(req, offset);
    wr16(req + 4, id);
    wr16(req + 6, len);
    return run(AL_READFIL, req, 8);
}

static uint16_t op_write(uint16_t id, uint32_t offset, const void *data, uint16_t len, uint16_t *written) {
    uint16_t ax;
    wr32(req, offset);
    wr16(req + 4, id);
    if (len) memcpy(req + 6, data, len);
    ax = run(AL_WRITEFIL, req, (uint16_t)(6 + len));
    if (ax == 0) {
        CHECK(last_len == 2, "write answer length %u != 2", last_len);
        if (written) *written = rd16(pl);
    }
    return ax;
}

static uint16_t op_getattr(const char *path, uint16_t *time, uint16_t *date, uint32_t *size, uint8_t *attr) {
    uint16_t ax = run_str(AL_GETATTR, path);
    if (ax == 0) {
        CHECK(last_len == 9, "getattr answer length %u != 9", last_len);
        if (time) *time = rd16(pl);
        if (date) *date = rd16(pl + 2);
        if (size) *size = rd32(pl + 4);
        if (attr) *attr = pl[8];
    }
    return ax;
}

static uint16_t op_setattr(const char *path, uint8_t attr) {
    size_t n = strlen(path);
    req[0] = attr;
    memcpy(req + 1, path, n);
    return run(AL_SETATTR, req, (uint16_t)(1 + n));
}

static uint16_t op_rename(const char *from, const char *to) {
    size_t a = strlen(from), b = strlen(to);
    req[0] = (uint8_t)a;
    memcpy(req + 1, from, a);
    memcpy(req + 1 + a, to, b);
    return run(AL_RENAME, req, (uint16_t)(1 + a + b));
}

static uint16_t op_seekend(uint16_t id, int32_t offset, uint32_t *pos) {
    uint16_t ax;
    wr32(req, (uint32_t)offset);
    wr16(req + 4, id);
    ax = run(AL_SKFMEND, req, 6);
    if (ax == 0) {
        CHECK(last_len == 4, "seekfromend answer length %u != 4", last_len);
        if (pos) *pos = rd32(pl);
    }
    return ax;
}

static uint16_t op_utime(uint16_t id, uint16_t time, uint16_t date) {
    wr16(req, time);
    wr16(req + 2, date);
    wr16(req + 4, id);
    return run(AL_SETFTIME, req, 6);
}

/* create + close a file with attributes */
static void make_file(const char *path, uint8_t attr) {
    uint16_t id = 0;
    uint16_t ax = op_open(AL_CREATE, attr, 0, 0, path, &id, NULL);
    CHECK(ax == 0, "create '%s' -> AX %04X", path, ax);
    if (ax == 0) CHECK(op_close(id) == 0, "close '%s'", path);
}

/* ---- search helpers -------------------------------------------------------- */

typedef struct {
    uint8_t  attr;
    char     name[12];      /* 11-char FCB name, NUL terminated */
    uint16_t time, date;
    uint32_t size;
    uint16_t dir_id, pos;
    char     mask[11];      /* FCB template for FINDNEXT */
    uint8_t  sattr;         /* search attribute */
} search_t;

/* test-side copy of the FCB conversion for the FINDNEXT template */
static void to_fcb(char *fcb, const char *name) {
    int i = 0, j = 0;
    memset(fcb, ' ', 11);
    while (i < 8 && name[j] && name[j] != '.') {
        if (name[j] == '*') { while (i < 8) fcb[i++] = '?'; break; }
        fcb[i++] = name[j++];
    }
    while (name[j] && name[j] != '.') j++;
    if (name[j] != '.') return;
    j++;
    i = 8;
    while (i < 11 && name[j] && name[j] != '.') {
        if (name[j] == '*') { while (i < 11) fcb[i++] = '?'; break; }
        fcb[i++] = name[j++];
    }
}

static void unpack_search(search_t *s) {
    s->attr = pl[0];
    memcpy(s->name, pl + 1, 11);
    s->name[11] = 0;
    s->time = rd16(pl + 12);
    s->date = rd16(pl + 14);
    s->size = rd32(pl + 16);
    s->dir_id = rd16(pl + 20);
    s->pos = rd16(pl + 22);
}

static uint16_t op_findfirst(uint8_t attr, const char *pathmask, search_t *s) {
    uint16_t ax;
    size_t n = strlen(pathmask);
    const char *last = strrchr(pathmask, '\\');
    req[0] = attr;
    memcpy(req + 1, pathmask, n);
    memset(s, 0, sizeof(*s));
    s->sattr = attr;
    to_fcb(s->mask, last ? last + 1 : pathmask);
    ax = run(AL_FINDFIRST, req, (uint16_t)(1 + n));
    if (ax == 0) {
        CHECK(last_len == 24, "findfirst answer length %u != 24", last_len);
        unpack_search(s);
    }
    return ax;
}

static uint16_t op_findnext(search_t *s) {
    uint16_t ax;
    wr16(req, s->dir_id);
    wr16(req + 2, s->pos);
    req[4] = s->sattr;
    memcpy(req + 5, s->mask, 11);
    ax = run(AL_FINDNEXT, req, 16);
    if (ax == 0) {
        CHECK(last_len == 24, "findnext answer length %u != 24", last_len);
        unpack_search(s);
    }
    return ax;
}

/* run a whole search, collecting up to max names; returns the count */
static int collect(uint8_t attr, const char *pathmask, char names[][12], int max) {
    search_t s;
    int n = 0;
    uint16_t ax = op_findfirst(attr, pathmask, &s);
    while (ax == 0 && n < max) {
        if (names) memcpy(names[n], s.name, 12);
        n++;
        ax = op_findnext(&s);
    }
    CHECK(ax == 0x12 || n == max, "search '%s' attr %02X ended with AX %04X after %d", pathmask, attr, ax, n);
    return n;
}

static int has_name(char names[][12], int n, const char *fcb) {
    for (int i = 0; i < n; i++) if (memcmp(names[i], fcb, 11) == 0) return 1;
    return 0;
}

/* ---- DIAG helpers ---------------------------------------------------------- */

typedef struct {
    uint8_t  version, flags, fs_type;
    uint32_t free_clst, n_fatent;
    uint16_t csize;
    uint8_t  last_fr, last_call, hard_fr, hard_call;
    uint8_t  rd_res, wr_res, rd_cause, wr_cause, csw_status;
    uint32_t reads, writes;
    uint16_t rd_refused, wr_refused, rd_csw_err, wr_csw_err, timeouts, gone;
    uint32_t wr_us, wr_lba;
    uint16_t wr_count, stale;
    uint32_t csw_residue;
} diag_t;

/* runs DFS_AL_DIAG and unpacks the record; AX and the length are checked here */
static void op_diag(diag_t *d) {
    uint16_t ax = run(DFS_AL_DIAG, NULL, 0);
    CHECK(ax == 0, "diag -> AX %04X", ax);
    CHECK(last_len == DFS_DIAG_LEN, "diag answer length %u != %u", last_len, DFS_DIAG_LEN);
    memset(d, 0, sizeof(*d));
    if (last_len < DFS_DIAG_LEN) return;
    d->version     = pl[DFS_DIAG_OFF_VERSION];
    d->flags       = pl[DFS_DIAG_OFF_FLAGS];
    d->fs_type     = pl[DFS_DIAG_OFF_FSTYPE];
    d->free_clst   = rd32(pl + DFS_DIAG_OFF_FREECLST);
    d->n_fatent    = rd32(pl + DFS_DIAG_OFF_NFATENT);
    d->csize       = rd16(pl + DFS_DIAG_OFF_CSIZE);
    d->last_fr     = pl[DFS_DIAG_OFF_LASTFR];
    d->last_call   = pl[DFS_DIAG_OFF_LASTCALL];
    d->hard_fr     = pl[DFS_DIAG_OFF_HARDFR];
    d->hard_call   = pl[DFS_DIAG_OFF_HARDCALL];
    d->rd_res      = pl[DFS_DIAG_OFF_RDRES];
    d->wr_res      = pl[DFS_DIAG_OFF_WRRES];
    d->rd_cause    = pl[DFS_DIAG_OFF_RDCAUSE];
    d->wr_cause    = pl[DFS_DIAG_OFF_WRCAUSE];
    d->csw_status  = pl[DFS_DIAG_OFF_CSWSTAT];
    d->reads       = rd32(pl + DFS_DIAG_OFF_READS);
    d->writes      = rd32(pl + DFS_DIAG_OFF_WRITES);
    d->rd_refused  = rd16(pl + DFS_DIAG_OFF_RDREFUSED);
    d->wr_refused  = rd16(pl + DFS_DIAG_OFF_WRREFUSED);
    d->stale       = rd16(pl + DFS_DIAG_OFF_STALE);
    d->rd_csw_err  = rd16(pl + DFS_DIAG_OFF_RDCSWERR);
    d->wr_csw_err  = rd16(pl + DFS_DIAG_OFF_WRCSWERR);
    d->timeouts    = rd16(pl + DFS_DIAG_OFF_TIMEOUTS);
    d->gone        = rd16(pl + DFS_DIAG_OFF_GONE);
    d->wr_us       = rd32(pl + DFS_DIAG_OFF_WRUS);
    d->wr_lba      = rd32(pl + DFS_DIAG_OFF_WRLBA);
    d->wr_count    = rd16(pl + DFS_DIAG_OFF_WRCOUNT);
    d->csw_residue = rd32(pl + DFS_DIAG_OFF_CSWRESID);
}

/* ---- the tests ------------------------------------------------------------- */

static char names[64][12];

static void test_setup(void) {
    static uint8_t work[FF_MAX_SS];
    MKFS_PARM opt = { FM_FAT32 | FM_SFD, 1, 0, 0, 512 };
    FRESULT fr;
    diag_t d;

    SECTION("setup");
    /* DIAG before anything exists: a valid record of zeros */
    dfs_server_init();
    op_diag(&d);
    CHECK(d.version == DFS_DIAG_VERSION, "diag version %u", d.version);
    {
        int zero = 1;
        for (int i = 1; i < DFS_DIAG_LEN; i++) if (pl[i] != 0) zero = 0;
        CHECK(zero, "diag record is all zero before the first mount");
    }
    /* the record is clipped to a small buffer and stays AX = 0 */
    wr16(buf, DFS_HDR_LEN); buf[2] = 0; buf[3] = DFS_AL_DIAG;
    CHECK(run_frame(DFS_HDR_LEN, DFS_HDR_LEN + 16) == 0 && last_len == 16 && pl[0] == DFS_DIAG_VERSION, "diag clipped to a 16-byte payload: len %u", last_len);

    CHECK(ramdisk_init(64u * 1024 * 1024 / RAMDISK_SECTOR_SIZE), "ramdisk alloc");
    fr = f_mkfs("", &opt, work, sizeof(work));
    CHECK(fr == FR_OK, "f_mkfs -> %d", fr);
    fr = f_mount(&fatfs, "", 1);
    CHECK(fr == FR_OK, "f_mount -> %d", fr);
    fr = f_setlabel("PGTEST");
    CHECK(fr == FR_OK, "f_setlabel -> %d", fr);

    dfs_server_init();
    CHECK(!dfs_server_drive_present(), "not present after init");
    CHECK(dfs_server_info_string()[0] == 0, "info empty after init");

    /* before the mount notification: ECHO works, everything else is 15h */
    CHECK(run(DFS_AL_ECHO, "abc", 3) == 0 && last_len == 3, "echo before mount");
    CHECK(run(AL_DISKSPACE, NULL, 0) == 0x15, "diskspace before mount -> 15h");

    dfs_server_drive_mounted();
    CHECK(dfs_server_drive_present(), "present after mount");
    printf("  info: '%s'\n", dfs_server_info_string());
    CHECK(strncmp(dfs_server_info_string(), "PGTEST|FAT32|", 13) == 0, "info string prefix '%s'", dfs_server_info_string());
    {
        const char *s = dfs_server_info_string();
        const char *p = strrchr(s, '|');
        int bars = 0;
        for (const char *q = s; *q; q++) if (*q == '|') bars++;
        CHECK(bars == 3, "info string has %d bars", bars);
        CHECK(p && strlen(p + 1) == 8, "serial is 8 hex digits");
        CHECK(atoi(strchr(s, '|') + 7) > 50 && atoi(strchr(s, '|') + 7) < 70, "size MB plausible");
    }
    /* DIAG after the mount: geometry known, free count not scanned yet
     * (FF_FS_NOFSINFO bit 0: the FSINFO count is not trusted, and the mount
     * notification must not trigger the FAT scan), counters from f_mkfs */
    op_diag(&d);
    CHECK(d.flags == 1, "diag flags %02X: drive present", d.flags);
    CHECK(d.fs_type == FS_FAT32, "diag fs type %u", d.fs_type);
    CHECK(d.csize == 1, "diag sectors per cluster %u", d.csize);
    CHECK(d.n_fatent > 100000 && d.n_fatent < 140000, "diag n_fatent %u", d.n_fatent);
    CHECK(d.free_clst > d.n_fatent - 2, "diag free clusters unknown before the first DISKSPACE: %08X", d.free_clst);
    CHECK(d.reads > 0 && d.writes > 0, "diag counters after mkfs: %u reads, %u writes", d.reads, d.writes);
    CHECK(d.wr_res == RES_OK && d.wr_cause == MSC_IO_OK, "diag last write ok: res %u cause %u", d.wr_res, d.wr_cause);
    CHECK(d.hard_fr == 0 && d.hard_call == 0, "diag no hard error yet: %u/%u", d.hard_fr, d.hard_call);
    CHECK(d.stale == 0, "diag stale completions %u", d.stale);
    CHECK(run(AL_DISKSPACE, NULL, 0) == 1, "diskspace scans the FAT");
    op_diag(&d);
    CHECK(d.free_clst <= d.n_fatent - 2, "diag free clusters known after DISKSPACE: %u of %u", d.free_clst, d.n_fatent - 2);
}

static void test_framing(void) {
    uint16_t ax;
    SECTION("framing, echo, errors");
    ax = run(DFS_AL_ECHO, "hello", 5);
    CHECK(ax == 0 && last_len == 5 && memcmp(pl, "hello", 5) == 0, "echo hello");
    ax = run(DFS_AL_ECHO, NULL, 0);
    CHECK(ax == 0 && last_len == 0, "echo empty");
    ax = run(0x7F, NULL, 0);
    CHECK(ax == 1, "unknown AL -> 1, got %04X", ax);
    ax = run(0x00, NULL, 0);
    CHECK(ax == 1, "AL 00 -> 1, got %04X", ax);
    ax = run_drv(1, AL_DISKSPACE, NULL, 0);
    CHECK(ax == 0x0F, "drive index 1 -> 0Fh, got %04X", ax);
    ax = run_drv(0xE0, AL_DISKSPACE, NULL, 0);
    CHECK(ax == 1 && last_len == 6, "flag bits ignored, got %04X", ax);
    /* short frame */
    wr16(buf, 3); buf[2] = 0; buf[3] = AL_DISKSPACE;
    ax = run_frame(3, sizeof(buf));
    CHECK(ax == 1, "short frame -> 1");
    /* a buffer too small for the fixed answers is refused before dispatch */
    wr16(buf, 4); buf[2] = 0; buf[3] = AL_DISKSPACE;
    ax = run_frame(4, 20);
    CHECK(ax == 1 && last_len == 0, "buffer under 32 payload bytes -> 1, got %04X", ax);
    wr16(buf, 9); buf[2] = 0; buf[3] = DFS_AL_ECHO; memcpy(pl, "12345", 5);
    ax = run_frame(9, 9);
    CHECK(ax == 0 && last_len == 5, "echo still works in a tiny buffer");
    ax = run(AL_LOCKFIL, "xxxxxx", 6);
    CHECK(ax == 0 && last_len == 0, "lock is a no-op success");
    ax = run(AL_UNLOCKFIL, NULL, 0);
    CHECK(ax == 0, "unlock is a no-op success");
}

static void test_diskspace(void) {
    uint16_t ax;
    SECTION("diskspace");
    ax = run(AL_DISKSPACE, NULL, 0);
    CHECK(ax == 1, "AX = sectors per cluster = 1, got %04X", ax);
    CHECK(last_len == 6, "answer length %u", last_len);
    CHECK(rd16(pl + 2) == 32768, "CX = 32768");
    CHECK(rd16(pl) > 1500 && rd16(pl) < 2100, "BX total 32K units = %u", rd16(pl));
    CHECK(rd16(pl + 4) > 0 && rd16(pl + 4) <= rd16(pl), "DX free = %u", rd16(pl + 4));
}

static void test_dirs(void) {
    uint16_t ax;
    SECTION("mkdir / chdir / rmdir");
    CHECK(run_str(AL_MKDIR, "\\SUB") == 0, "mkdir \\SUB");
    ax = run_str(AL_MKDIR, "\\SUB");
    CHECK(ax == 5, "mkdir existing -> 5, got %04X", ax);
    ax = run_str(AL_MKDIR, "\\NOPE\\X");
    CHECK(ax == 3, "mkdir under missing dir -> 3, got %04X", ax);
    CHECK(run_str(AL_CHDIR, "\\SUB") == 0, "chdir \\SUB");
    CHECK(run_str(AL_CHDIR, "\\sub") == 0, "chdir \\sub (case)");
    CHECK(run_str(AL_CHDIR, "\\") == 0, "chdir root");
    ax = run_str(AL_CHDIR, "\\NOPE");
    CHECK(ax == 3, "chdir missing -> 3, got %04X", ax);
    CHECK(run_str(AL_MKDIR, "\\SUB\\DEEP") == 0, "mkdir \\SUB\\DEEP");
    ax = run_str(AL_RMDIR, "\\SUB");
    CHECK(ax == 5, "rmdir non-empty -> 5, got %04X", ax);
    CHECK(run_str(AL_RMDIR, "\\SUB\\DEEP") == 0, "rmdir \\SUB\\DEEP");
    ax = run_str(AL_RMDIR, "\\SUB\\DEEP");
    CHECK(ax == 3, "rmdir missing -> 3, got %04X", ax);
    ax = run_str(AL_CHDIR, "\\SUB\\DEEP");
    CHECK(ax == 3, "chdir removed -> 3, got %04X", ax);
    ax = run_str(AL_RMDIR, "\\");
    CHECK(ax == 5, "rmdir root -> 5, got %04X", ax);
}

static void test_files(void) {
    uint16_t ax, id = 0, rr = 0, written = 0;
    uint32_t size = 0, pos = 0;
    uint8_t attr = 0;

    SECTION("create / write / read / truncate / close");
    ax = op_open(AL_CREATE, 0, 0, 0, "\\SUB\\TEST.TXT", &id, &rr);
    CHECK(ax == 0, "create -> %04X", ax);
    CHECK(id >= 1 && id <= 8, "file id %u", id);
    CHECK(rr == 2, "create result %u", rr);
    CHECK(pl[24] == 2, "create open mode byte %02X", pl[24]);
    CHECK(memcmp(pl + 1, "TEST    TXT", 11) == 0, "create FCB name '%.11s'", pl + 1);
    CHECK(rd32(pl + 16) == 0, "created size 0");
    CHECK(pl[0] == 0x20, "created attr %02X", pl[0]);

    ax = op_write(id, 0, "Hello, World!", 13, &written);
    CHECK(ax == 0 && written == 13, "write 13 at 0 -> %04X/%u", ax, written);
    CHECK(op_getattr("\\SUB\\TEST.TXT", NULL, NULL, &size, &attr) == 0 && size == 13, "size 13 after synced write, got %u", size);
    ax = op_write(id, 13, " Bye.", 5, &written);
    CHECK(ax == 0 && written == 5, "append");
    CHECK(op_seekend(id, 0, &pos) == 0 && pos == 18, "seekfromend 0 -> 18, got %u", pos);
    ax = op_write(id, 7, "PGDFS", 5, &written);
    CHECK(ax == 0 && written == 5, "overwrite in the middle");
    ax = op_read(id, 0, 100);
    CHECK(ax == 0 && last_len == 18 && memcmp(pl, "Hello, PGDFS! Bye.", 18) == 0, "read full: %04X len %u '%.*s'", ax, last_len, last_len, pl);
    ax = op_read(id, 15, 100);
    CHECK(ax == 0 && last_len == 3 && memcmp(pl, "ye.", 3) == 0, "read partial at EOF");
    ax = op_read(id, 18, 10);
    CHECK(ax == 0 && last_len == 0, "read at EOF -> 0 bytes");
    ax = op_read(id, 1000, 10);
    CHECK(ax == 0 && last_len == 0, "read past EOF -> 0 bytes");
    CHECK(op_seekend(id, 0, &pos) == 0 && pos == 18, "read past EOF did not grow the file");
    ax = op_read(id, 4, 4);
    CHECK(ax == 0 && last_len == 4 && memcmp(pl, "o, P", 4) == 0, "read 4 at 4");

    ax = op_write(id, 5, NULL, 0, &written);
    CHECK(ax == 0 && written == 0, "zero-length write (truncate)");
    CHECK(op_seekend(id, 0, &pos) == 0 && pos == 5, "size 5 after truncate, got %u", pos);
    ax = op_read(id, 0, 100);
    CHECK(ax == 0 && last_len == 5 && memcmp(pl, "Hello", 5) == 0, "content after truncate");
    CHECK(op_seekend(id, -2, &pos) == 0 && pos == 3, "seekfromend -2 -> 3");
    CHECK(op_seekend(id, -100, &pos) == 0 && pos == 0, "seekfromend -100 -> 0");
    CHECK(op_seekend(id, 7, &pos) == 0 && pos == 5, "seekfromend +7 -> size");
    ax = op_write(id, 10, NULL, 0, &written);
    CHECK(ax == 0, "zero-length write beyond EOF extends");
    CHECK(op_seekend(id, 0, &pos) == 0 && pos == 10, "size 10 after extend, got %u", pos);
    CHECK(op_getattr("\\SUB\\TEST.TXT", NULL, NULL, &size, &attr) == 0 && size == 10, "getattr size 10");

    /* large read/write through the frame limit */
    {
        static uint8_t big[DFS_MAX_PAYLOAD];
        for (unsigned i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i * 7);
        ax = op_write(id, 0, big, DFS_MAX_PAYLOAD - 6, &written);
        CHECK(ax == 0 && written == DFS_MAX_PAYLOAD - 6, "max-size write");
        ax = op_read(id, 0, 0xFFFF);
        CHECK(ax == 0 && last_len == DFS_MAX_PAYLOAD - 6, "read clamps to the buffer: %u", last_len);
        CHECK(memcmp(pl, big, DFS_MAX_PAYLOAD - 6) == 0, "big data intact");
        /* a smaller buffer_size must be honoured */
        wr32(req, 0); wr16(req + 4, id); wr16(req + 6, 4000);
        wr16(buf, 12); buf[2] = 0; buf[3] = AL_READFIL; memcpy(pl, req, 8);
        ax = run_frame(12, 100);
        CHECK(ax == 0 && last_len == 96, "read into 100-byte buffer -> 96, got %u", last_len);
    }

    CHECK(op_close(id) == 0, "close");
    ax = op_close(id);
    CHECK(ax == 6, "close again -> 6, got %04X", ax);
    ax = op_read(id, 0, 10);
    CHECK(ax == 6, "read on closed handle -> 6, got %04X", ax);
    ax = op_write(id, 0, "x", 1, NULL);
    CHECK(ax == 6, "write on closed handle -> 6, got %04X", ax);
    ax = op_seekend(id, 0, NULL);
    CHECK(ax == 6, "seek on closed handle -> 6, got %04X", ax);
    ax = op_close(0);
    CHECK(ax == 6, "close id 0 -> 6");
    ax = op_close(9);
    CHECK(ax == 6, "close id 9 -> 6");
}

static void test_open(void) {
    uint16_t ax, id = 0, rr = 0;
    SECTION("open");
    ax = op_open(AL_OPEN, 0, 0, 0, "\\SUB\\TEST.TXT", &id, &rr);
    CHECK(ax == 0 && rr == 1, "open existing read-only -> %04X rr %u", ax, rr);
    CHECK(pl[24] == 0, "open mode byte echoes the stack word: %02X", pl[24]);
    CHECK(rd32(pl + 16) == DFS_MAX_PAYLOAD - 6, "open reports the size");
    ax = op_write(id, 0, "x", 1, NULL);
    CHECK(ax == 5, "write on read-only handle -> 5, got %04X", ax);
    ax = op_read(id, 0, 4);
    CHECK(ax == 0 && last_len == 4, "read on read-only handle");
    CHECK(op_close(id) == 0, "close");

    ax = op_open(AL_OPEN, 0x42, 0, 0, "\\sub\\test.txt", &id, &rr);
    CHECK(ax == 0 && pl[24] == 0x42, "open r/w lower-case path, mode byte %02X", pl[24]);
    ax = op_write(id, 0, "y", 1, NULL);
    CHECK(ax == 0, "write on r/w handle");
    CHECK(op_close(id) == 0, "close");

    ax = op_open(AL_OPEN, 0, 0, 0, "\\SUB\\NOPE.TXT", &id, &rr);
    CHECK(ax == 2, "open missing file -> 2, got %04X", ax);
    ax = op_open(AL_OPEN, 0, 0, 0, "\\NOPE\\X.TXT", &id, &rr);
    CHECK(ax == 3, "open in missing dir -> 3, got %04X", ax);
    ax = op_open(AL_OPEN, 0, 0, 0, "\\SUB", &id, &rr);
    CHECK(ax == 5, "open a directory -> 5, got %04X", ax);
    ax = op_open(AL_CREATE, 0, 0, 0, "\\SUB", &id, &rr);
    CHECK(ax == 5, "create over a directory -> 5, got %04X", ax);
    ax = op_open(AL_CREATE, 0, 0, 0, "\\NOPE\\NEW.TXT", &id, &rr);
    CHECK(ax == 3, "create in missing dir -> 3, got %04X", ax);
    ax = run(AL_OPEN, "\x00\x00\x00\x00\x00\x00", 6);
    CHECK(ax == 1, "open without a path -> 1, got %04X", ax);
}

static void test_spopnfil(void) {
    uint16_t ax, id = 0, rr = 0;
    uint32_t size = 0;
    uint8_t attr = 0;
    SECTION("spopnfil action codes");
    ax = op_open(AL_SPOPNFIL, 0, 0x00, 0x02, "\\SUB\\TEST.TXT", &id, &rr);
    CHECK(ax == 0x50, "exists + fail-if-exists -> 50h, got %04X", ax);
    ax = op_open(AL_SPOPNFIL, 0, 0x01, 0x02, "\\SUB\\TEST.TXT", &id, &rr);
    CHECK(ax == 0 && rr == 1, "exists + open -> rr 1, got %04X/%u", ax, rr);
    CHECK(pl[24] == 2, "spop mode byte %02X", pl[24]);
    CHECK(op_close(id) == 0, "close");
    ax = op_open(AL_SPOPNFIL, 0, 0x11, 0x82, "\\SUB\\TEST.TXT", &id, &rr);
    CHECK(ax == 0 && rr == 1 && pl[24] == 2, "exists + open|create -> rr 1, mode masked to 7 bits");
    CHECK(op_close(id) == 0, "close");
    ax = op_open(AL_SPOPNFIL, 0, 0x02, 0x02, "\\SUB\\TEST.TXT", &id, &rr);
    CHECK(ax == 0 && rr == 3, "exists + truncate -> rr 3, got %04X/%u", ax, rr);
    CHECK(rd32(pl + 16) == 0, "truncated size 0");
    CHECK(op_close(id) == 0, "close");
    CHECK(op_getattr("\\SUB\\TEST.TXT", NULL, NULL, &size, &attr) == 0 && size == 0, "size 0 after truncate");

    ax = op_open(AL_SPOPNFIL, 0, 0x01, 0x02, "\\SUB\\NEW.TXT", &id, &rr);
    CHECK(ax == 2, "missing + open -> 2, got %04X", ax);
    ax = op_open(AL_SPOPNFIL, 0, 0x00, 0x02, "\\SUB\\NEW.TXT", &id, &rr);
    CHECK(ax == 2, "missing + fail -> 2, got %04X", ax);
    ax = op_open(AL_SPOPNFIL, 0x02, 0x10, 0x02, "\\SUB\\NEW.TXT", &id, &rr);
    CHECK(ax == 0 && rr == 2, "missing + create -> rr 2, got %04X/%u", ax, rr);
    CHECK(pl[0] & 0x02, "created file carries the requested hidden attribute: %02X", pl[0]);
    CHECK(op_write(id, 0, "new", 3, NULL) == 0, "write to created file");
    CHECK(op_close(id) == 0, "close");
    CHECK(op_getattr("\\SUB\\NEW.TXT", NULL, NULL, &size, &attr) == 0 && size == 3 && (attr & 0x22) == 0x22, "new file size %u attr %02X", size, attr);
    ax = op_open(AL_SPOPNFIL, 0, 0x12, 0x02, "\\SUB\\NEW.TXT", &id, &rr);
    CHECK(ax == 0 && rr == 3, "exists + create|truncate -> rr 3");
    CHECK(op_close(id) == 0, "close");
    ax = op_open(AL_SPOPNFIL, 0, 0x12, 0x02, "\\SUB\\NEW2.TXT", &id, &rr);
    CHECK(ax == 0 && rr == 2, "missing + create|truncate -> rr 2");
    CHECK(op_close(id) == 0, "close");
    ax = op_open(AL_SPOPNFIL, 0, 0x11, 0x02, "\\SUB", &id, &rr);
    CHECK(ax == 5, "spopnfil on a directory -> 5, got %04X", ax);
    ax = op_open(AL_SPOPNFIL, 0, 0x11, 0x02, "\\NOPE\\F.TXT", &id, &rr);
    CHECK(ax == 3, "spopnfil in missing dir -> 3, got %04X", ax);
    ax = op_open(AL_SPOPNFIL, 0, 0x10, 0x00, "\\SUB\\RO.TXT", &id, &rr);
    CHECK(ax == 0 && rr == 2, "create with read-only access");
    ax = op_write(id, 0, "x", 1, NULL);
    CHECK(ax == 5, "write on read-access handle -> 5, got %04X", ax);
    CHECK(op_close(id) == 0, "close");
}

static void test_find(void) {
    uint16_t ax;
    int n;
    search_t s, s1, s2;
    char seq1[32][12], seq2[32][12];
    int n1, n2, i;

    SECTION("findfirst / findnext");
    CHECK(run_str(AL_MKDIR, "\\FF") == 0, "mkdir \\FF");
    CHECK(run_str(AL_MKDIR, "\\FF\\SUB1") == 0, "mkdir \\FF\\SUB1");
    for (i = 1; i <= 6; i++) {
        char p[32];
        snprintf(p, sizeof(p), "\\FF\\FILE%04d.DAT", i);
        make_file(p, 0);
    }
    make_file("\\FF\\A.TXT", 0);
    make_file("\\FF\\B.TXT", 0);
    make_file("\\FF\\C.TXT", 0);
    make_file("\\FF\\HIDE.SYS", 0x02);
    make_file("\\FF\\SYSF.SYS", 0x04);
    make_file("\\FF\\X.TMP", 0);
    make_file("\\FF\\Y.TMP", 0);
    make_file("\\FF\\Z.TMP", 0);

    n = collect(0x00, "\\FF\\????????.???", names, 64);
    CHECK(n == 12, "attr 00 *.*: %d entries (12 plain files)", n);
    CHECK(!has_name(names, n, ".          "), "no '.' without the DIR bit");
    CHECK(!has_name(names, n, "HIDE    SYS"), "hidden file excluded");
    CHECK(has_name(names, n, "FILE0003DAT"), "FILE0003.DAT listed");

    n = collect(0x10, "\\FF\\????????.???", names, 64);
    CHECK(n == 15, "attr 10 *.*: %d entries (12 + SUB1 + . + ..)", n);
    CHECK(memcmp(names[0], ".          ", 11) == 0, "first entry '.', got '%s'", names[0]);
    CHECK(memcmp(names[1], "..         ", 11) == 0, "second entry '..', got '%s'", names[1]);
    CHECK(has_name(names, n, "SUB1       "), "SUB1 listed");

    n = collect(0x16, "\\FF\\????????.???", names, 64);
    CHECK(n == 17, "attr 16 *.*: %d entries", n);
    CHECK(has_name(names, n, "HIDE    SYS") && has_name(names, n, "SYSF    SYS"), "hidden and system listed");

    n = collect(0x02, "\\FF\\????????.???", names, 64);
    CHECK(n == 13, "attr 02 *.*: %d entries (12 + hidden)", n);
    n = collect(0x3F, "\\FF\\????????.???", names, 64);
    CHECK(n == 17, "attr 3F *.*: %d entries", n);

    n = collect(0x00, "\\FF\\????????.TXT", names, 64);
    CHECK(n == 3, "*.TXT: %d", n);
    CHECK(has_name(names, n, "A       TXT") && has_name(names, n, "B       TXT") && has_name(names, n, "C       TXT"), "A/B/C.TXT");
    n = collect(0x00, "\\FF\\*.TXT", names, 64);
    CHECK(n == 3, "star mask *.TXT: %d", n);
    n = collect(0x00, "\\FF\\FILE????.DAT", names, 64);
    CHECK(n == 6, "FILE????.DAT: %d", n);
    n = collect(0x10, "\\FF\\FILE????.DAT", names, 64);
    CHECK(n == 6, "FILE????.DAT with DIR bit: %d (dot entries do not match)", n);
    n = collect(0x10, "\\FF\\SUB?", names, 64);
    CHECK(n == 1 && memcmp(names[0], "SUB1       ", 11) == 0, "SUB? -> SUB1");
    n = collect(0x00, "\\ff\\file0001.dat", names, 64);
    CHECK(n == 1, "lower-case exact search: %d", n);

    /* root: no dot entries */
    n = collect(0x10, "\\????????.???", names, 64);
    CHECK(n >= 2, "root listing %d", n);
    CHECK(!has_name(names, n, ".          ") && !has_name(names, n, "..         "), "no dot entries in root");
    CHECK(has_name(names, n, "SUB        ") && has_name(names, n, "FF         "), "root has SUB and FF");

    /* failures */
    ax = op_findfirst(0x00, "\\FF\\NOPE????.???", &s);
    CHECK(ax == 0x12, "no match -> 12h, got %04X", ax);
    ax = op_findfirst(0x00, "\\NOPE\\????????.???", &s);
    CHECK(ax == 3, "missing directory -> 3, got %04X", ax);
    ax = op_findfirst(0x10, "\\FF\\????????.???", &s);
    CHECK(ax == 0 && s.dir_id != 0, "dir id never 0");
    s.dir_id = 0;
    ax = op_findnext(&s);
    CHECK(ax == 0x12, "findnext with dir id 0 -> 12h, got %04X", ax);
    s.dir_id = 0x7777;
    ax = op_findnext(&s);
    CHECK(ax == 0x12, "findnext with bogus id -> 12h, got %04X", ax);
    ax = run(AL_FINDNEXT, "\x01\x00\x00\x00\x10", 5);
    CHECK(ax == 1, "short findnext -> 1, got %04X", ax);

    /* attributes, timestamps and sizes come through */
    ax = op_findfirst(0x00, "\\FF\\HIDE.SYS", &s);
    CHECK(ax == 0x12, "hidden HIDE.SYS not found without HID bit, got %04X", ax);
    ax = op_findfirst(0x02, "\\FF\\HIDE.SYS", &s);
    CHECK(ax == 0 && s.size == 0 && (s.attr & 0x02), "hidden HIDE.SYS found with HID bit: %04X size %u attr %02X", ax, s.size, s.attr);
    CHECK(s.date != 0, "entry has a date");
    /* the truncating SPOPNFIL above reset NEW.TXT's attributes (DOS 3Ch semantics) */
    ax = op_findfirst(0x00, "\\SUB\\NEW.TXT", &s);
    CHECK(ax == 0 && s.attr == 0x20 && s.size == 0, "truncated NEW.TXT is a plain file: %04X attr %02X", ax, s.attr);
    /* COMMAND.COM (PATHCRUNCH) decides "is this a directory" with CHDIR: a file must fail with 3 */
    ax = run_str(AL_CHDIR, "\\SUB\\NEW.TXT");
    CHECK(ax == 3, "chdir on a plain file -> 3 (path not found), got %04X", ax);
    ax = run_str(AL_CHDIR, "\\SUB\\NEW.TXT\\");
    CHECK(ax == 3, "chdir on a plain file with trailing separator -> 3, got %04X", ax);
    ax = run_str(AL_CHDIR, "\\SUB\\");
    CHECK(ax == 0, "chdir on a directory with trailing separator -> 0, got %04X", ax);

    /* interleaved searches in two directories (DIR /S, XCOPY) */
    n1 = collect(0x10, "\\FF\\????????.???", seq1, 32);
    n2 = collect(0x10, "\\SUB\\????????.???", seq2, 32);
    CHECK(n1 == 15 && n2 >= 3, "reference sequences %d / %d", n1, n2);
    {
        uint16_t ax1 = op_findfirst(0x10, "\\FF\\????????.???", &s1);
        uint16_t ax2 = op_findfirst(0x10, "\\SUB\\????????.???", &s2);
        int i1 = 0, i2 = 0, ok = 1;
        CHECK(s1.dir_id != s2.dir_id, "different directories get different ids");
        while (ax1 == 0 || ax2 == 0) {
            if (ax1 == 0) {
                if (i1 >= n1 || memcmp(s1.name, seq1[i1], 11) != 0) ok = 0;
                i1++;
                ax1 = op_findnext(&s1);
            }
            if (ax2 == 0) {
                if (i2 >= n2 || memcmp(s2.name, seq2[i2], 11) != 0) ok = 0;
                i2++;
                ax2 = op_findnext(&s2);
            }
        }
        CHECK(ok && i1 == n1 && i2 == n2, "interleaved searches match: %d/%d %d/%d ok=%d", i1, n1, i2, n2, ok);
    }
    /* a search resumed at an arbitrary earlier position */
    {
        uint16_t axr = op_findfirst(0x00, "\\FF\\????????.???", &s);
        search_t saved;
        CHECK(axr == 0, "resume: first");
        axr = op_findnext(&s);
        CHECK(axr == 0, "resume: second");
        saved = s;
        axr = op_findnext(&s);
        CHECK(axr == 0, "resume: third");
        /* go back to the saved DTA: the next entry must be the third one again */
        axr = op_findnext(&saved);
        CHECK(axr == 0 && memcmp(saved.name, s.name, 11) == 0 && saved.pos == s.pos, "resume from an older position");
    }

    /* directory id eviction: DFS_MAX_DIRS slots, one more evicts the oldest */
    {
        search_t first;
        uint16_t axe = op_findfirst(0x10, "\\FF\\????????.???", &first);
        CHECK(axe == 0, "eviction: search in \\FF");
        for (i = 0; i < DFS_MAX_DIRS; i++) {
            char p[32];
            snprintf(p, sizeof(p), "\\FF\\E%d", i);
            CHECK(run_str(AL_MKDIR, p) == 0, "mkdir %s", p);
            strcat(p, "\\????????.???");
            axe = op_findfirst(0x10, p, &s);
            CHECK(axe == 0, "search in %s", p);
        }
        axe = op_findnext(&first);
        CHECK(axe == 0x12, "evicted id -> 12h, got %04X", axe);
        for (i = 0; i < DFS_MAX_DIRS; i++) {
            char p[32];
            snprintf(p, sizeof(p), "\\FF\\E%d", i);
            CHECK(run_str(AL_RMDIR, p) == 0, "rmdir %s", p);
        }
    }

    /* deleting the entry just returned, with a search in another directory
     * in between (DELTREE recursing, installers), must not skip the next
     * entry: positions are directory slots, not counts of live entries */
    {
        search_t a, b;
        int visited = 0, ok = 1;
        uint16_t axd;
        CHECK(run_str(AL_MKDIR, "\\DL") == 0, "mkdir \\DL");
        for (i = 0; i < 6; i++) {
            char p[32];
            uint16_t fid;
            snprintf(p, sizeof(p), "\\DL\\D%d.TMP", i);
            CHECK(op_open(AL_CREATE, 0, 0, 0, p, &fid, NULL) == 0, "create %s", p);
            CHECK(op_close(fid) == 0, "close %s", p);
        }
        axd = op_findfirst(0x00, "\\DL\\????????.TMP", &a);
        while (axd == 0 && visited < 10) {
            char p[32];
            snprintf(p, sizeof(p), "\\DL\\%c%c.TMP", a.name[0], a.name[1]);
            if (run_str(AL_DELETE, p) != 0) ok = 0;
            visited++;
            /* another directory's search reopens the cached DIR */
            op_findfirst(0x10, "\\FF\\????????.???", &b);
            axd = op_findnext(&a);
        }
        CHECK(ok && visited == 6 && axd == 0x12, "delete while searching visits every file: %d, ok=%d, end=%04X", visited, ok, axd);
        axd = op_findfirst(0x00, "\\DL\\????????.???", &a);
        CHECK(axd == 0x12, "\\DL is empty afterwards, got %04X", axd);
        CHECK(run_str(AL_RMDIR, "\\DL") == 0, "rmdir \\DL");
    }

    /* volume label */
    ax = op_findfirst(0x08, "\\*.*", &s);
    CHECK(ax == 0, "label search -> %04X", ax);
    CHECK(s.attr == 0x08 && memcmp(s.name, "PGTEST     ", 11) == 0, "label '%s' attr %02X", s.name, s.attr);
    ax = op_findnext(&s);
    CHECK(ax == 0x12, "findnext after the label -> 12h, got %04X", ax);
    ax = op_findfirst(0x08, "\\FF\\*.*", &s);
    CHECK(ax == 0x12, "label search in a subdirectory -> 12h, got %04X", ax);
    ax = op_findfirst(0x08, "\\NOPE.*", &s);
    CHECK(ax == 0x12, "label search with a non-matching mask -> 12h, got %04X", ax);
}

static void test_attr_delete_rename(void) {
    uint16_t ax, id = 0;
    uint8_t attr = 0;
    int n;

    SECTION("getattr / setattr / delete / rename");
    CHECK(op_getattr("\\FF\\A.TXT", NULL, NULL, NULL, &attr) == 0 && attr == 0x20, "A.TXT attr %02X", attr);
    CHECK(op_setattr("\\FF\\A.TXT", 0x01) == 0, "setattr read-only");
    CHECK(op_getattr("\\FF\\A.TXT", NULL, NULL, NULL, &attr) == 0 && attr == 0x01, "A.TXT attr now %02X", attr);
    ax = run_str(AL_DELETE, "\\FF\\A.TXT");
    CHECK(ax == 5, "delete read-only -> 5, got %04X", ax);
    ax = op_open(AL_OPEN, 2, 0, 0, "\\FF\\A.TXT", &id, NULL);
    CHECK(ax == 5, "open read-only file for r/w -> 5, got %04X", ax);
    ax = op_open(AL_OPEN, 0, 0, 0, "\\FF\\A.TXT", &id, NULL);
    CHECK(ax == 0, "open read-only file for read");
    if (ax == 0) CHECK(op_close(id) == 0, "close");
    ax = op_open(AL_CREATE, 0, 0, 0, "\\FF\\A.TXT", &id, NULL);
    CHECK(ax == 5, "create over a read-only file -> 5, got %04X", ax);
    ax = op_rename("\\FF\\A.TXT", "\\FF\\A2.TXT");
    CHECK(ax == 0, "rename a read-only file -> %04X", ax);
    CHECK(op_setattr("\\FF\\A2.TXT", 0x20) == 0, "setattr archive");
    CHECK(run_str(AL_DELETE, "\\FF\\A2.TXT") == 0, "delete ok");
    ax = op_getattr("\\FF\\A2.TXT", NULL, NULL, NULL, &attr);
    CHECK(ax == 2, "getattr deleted -> 2, got %04X", ax);
    ax = run_str(AL_DELETE, "\\FF\\A2.TXT");
    CHECK(ax == 2, "delete missing -> 2, got %04X", ax);
    ax = run_str(AL_DELETE, "\\NOPE\\A2.TXT");
    CHECK(ax == 3, "delete in missing dir -> 3, got %04X", ax);
    ax = op_setattr("\\FF\\NOPE.TXT", 0x01);
    CHECK(ax == 2, "setattr missing -> 2, got %04X", ax);
    CHECK(op_getattr("\\", NULL, NULL, NULL, &attr) == 0 && attr == 0x10, "getattr root -> dir");
    CHECK(op_getattr("\\FF", NULL, NULL, NULL, &attr) == 0 && attr == 0x10, "getattr dir");
    ax = op_getattr("\\NOPE\\X", NULL, NULL, NULL, &attr);
    CHECK(ax == 3, "getattr in missing dir -> 3, got %04X", ax);
    ax = run_str(AL_DELETE, "\\FF");
    CHECK(ax == 5, "delete a directory -> 5, got %04X", ax);
    CHECK(op_setattr("\\FF\\SUB1", 0x02) == 0, "setattr on a directory");
    CHECK(op_getattr("\\FF\\SUB1", NULL, NULL, NULL, &attr) == 0 && attr == 0x12, "dir attr %02X", attr);
    CHECK(op_setattr("\\FF\\SUB1", 0x00) == 0, "setattr clear");

    /* rename */
    CHECK(op_rename("\\FF\\B.TXT", "\\FF\\BB.TXT") == 0, "rename");
    CHECK(op_getattr("\\FF\\B.TXT", NULL, NULL, NULL, NULL) == 2, "old name gone");
    CHECK(op_getattr("\\FF\\BB.TXT", NULL, NULL, NULL, NULL) == 0, "new name there");
    ax = op_rename("\\FF\\BB.TXT", "\\FF\\C.TXT");
    CHECK(ax != 0, "rename onto an existing name fails, got %04X", ax);
    CHECK(op_getattr("\\FF\\BB.TXT", NULL, NULL, NULL, NULL) == 0, "source untouched");
    ax = op_rename("\\FF\\NOPE.TXT", "\\FF\\Q.TXT");
    CHECK(ax == 2, "rename missing -> 2, got %04X", ax);
    ax = op_rename("\\FF\\BB.TXT", "\\NOPE\\Q.TXT");
    CHECK(ax == 3, "rename into missing dir -> 3, got %04X", ax);
    CHECK(op_rename("\\FF\\SUB1", "\\FF\\SUB2") == 0, "rename a directory");
    CHECK(run_str(AL_CHDIR, "\\FF\\SUB2") == 0, "renamed directory exists");
    CHECK(op_rename("\\FF\\BB.TXT", "\\SUB\\MOVED.TXT") == 0, "rename across directories");
    CHECK(op_getattr("\\SUB\\MOVED.TXT", NULL, NULL, NULL, NULL) == 0, "moved file there");
    ax = run(AL_RENAME, "\x05\\A", 3);
    CHECK(ax == 1, "malformed rename -> 1, got %04X", ax);

    /* wildcard delete */
    n = collect(0x00, "\\FF\\????????.TMP", names, 64);
    CHECK(n == 3, "3 TMP files before delete");
    CHECK(run_str(AL_DELETE, "\\FF\\????????.TMP") == 0, "delete *.TMP");
    ax = op_findfirst(0x00, "\\FF\\????????.TMP", &(search_t){0});
    CHECK(ax == 0x12, "no TMP files left");
    ax = run_str(AL_DELETE, "\\FF\\*.TMP");
    CHECK(ax == 2, "wildcard delete with no match -> 2, got %04X", ax);
    ax = run_str(AL_DELETE, "\\NOPE\\*.TMP");
    CHECK(ax == 3, "wildcard delete in missing dir -> 3, got %04X", ax);
    CHECK(op_setattr("\\FF\\FILE0003.DAT", 0x01) == 0, "make FILE0003.DAT read-only");
    ax = run_str(AL_DELETE, "\\FF\\FILE????.DAT");
    CHECK(ax == 5, "wildcard delete over a read-only file -> 5, got %04X", ax);
    n = collect(0x00, "\\FF\\FILE????.DAT", names, 64);
    CHECK(n == 1 && memcmp(names[0], "FILE0003DAT", 11) == 0, "only the read-only file survives: %d", n);
    CHECK(op_setattr("\\FF\\FILE0003.DAT", 0x20) == 0, "clear read-only");
    CHECK(run_str(AL_DELETE, "\\FF\\FILE????.DAT") == 0, "delete the rest");
    n = collect(0x10, "\\FF\\????????.???", names, 64);
    CHECK(n == 4, "directory after deletes: %d entries (., .., SUB2, C.TXT)", n);
    n = collect(0x16, "\\FF\\????????.???", names, 64);
    CHECK(n == 6, "with hidden/system: %d entries (+ HIDE.SYS, SYSF.SYS)", n);
}

static void test_time(void) {
    uint16_t ax, id = 0, t = 0, d = 0;
    uint32_t size = 0;
    SECTION("timestamps");
    /* 2026-09-10 12:33:00 */
    dfs_server_set_dos_time(0x6420, 0x5D2A);
    make_file("\\T1.TXT", 0);
    CHECK(op_getattr("\\T1.TXT", &t, &d, NULL, NULL) == 0, "getattr T1");
    CHECK(t == 0x6420 && d == 0x5D2A, "T1 stamp %04X %04X (want 6420 5D2A)", t, d);
    test_clock_ms += 90000;             /* +1:30 -> 12:34:30 */
    make_file("\\T2.TXT", 0);
    CHECK(op_getattr("\\T2.TXT", &t, &d, NULL, NULL) == 0, "getattr T2");
    CHECK(t == 0x644F && d == 0x5D2A, "T2 stamp %04X %04X (want 644F 5D2A)", t, d);
    /* day/year rollover: 2026-12-31 23:59:58 + 4 s */
    dfs_server_set_dos_time(0xBF7D, 0x5D9F);
    test_clock_ms += 4000;
    make_file("\\T3.TXT", 0);
    CHECK(op_getattr("\\T3.TXT", &t, &d, NULL, NULL) == 0, "getattr T3");
    CHECK(t == 0x0001 && d == 0x5E21, "T3 stamp %04X %04X (want 0001 5E21)", t, d);
    /* leap day: 2028-02-28 23:59:58 + 4 s -> 2028-02-29 00:00:02 */
    dfs_server_set_dos_time(0xBF7D, 0x605C);
    test_clock_ms += 4000;
    make_file("\\T4.TXT", 0);
    CHECK(op_getattr("\\T4.TXT", &t, &d, NULL, NULL) == 0, "getattr T4");
    CHECK(t == 0x0001 && d == 0x605D, "T4 stamp %04X %04X (want 0001 605D)", t, d);
    /* the millisecond counter wraps between calls */
    dfs_server_set_dos_time(0x6420, 0x5D2A);
    test_clock_ms = 0xFFFFFC18u;        /* wraps in 1000 ms */
    dfs_server_set_dos_time(0x6420, 0x5D2A);
    test_clock_ms += 2000;              /* through the wrap: 12:33:02 */
    make_file("\\T5.TXT", 0);
    CHECK(op_getattr("\\T5.TXT", &t, &d, NULL, NULL) == 0 && t == 0x6421, "T5 stamp %04X after counter wrap (want 6421)", t);
    /* a modified file gets the current time */
    test_clock_ms += 120000;            /* 12:35:02 */
    ax = op_open(AL_OPEN, 2, 0, 0, "\\T1.TXT", &id, NULL);
    CHECK(ax == 0, "open T1 r/w");
    CHECK(op_write(id, 0, "data", 4, NULL) == 0, "write T1");
    CHECK(op_close(id) == 0, "close T1");
    CHECK(op_getattr("\\T1.TXT", &t, &d, &size, NULL) == 0 && t == 0x6461 && size == 4, "T1 after write: %04X size %u (want 6461 4)", t, size);

    /* SETFILETIMESTAMP on an open file survives the close */
    ax = op_open(AL_OPEN, 2, 0, 0, "\\T1.TXT", &id, NULL);
    CHECK(ax == 0, "open T1 again");
    CHECK(op_write(id, 4, "more", 4, NULL) == 0, "write T1 (pending)");
    ax = op_utime(id, 0x4800, 0x2821);  /* 2000-01-01 09:00:00 */
    CHECK(ax == 0, "setfiletimestamp -> %04X", ax);
    CHECK(op_close(id) == 0, "close T1");
    CHECK(op_getattr("\\T1.TXT", &t, &d, &size, NULL) == 0, "getattr T1");
    CHECK(t == 0x4800 && d == 0x2821 && size == 8, "T1 stamp %04X %04X size %u (want 4800 2821 8)", t, d, size);
    ax = op_utime(id, 0x4800, 0x2821);
    CHECK(ax == 6, "setfiletimestamp on closed handle -> 6, got %04X", ax);
    ax = run(AL_SETFTIME, "\x00\x00\x00\x00", 4);
    CHECK(ax == 1, "short setfiletimestamp -> 1, got %04X", ax);
    /* the search reports the same stamp */
    {
        search_t s;
        CHECK(op_findfirst(0x00, "\\T1.TXT", &s) == 0 && s.time == 0x4800 && s.date == 0x2821 && s.size == 8, "findfirst stamp %04X %04X", s.time, s.date);
    }
}

static void test_lfn(void) {
    FIL f;
    FRESULT fr;
    uint16_t ax, id = 0;
    search_t s;
    SECTION("long file names");
    fr = f_open(&f, "\\FF\\LongFileName.txt", FA_CREATE_ALWAYS | FA_WRITE);
    CHECK(fr == FR_OK, "f_open LFN -> %d", fr);
    if (fr == FR_OK) {
        UINT bw;
        f_write(&f, "lfn", 3, &bw);
        f_close(&f);
    }
    fr = f_open(&f, "\\FF\\lower.txt", FA_CREATE_ALWAYS | FA_WRITE);
    CHECK(fr == FR_OK, "f_open lower-case SFN -> %d", fr);
    if (fr == FR_OK) f_close(&f);
    ax = op_findfirst(0x00, "\\FF\\LONGFI??.???", &s);
    CHECK(ax == 0, "findfirst LONGFI?? -> %04X", ax);
    CHECK(memcmp(s.name, "LONGFI~1TXT", 11) == 0, "LFN reported as '%s'", s.name);
    CHECK(s.size == 3, "LFN size %u", s.size);
    ax = op_findfirst(0x00, "\\FF\\LOWER.TXT", &s);
    CHECK(ax == 0 && memcmp(s.name, "LOWER   TXT", 11) == 0, "lower-case SFN reported upper-case '%s'", s.name);
    ax = op_open(AL_OPEN, 0, 0, 0, "\\FF\\LONGFI~1.TXT", &id, NULL);
    CHECK(ax == 0, "open by 8.3 alias -> %04X", ax);
    if (ax == 0) {
        CHECK(memcmp(pl + 1, "LONGFI~1TXT", 11) == 0, "open reports the alias");
        CHECK(op_read(id, 0, 10) == 0 && last_len == 3 && memcmp(pl, "lfn", 3) == 0, "read through alias");
        CHECK(op_close(id) == 0, "close");
    }
    CHECK(op_getattr("\\FF\\longfi~1.txt", NULL, NULL, NULL, NULL) == 0, "getattr by lower-case alias");
    CHECK(run_str(AL_DELETE, "\\FF\\LONGFI~1.TXT") == 0, "delete by alias");
    CHECK(op_getattr("\\FF\\LongFileName.txt", NULL, NULL, NULL, NULL) == 2, "LFN gone");
}

/* answer payload equals s (no terminator on the wire) */
static int payload_is(const char *s) {
    return (size_t)last_len == strlen(s) && memcmp(pl, s, last_len) == 0;
}

static void test_longname(void) {
    FIL f;
    FRESULT fr;
    uint16_t ax;
    search_t s;
    /* 52 characters: longer than the 32-byte payload buffer used below */
    static const char lfn[]  = "A Rather Long File Name That Exceeds Thirty Two.txt";
    static const char ldir[] = "Long Directory Name";
    SECTION("long name lookup");
    fr = f_open(&f, "\\FF\\A Rather Long File Name That Exceeds Thirty Two.txt", FA_CREATE_ALWAYS | FA_WRITE);
    CHECK(fr == FR_OK, "f_open LFN -> %d", fr);
    if (fr == FR_OK) f_close(&f);
    fr = f_mkdir("\\FF\\Long Directory Name");
    CHECK(fr == FR_OK, "f_mkdir LFN dir -> %d", fr);

    /* the redirector interface only ever shows the 8.3 alias */
    ax = op_findfirst(0x00, "\\FF\\ARATHE??.???", &s);
    CHECK(ax == 0 && memcmp(s.name, "ARATHE~1TXT", 11) == 0, "findfirst file alias '%s' (%04X)", s.name, ax);
    ax = op_findfirst(0x10, "\\FF\\LONGDI??.???", &s);
    CHECK(ax == 0 && memcmp(s.name, "LONGDI~1   ", 11) == 0, "findfirst dir alias '%s' (%04X)", s.name, ax);

    /* LONGNAME on the alias returns the long name */
    ax = run_str(DFS_AL_LONGNAME, "\\FF\\ARATHE~1.TXT");
    CHECK(ax == 0, "longname file -> %04X", ax);
    CHECK(payload_is(lfn), "longname file '%.*s'", last_len, pl);
    ax = run_str(DFS_AL_LONGNAME, "\\ff\\arathe~1.txt");
    CHECK(ax == 0 && payload_is(lfn), "longname path is case-insensitive (%04X)", ax);
    ax = run_str(DFS_AL_LONGNAME, "\\FF\\LONGDI~1");
    CHECK(ax == 0 && payload_is(ldir), "longname dir '%.*s' (%04X)", last_len, pl, ax);
    /* a plain 8.3 entry is its own long name */
    ax = run_str(DFS_AL_LONGNAME, "\\T1.TXT");
    CHECK(ax == 0 && payload_is("T1.TXT"), "longname 8.3 file '%.*s' (%04X)", last_len, pl, ax);
    ax = run_str(DFS_AL_LONGNAME, "\\FF");
    CHECK(ax == 0 && payload_is("FF"), "longname 8.3 dir '%.*s' (%04X)", last_len, pl, ax);
    /* an 8.3 entry with NT case bits only: the long name is the cased spelling.
     * lower.txt (from test_lfn) sits elsewhere in \FF, so the resumed scan wraps. */
    ax = run_str(DFS_AL_LONGNAME, "\\FF\\LOWER.TXT");
    CHECK(ax == 0 && payload_is("lower.txt"), "longname case-only name '%.*s' (%04X)", last_len, pl, ax);
    ax = run_str(DFS_AL_LONGNAME, "\\FF\\ARATHE~1.TXT");
    CHECK(ax == 0 && payload_is(lfn), "longname after wrapping around (%04X)", ax);
    /* dot entries are their own names */
    ax = run_str(DFS_AL_LONGNAME, "\\FF\\.");
    CHECK(ax == 0 && payload_is("."), "longname '.' (%04X)", ax);
    ax = run_str(DFS_AL_LONGNAME, "\\FF\\..");
    CHECK(ax == 0 && payload_is(".."), "longname '..' (%04X)", ax);
    /* the root has no name */
    ax = run_str(DFS_AL_LONGNAME, "\\");
    CHECK(ax == 0 && last_len == 0, "longname root -> %04X len %u", ax, last_len);
    /* errors: the GETATTR codes */
    ax = run_str(DFS_AL_LONGNAME, "\\FF\\NOPE.TXT");
    CHECK(ax == 2, "longname missing file -> 2, got %04X", ax);
    ax = run_str(DFS_AL_LONGNAME, "\\NOPE\\ARATHE~1.TXT");
    CHECK(ax == 3, "longname missing dir -> 3, got %04X", ax);
    ax = run(DFS_AL_LONGNAME, NULL, 0);
    CHECK(ax == 1, "longname empty payload -> 1, got %04X", ax);
    ax = run_str(DFS_AL_LONGNAME, "\\FF\\ARATHE??.TXT");
    CHECK(ax == 3, "longname wildcard -> 3, got %04X", ax);
    ax = run_str(DFS_AL_LONGNAME, "\\FF\\");
    CHECK(ax == 3, "longname trailing separator -> 3, got %04X", ax);
    ax = run_drv(1, DFS_AL_LONGNAME, "\\T1.TXT", 7);
    CHECK(ax == 0x0F, "longname on drive 1 -> 0Fh, got %04X", ax);
    /* the answer is clamped to the buffer, never past it */
    {
        const char *p = "\\FF\\ARATHE~1.TXT";
        uint16_t n = (uint16_t)strlen(p);
        wr16(buf, (uint16_t)(DFS_HDR_LEN + n));
        buf[2] = 0;
        buf[3] = DFS_AL_LONGNAME;
        memcpy(pl, p, n);
        ax = run_frame((uint16_t)(DFS_HDR_LEN + n), DFS_HDR_LEN + 32);
        CHECK(ax == 0 && last_len == 32 && memcmp(pl, lfn, 32) == 0, "longname clamped to a 32-byte payload: %04X len %u", ax, last_len);
    }
    fr = f_unlink("\\FF\\A Rather Long File Name That Exceeds Thirty Two.txt");
    CHECK(fr == FR_OK, "cleanup LFN file -> %d", fr);
    fr = f_unlink("\\FF\\Long Directory Name");
    CHECK(fr == FR_OK, "cleanup LFN dir -> %d", fr);
    /* the resumed scan copes with entries deleted since the last lookup */
    ax = run_str(DFS_AL_LONGNAME, "\\FF\\ARATHE~1.TXT");
    CHECK(ax == 2, "longname deleted entry -> 2, got %04X", ax);
    ax = run_str(DFS_AL_LONGNAME, "\\FF\\LOWER.TXT");
    CHECK(ax == 0 && payload_is("lower.txt"), "longname still works after a miss (%04X)", ax);
}

static void test_handles(void) {
    uint16_t ax, ids[8], id = 0;
    int i;
    SECTION("open-file table");
    for (i = 0; i < 8; i++) {
        char p[16];
        snprintf(p, sizeof(p), "\\H%d.TXT", i);
        ax = op_open(AL_CREATE, 0, 0, 0, p, &ids[i], NULL);
        CHECK(ax == 0 && ids[i] >= 1 && ids[i] <= 8, "open #%d -> %04X id %u", i, ax, ids[i]);
    }
    for (i = 1; i < 8; i++) CHECK(ids[i] != ids[0], "ids are distinct");
    ax = op_open(AL_CREATE, 0, 0, 0, "\\H8.TXT", &id, NULL);
    CHECK(ax == 4, "ninth open -> 4, got %04X", ax);
    CHECK(op_close(ids[3]) == 0, "close one");
    ax = op_open(AL_CREATE, 0, 0, 0, "\\H8.TXT", &id, NULL);
    CHECK(ax == 0 && id == ids[3], "slot reused: %04X id %u", ax, id);
    CHECK(op_write(ids[7], 0, "seven", 5, NULL) == 0, "write on handle 7");
    CHECK(op_read(ids[7], 0, 5) == 0 && last_len == 5, "read on handle 7");
    for (i = 0; i < 8; i++) {
        if (i == 3) continue;
        CHECK(op_close(ids[i]) == 0, "close #%d", i);
    }
    CHECK(op_close(id) == 0, "close reused");
    ax = op_close(ids[3]);
    CHECK(ax == 6, "stale handle -> 6");
}

static void test_diag_faults(void) {
    uint16_t ax, id = 0, written = 0;
    uint32_t saved_free;
    diag_t before, d;
    SECTION("diag record / disk faults");

    /* counters move with a write, and the last write is described */
    op_diag(&before);
    ax = op_open(AL_CREATE, 0, 0, 0, "\\DIAG.TXT", &id, NULL);
    CHECK(ax == 0, "create DIAG.TXT -> %04X", ax);
    CHECK(op_write(id, 0, "telemetry", 9, &written) == 0 && written == 9, "write DIAG.TXT");
    op_diag(&d);
    CHECK(d.writes > before.writes, "writes counted: %u -> %u", before.writes, d.writes);
    CHECK(d.wr_res == RES_OK && d.wr_cause == MSC_IO_OK, "last write ok: res %u cause %u", d.wr_res, d.wr_cause);
    CHECK(d.wr_count >= 1 && d.wr_lba > 0, "last write %u sector(s) at %u", d.wr_count, d.wr_lba);
    CHECK(d.wr_us == 100u * d.wr_count, "last write time %u us", d.wr_us);
    CHECK(d.wr_csw_err == 0 && d.csw_status == 0, "no write errors yet");
    CHECK(d.last_fr == FR_NO_FILE || d.last_fr == FR_NO_PATH || d.last_fr == 0 || d.last_fr == FR_EXIST,
          "last (soft) error is a lookup miss: %u from call %u", d.last_fr, d.last_call);

    /* a failing disk write: WRITE answers 1Dh (write fault), not 15h */
    ramdisk_set_write_fault(true);
    ax = op_write(id, 9, " more", 5, &written);
    ramdisk_set_write_fault(false);
    CHECK(ax == DFS_ERR_WRFAULT, "write on a failing disk -> 1Dh, got %04X", ax);
    op_diag(&d);
    CHECK(d.hard_fr == FR_DISK_ERR, "diag hard error FR_DISK_ERR, got %u", d.hard_fr);
    CHECK(d.hard_call == DFS_CALL_WRITE || d.hard_call == DFS_CALL_SYNC, "diag hard error from write/sync, got call %u", d.hard_call);
    CHECK(d.last_fr == FR_DISK_ERR && d.last_call == d.hard_call, "diag last error is the same: %u/%u", d.last_fr, d.last_call);
    CHECK(d.wr_res == RES_ERROR && d.wr_cause == MSC_IO_CSW, "diag last write failed: res %u cause %u", d.wr_res, d.wr_cause);
    CHECK(d.wr_csw_err == 1 && d.csw_status == 1 && d.csw_residue == 512, "diag CSW telemetry: %u errors, status %u, residue %u", d.wr_csw_err, d.csw_status, d.csw_residue);
    CHECK(d.wr_refused == 0 && d.timeouts == 0 && d.gone == 0, "diag other counters untouched");
    /* the handle recovers once the disk does */
    ax = op_write(id, 9, " more", 5, &written);
    CHECK(ax == 0 && written == 5, "write after the fault cleared -> %04X/%u", ax, written);
    ax = op_close(id);
    CHECK(ax == 0, "close after the fault -> %04X", ax);

    /* a failing disk write during CLOSE (pending data) is a write fault too */
    ax = op_open(AL_OPEN, 2, 0, 0, "\\DIAG.TXT", &id, NULL);
    CHECK(ax == 0, "reopen DIAG.TXT r/w");
    if (ax == 0) {
        ramdisk_set_write_fault(true);
        ax = op_close(id);
        ramdisk_set_write_fault(false);
        CHECK(ax == 0 || ax == DFS_ERR_WRFAULT, "close with a failing disk -> 0 or 1Dh, got %04X", ax);
        if (ax != 0) CHECK(op_close(id) == 6, "handle released although the flush failed");
    }

    /* a failing disk read: READ answers 1Eh (read fault) */
    ax = op_open(AL_OPEN, 0, 0, 0, "\\DIAG.TXT", &id, NULL);
    CHECK(ax == 0, "open DIAG.TXT for reading");
    ramdisk_set_read_fault(true);
    ax = op_read(id, 0, 14);
    ramdisk_set_read_fault(false);
    CHECK(ax == DFS_ERR_RDFAULT, "read on a failing disk -> 1Eh, got %04X", ax);
    op_diag(&d);
    CHECK(d.hard_fr == FR_DISK_ERR && d.hard_call == DFS_CALL_READ, "diag hard error from read: %u/%u", d.hard_fr, d.hard_call);
    CHECK(d.rd_res == RES_ERROR && d.rd_cause == MSC_IO_CSW && d.rd_csw_err == 1, "diag last read failed: res %u cause %u errors %u", d.rd_res, d.rd_cause, d.rd_csw_err);
    CHECK(op_close(id) == 0, "close after the read fault");

    /* MKDIR on a failing disk: 1Dh, and the FatFs call is f_mkdir */
    ramdisk_set_write_fault(true);
    ax = run_str(AL_MKDIR, "\\FAULTDIR");
    ramdisk_set_write_fault(false);
    CHECK(ax == DFS_ERR_WRFAULT, "mkdir on a failing disk -> 1Dh, got %04X", ax);
    op_diag(&d);
    CHECK(d.hard_fr == FR_DISK_ERR && d.hard_call == DFS_CALL_MKDIR, "diag hard error from mkdir: %u/%u", d.hard_fr, d.hard_call);
    CHECK(d.wr_res == RES_ERROR && d.wr_cause == MSC_IO_CSW, "diag last write failed in mkdir");
    /* whatever FatFs left behind, the disk works again */
    ax = run_str(AL_MKDIR, "\\FAULTDIR");
    CHECK(ax == 0 || ax == 5, "mkdir after the fault cleared -> 0 or 5 (already there), got %04X", ax);
    CHECK(run_str(AL_CHDIR, "\\FAULTDIR") == 0, "the directory exists afterwards");
    CHECK(run_str(AL_RMDIR, "\\FAULTDIR") == 0, "rmdir FAULTDIR");
    CHECK(run_str(AL_DELETE, "\\DIAG.TXT") == 0, "delete DIAG.TXT");

    /* the field failure: FatFs believing the volume is full (a stale FSINFO
     * count of 0, as sticks written by other OSes carry when FF_FS_NOFSINFO
     * is 0) makes every allocation FR_DENIED = DOS 5 "Access denied", while
     * the DIAG record shows 0 free clusters and FR_DENIED from f_mkdir */
    saved_free = (uint32_t)fatfs.free_clst;
    fatfs.free_clst = 0;
    ax = run_str(AL_MKDIR, "\\FULLDIR");
    CHECK(ax == 5, "mkdir with FatFs believing 0 free clusters -> 5, got %04X", ax);
    op_diag(&d);
    CHECK(d.free_clst == 0, "diag free clusters 0, got %u", d.free_clst);
    CHECK(d.hard_fr == FR_DENIED && d.hard_call == DFS_CALL_MKDIR, "diag hard error FR_DENIED from mkdir: %u/%u", d.hard_fr, d.hard_call);
    CHECK(d.wr_res == RES_OK, "no disk write was even attempted: last write res %u", d.wr_res);
    ax = op_open(AL_CREATE, 0, 0, 0, "\\FULL.TXT", &id, NULL);
    CHECK(ax == 0, "create with 0 free clusters still succeeds (entry only) -> %04X", ax);
    if (ax == 0) {
        /* f_write() reports "disk full" the DOS way: success with fewer bytes
         * (here 0) rather than an error, so only MKDIR shows the FR_DENIED */
        ax = op_write(id, 0, "x", 1, &written);
        CHECK(ax == 0 && written == 0, "write with FatFs believing 0 free clusters -> 0 bytes, got %04X/%u", ax, written);
        CHECK(op_close(id) == 0, "close FULL.TXT");
        CHECK(run_str(AL_DELETE, "\\FULL.TXT") == 0, "delete FULL.TXT");
    }
    fatfs.free_clst = 0xFFFFFFFFu;           /* unknown: the next DISKSPACE rescans */
    CHECK(run(AL_DISKSPACE, NULL, 0) == 1, "diskspace rescans");
    op_diag(&d);
    CHECK(d.free_clst <= d.n_fatent - 2 && d.free_clst + 8 >= saved_free && d.free_clst <= saved_free + 8,
          "diag free clusters back to %u (was %u)", d.free_clst, saved_free);
    CHECK(run_str(AL_MKDIR, "\\FULLDIR") == 0 && run_str(AL_RMDIR, "\\FULLDIR") == 0, "allocation works again");
}

static void test_unmount(void) {
    uint16_t ax, id = 0;
    search_t s;
    SECTION("unmount / remount");
    ax = op_open(AL_OPEN, 2, 0, 0, "\\T1.TXT", &id, NULL);
    CHECK(ax == 0, "open before unmount");
    ax = op_findfirst(0x10, "\\FF\\????????.???", &s);
    CHECK(ax == 0, "search before unmount");
    dfs_server_drive_unmounted();
    CHECK(!dfs_server_drive_present(), "not present after unmount");
    CHECK(dfs_server_info_string()[0] == 0, "info empty after unmount");
    ax = op_read(id, 0, 4);
    CHECK(ax == 0x15, "read on stale handle after unmount -> 15h, got %04X", ax);
    ax = op_open(AL_OPEN, 0, 0, 0, "\\T1.TXT", &id, NULL);
    CHECK(ax == 0x15, "open after unmount -> 15h, got %04X", ax);
    ax = op_findnext(&s);
    CHECK(ax == 0x15, "findnext after unmount -> 15h, got %04X", ax);
    ax = run_str(DFS_AL_LONGNAME, "\\T1.TXT");
    CHECK(ax == 0x15, "longname after unmount -> 15h, got %04X", ax);
    CHECK(run(DFS_AL_ECHO, "still", 5) == 0 && last_len == 5, "echo after unmount");
    {
        diag_t d;
        op_diag(&d);
        CHECK(d.flags == 0, "diag after unmount: drive absent, flags %02X", d.flags);
        CHECK(d.writes > 0, "diag counters survive the unmount");
    }
    dfs_server_drive_mounted();
    CHECK(dfs_server_drive_present(), "present after remount");
    CHECK(strncmp(dfs_server_info_string(), "PGTEST|FAT32|", 13) == 0, "info back after remount");
    ax = op_read(id, 0, 4);
    CHECK(ax == 6, "old handle after remount -> 6, got %04X", ax);
    ax = op_findnext(&s);
    CHECK(ax == 0x12, "old dir id after remount -> 12h, got %04X", ax);
    ax = op_open(AL_OPEN, 0, 0, 0, "\\T1.TXT", &id, NULL);
    CHECK(ax == 0, "open after remount");
    CHECK(op_close(id) == 0, "close");
    /* long paths are refused, not overflowed */
    {
        char longp[300];
        memset(longp, 'A', sizeof(longp));
        longp[0] = '\\';
        longp[sizeof(longp) - 1] = 0;
        ax = run_str(AL_GETATTR, longp);
        CHECK(ax == 3, "over-long path -> 3, got %04X", ax);
        ax = op_open(AL_OPEN, 0, 0, 0, longp, &id, NULL);
        CHECK(ax == 3, "over-long open path -> 3, got %04X", ax);
    }
}

int main(void) {
    test_setup();
    if (n_fails == 0) {
        test_framing();
        test_diskspace();
        test_dirs();
        test_files();
        test_open();
        test_spopnfil();
        test_find();
        test_attr_delete_rename();
        test_time();
        test_lfn();
        test_longname();
        test_handles();
        test_diag_faults();
        test_unmount();
    }
    f_unmount("");
    ramdisk_free();
    printf("PGDFS server tests: %d checks, %d failures -> %s\n", n_checks, n_fails, n_fails ? "FAIL" : "PASS");
    return n_fails ? 1 : 0;
}
