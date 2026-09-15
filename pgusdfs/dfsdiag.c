/*
 * DFSDIAG - PicoGUS PGDFS transport and protocol test tool
 *
 * Copyright (C) 2026 PicoGUS contributors
 * Distributed under the MIT license, see LICENSE.
 *
 * A plain (non-resident) DOS program that talks to the PicoGUS through the
 * same transport module as the PGDFS TSR (xport.c) and exercises the EDF5
 * requests described in sw/dfs/PROTOCOL.md. It is meant as the protocol
 * conformance check for the firmware, so every failure is reported with the
 * status byte, AX and the lengths involved.
 *
 *   DFSDIAG /INFO            card, protocol, data port, max payload, drive info,
 *                            and the card's disk/FatFs telemetry (DIAG)
 *   DFSDIAG /ECHO [n]        echo 64/512/4096-byte payloads n times, verify
 *   DFSDIAG /DIR [path]      FINDFIRST/FINDNEXT listing
 *   DFSDIAG /LDIR [path]     the same listing with long file names (LONGNAME)
 *   DFSDIAG /TYPE file       READ a file to stdout
 *   DFSDIAG /GET remote local  copy a file from the USB drive
 *   DFSDIAG /PUT local remote  copy a file to the USB drive
 *   DFSDIAG /MKDIR path      create a directory (MKDIR)
 *   DFSDIAG /WRTEST path [n] create, write n bytes, read back, delete: one
 *                            write round trip with every step's AX shown
 *   DFSDIAG /TIME            push the DOS clock to the card
 *
 * After any failed command the tool fetches the DIAG record from the card and
 * prints it, so one screen carries everything needed to see which layer
 * (USB transfer, FatFs, server) a failure came from.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <conio.h>
#include <i86.h>
#include "../common/picogus.h"
#include "xport.h"
#include "version.h"

/* EDF5 subfunctions used here (AL values of the INT 2Fh/11h calls) */
#define AL_MKDIR      0x03
#define AL_CLSFIL     0x06
#define AL_READFIL    0x08
#define AL_WRITEFIL   0x09
#define AL_DISKSPACE  0x0C
#define AL_DELETE     0x13
#define AL_OPEN       0x16
#define AL_CREATE     0x17
#define AL_FINDFIRST  0x1B
#define AL_FINDNEXT   0x1C
#define AL_ECHO       0xF0  /* PGDFS extension (DFS_AL_ECHO in sw/dfs/dfs_server.h) */
#define AL_LONGNAME   0xF1  /* PGDFS extension (DFS_AL_LONGNAME): 8.3 path in, long name out */
#define AL_DIAG       0xF2  /* PGDFS extension (DFS_AL_DIAG): telemetry record out, AX = 0 always */

/* DIAG record layout: a copy of the DFS_DIAG_* table in sw/dfs/dfs_server.h
 * (and sw/dfs/PROTOCOL.md). Little-endian; u16 counters saturate at FFFFh. */
#define DIAG_VERSION       2
#define DIAG_LEN           60
#define DIAG_OFF_VERSION    0  /* u8  */
#define DIAG_OFF_FLAGS      1  /* u8  bit 0: drive present */
#define DIAG_OFF_FSTYPE     2  /* u8  0 none, 1 FAT12, 2 FAT16, 3 FAT32, 4 exFAT */
#define DIAG_OFF_FREECLST   4  /* u32 free clusters as FatFs believes, > n_fatent-2 = unknown */
#define DIAG_OFF_NFATENT    8  /* u32 FAT entries = clusters + 2 */
#define DIAG_OFF_CSIZE     12  /* u16 sectors per cluster */
#define DIAG_OFF_LASTFR    14  /* u8  last non-OK FRESULT of any FatFs call */
#define DIAG_OFF_LASTCALL  15  /* u8  which call */
#define DIAG_OFF_HARDFR    16  /* u8  last FRESULT other than a lookup miss */
#define DIAG_OFF_HARDCALL  17  /* u8  which call */
#define DIAG_OFF_RDRES     18  /* u8  DRESULT of the last disk read */
#define DIAG_OFF_WRRES     19  /* u8  DRESULT of the last disk write */
#define DIAG_OFF_RDCAUSE   20  /* u8  cause of the last disk read's outcome */
#define DIAG_OFF_WRCAUSE   21  /* u8  cause of the last disk write's outcome */
#define DIAG_OFF_CSWSTAT   22  /* u8  CSW status of the last failed USB command */
#define DIAG_OFF_READS     24  /* u32 disk reads */
#define DIAG_OFF_WRITES    28  /* u32 disk writes */
#define DIAG_OFF_RDREFUSED 32  /* u16 READ(10) refused by the USB stack */
#define DIAG_OFF_WRREFUSED 34  /* u16 WRITE(10) refused by the USB stack */
#define DIAG_OFF_RDCSWERR  36  /* u16 READ(10) completed with CSW status != 0 */
#define DIAG_OFF_WRCSWERR  38  /* u16 WRITE(10) completed with CSW status != 0 */
#define DIAG_OFF_TIMEOUTS  40  /* u16 transfers abandoned at the deadline (2 s rd, 10 s wr) */
#define DIAG_OFF_GONE      42  /* u16 transfers abandoned because the drive vanished */
#define DIAG_OFF_WRUS      44  /* u32 microseconds of the last disk write */
#define DIAG_OFF_WRLBA     48  /* u32 first sector of the last disk write */
#define DIAG_OFF_WRCOUNT   52  /* u16 sectors of the last disk write */
#define DIAG_OFF_STALE     54  /* u16 completions of already abandoned transfers, ignored */
#define DIAG_OFF_CSWRESID  56  /* u32 CSW data residue of the last failed USB command */

#define TICKS_PER_SEC 18.2065

static unsigned char buf[XPORT_FRAME_SIZE];
static unsigned short chunk = XPORT_MAX_PAYLOAD; /* largest payload per frame */
static int card_ok;                              /* checkcard() passed: DIAG can be fetched */

static const char *xport_errname(int r) {
  switch (r) {
    case XPORT_OK: return "ok";
    case XPORT_NODRIVE: return "NODRIVE (no USB drive mounted, or no PGDFS)";
    case XPORT_ABORTED: return "ABORTED twice (request rejected by the card)";
    case XPORT_TIMEOUT: return "TIMEOUT (no READY status within 30 s)";
    case XPORT_BADLEN: return "BADLEN (answer header announces an impossible length)";
    case XPORT_TOOLONG: return "TOOLONG (request does not fit the buffer)";
  }
  return "unknown";
}

static const char *statusname(unsigned char st) {
  switch (st) {
    case DFS_STATUS_IDLE: return "IDLE";
    case DFS_STATUS_RECEIVING: return "RECEIVING";
    case DFS_STATUS_BUSY: return "BUSY";
    case DFS_STATUS_READY: return "READY";
    case DFS_STATUS_ABORTED: return "ABORTED";
    case DFS_STATUS_NODRIVE: return "NODRIVE";
  }
  return "?";
}

/* sends one request and reads the answer. the payload is either copied from
 * payload (plen bytes), or, when payload is NULL, expected to be in place at
 * buf+4 already. returns the answer payload length (>= 0) with *ax set to
 * the DOS result, or -1 on transport failure (already reported). */
static int query(unsigned char al, const void *payload, unsigned short plen, unsigned short *ax) {
  unsigned short len = plen + DFS_HDR_LEN;
  int r;
  if (len > sizeof(buf)) {
    printf("ERROR: request of %u bytes does not fit the %u-byte frame buffer\n", len, (unsigned)sizeof(buf));
    return -1;
  }
  buf[0] = len & 0xFF;
  buf[1] = len >> 8;
  buf[2] = 0;   /* drive index 0 = the USB drive, no flags */
  buf[3] = al;
  if ((payload != NULL) && (plen != 0)) memcpy(buf + DFS_HDR_LEN, payload, plen);
  r = xport_transact(buf, sizeof(buf));
  if (r != XPORT_OK) {
    printf("ERROR: AL=%02Xh request (%u bytes): %s, last status byte %02Xh (%s)%s\n",
           al, len, xport_errname(r), xport_last_status, statusname(xport_last_status),
           xport_retried ? ", re-sent once" : "");
    return -1;
  }
  *ax = buf[2] | (buf[3] << 8);
  len = buf[0] | (buf[1] << 8);
  return len - DFS_HDR_LEN;
}

/* like query(), but also treats AX != 0 as failure and reports it */
static int query_ok(unsigned char al, const void *payload, unsigned short plen, const char *what) {
  unsigned short ax;
  int len = query(al, payload, plen, &ax);
  if (len < 0) return -1;
  if (ax != 0) {
    printf("ERROR: %s: AL=%02Xh answered AX=%04Xh (DOS error %u), %d payload bytes\n", what, al, ax, ax, len);
    return -1;
  }
  return len;
}

/* card presence, protocol version, CMD_DFSMAXLEN and CMD_DFSPORT checks
 * shared by every command. returns 0 when PGDFS is usable; then the data
 * window base is in xport_data_port and the payload size in chunk. */
static int checkcard(int verbose) {
  unsigned char proto, st;
  unsigned short maxlen, port;
  xport_detect_cpu();
  if (verbose) printf("CPU: %s\n", xport_cpu186 ? "80186 or later (using REP INSW/OUTSW)" : "8086/8088 (using IN AX,DX / OUT DX,AX loops)");
  if (!xport_present()) {
    printf("ERROR: PicoGUS not detected (CMD_MAGIC on port %03Xh/%03Xh did not answer DDh)\n", CONTROL_PORT, DATA_PORT_HIGH);
    return 1;
  }
  proto = xport_read8(CMD_PROTOCOL);
  maxlen = xport_read16(CMD_DFSMAXLEN);
  port = xport_read16(CMD_DFSPORT);
  st = xport_status();
  if (verbose) {
    printf("PicoGUS detected, protocol version %u (need >= %u)\n", proto, PICOGUS_PROTOCOL_VER);
    printf("CMD_DFSMAXLEN: %u (%04Xh)\n", maxlen, maxlen);
    printf("CMD_DFSPORT:   %04Xh%s\n", port, (port == 0) ? " (PGDFS disabled)" : "");
    printf("CMD_DFSSTAT:   %02Xh (%s)\n", st, statusname(st));
  }
  if (proto < PICOGUS_PROTOCOL_VER) {
    printf("ERROR: firmware protocol %u is too old for PGDFS (need %u)\n", proto, PICOGUS_PROTOCOL_VER);
    return 1;
  }
  /* PGDFS switched off (pgusinit /dfsport 0) reports both registers as 0 */
  if ((port == 0) && (maxlen == 0)) {
    printf("ERROR: PGDFS is disabled on this PicoGUS (enable it with pgusinit /dfsport 1D4)\n");
    return 1;
  }
  if ((maxlen < XPORT_MAXLEN_MIN) || (maxlen > XPORT_MAXLEN_MAX)) {
    printf("ERROR: CMD_DFSMAXLEN=%u is outside %u..%u: this firmware has no PGDFS support\n", maxlen, XPORT_MAXLEN_MIN, XPORT_MAXLEN_MAX);
    return 1;
  }
  if (port == 0) {
    printf("ERROR: PGDFS is disabled on this PicoGUS (enable it with pgusinit /dfsport 1D4)\n");
    return 1;
  }
  port &= 0xFFFEu; /* even base, the card ignores bit 0 */
  if ((port < 0x100) || (port > 0x3FE)) {
    printf("ERROR: CMD_DFSPORT=%04Xh is not a usable data port: PGDFS firmware older than this tool?\n", port);
    return 1;
  }
  xport_data_port = port;
  if (maxlen < chunk) chunk = maxlen;
  if (verbose) {
    printf("Data port:     %03Xh-%03Xh (word transfers, low byte from %03Xh)\n", port, port + 1, port);
    printf("Frame payload used by this tool: %u bytes\n", chunk);
  }
  card_ok = 1;
  return 0;
}

/* ---- DIAG record ------------------------------------------------------- */

static unsigned short rd16(const unsigned char *p) {
  return p[0] | (p[1] << 8);
}

static unsigned long rd32(const unsigned char *p) {
  return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* FatFs FRESULT names (ff.h order) */
static const char *frname(unsigned char fr) {
  static const char *names[] = {
    "FR_OK", "FR_DISK_ERR", "FR_INT_ERR", "FR_NOT_READY", "FR_NO_FILE", "FR_NO_PATH",
    "FR_INVALID_NAME", "FR_DENIED", "FR_EXIST", "FR_INVALID_OBJECT", "FR_WRITE_PROTECTED",
    "FR_INVALID_DRIVE", "FR_NOT_ENABLED", "FR_NO_FILESYSTEM", "FR_MKFS_ABORTED", "FR_TIMEOUT",
    "FR_LOCKED", "FR_NOT_ENOUGH_CORE", "FR_TOO_MANY_OPEN_FILES", "FR_INVALID_PARAMETER"
  };
  return (fr < sizeof(names) / sizeof(names[0])) ? names[fr] : "?";
}

/* DFS_CALL_* ids (sw/dfs/dfs_fs.h) */
static const char *callname(unsigned char c) {
  static const char *names[] = {
    "none", "f_open", "f_close", "f_lseek", "f_read", "f_write", "f_truncate", "f_sync",
    "f_stat", "f_chmod", "f_utime", "f_mkdir", "f_unlink", "f_rename", "f_opendir",
    "f_readdir", "f_closedir", "f_getfree", "f_getlabel"
  };
  return (c < sizeof(names) / sizeof(names[0])) ? names[c] : "?";
}

/* FatFs DRESULT */
static const char *dresname(unsigned char r) {
  static const char *names[] = { "RES_OK", "RES_ERROR", "RES_WRPRT", "RES_NOTRDY", "RES_PARERR" };
  return (r < sizeof(names) / sizeof(names[0])) ? names[r] : "?";
}

/* msc_io_cause_t (sw/usb_msc/msc_app.h) */
static const char *causename(unsigned char c) {
  static const char *names[] = {
    "completed", "no drive mounted", "refused by the USB stack (endpoint busy/not mounted)",
    "drive answered CSW status != 0", "no completion within the deadline (timeout)", "drive vanished during the transfer"
  };
  return (c < sizeof(names) / sizeof(names[0])) ? names[c] : "?";
}

static const char *fsname(unsigned char t) {
  static const char *names[] = { "none", "FAT12", "FAT16", "FAT32", "exFAT" };
  return (t < sizeof(names) / sizeof(names[0])) ? names[t] : "?";
}

/* fetches the DIAG record and prints it, one line per item. returns 0 when
 * the record was shown, 1 when the card could not deliver it (old firmware
 * answers AX=1 "invalid function" for the unknown AL) */
static int printdiag(void) {
  unsigned short ax;
  unsigned char r[DIAG_LEN];
  unsigned long freeclst, nfatent;
  int len = query(AL_DIAG, NULL, 0, &ax);
  if (len < 0) return 1;
  if ((ax != 0) || (len < DIAG_LEN)) {
    printf("DIAG: not available (AX=%04Xh, %d payload bytes): firmware without the DIAG request?\n", ax, len);
    return 1;
  }
  memcpy(r, buf + DFS_HDR_LEN, DIAG_LEN);
  if (r[DIAG_OFF_VERSION] != DIAG_VERSION) {
    printf("DIAG: record version %u, this tool knows version %u; showing what it can\n", r[DIAG_OFF_VERSION], DIAG_VERSION);
  }
  freeclst = rd32(r + DIAG_OFF_FREECLST);
  nfatent = rd32(r + DIAG_OFF_NFATENT);
  printf("--- card diagnostics (DIAG v%u) ---\n", r[DIAG_OFF_VERSION]);
  printf("Drive present (server):  %s\n", (r[DIAG_OFF_FLAGS] & 1) ? "yes" : "no");
  printf("FatFs volume:            %s, %lu clusters of %u sectors\n",
         fsname(r[DIAG_OFF_FSTYPE]), (nfatent >= 2) ? nfatent - 2 : 0, rd16(r + DIAG_OFF_CSIZE));
  if ((nfatent >= 2) && (freeclst <= nfatent - 2)) {
    printf("Free clusters (FatFs):   %lu\n", freeclst);
  } else {
    printf("Free clusters (FatFs):   unknown (FAT not scanned yet; the first DISKSPACE does it)\n");
  }
  printf("Last FatFs error:        %s (%u) from %s (%u)\n",
         frname(r[DIAG_OFF_LASTFR]), r[DIAG_OFF_LASTFR], callname(r[DIAG_OFF_LASTCALL]), r[DIAG_OFF_LASTCALL]);
  printf("Last hard FatFs error:   %s (%u) from %s (%u)\n",
         frname(r[DIAG_OFF_HARDFR]), r[DIAG_OFF_HARDFR], callname(r[DIAG_OFF_HARDCALL]), r[DIAG_OFF_HARDCALL]);
  printf("Disk reads:              %lu (refused %u, CSW errors %u), last %s: %s\n",
         rd32(r + DIAG_OFF_READS), rd16(r + DIAG_OFF_RDREFUSED), rd16(r + DIAG_OFF_RDCSWERR),
         dresname(r[DIAG_OFF_RDRES]), causename(r[DIAG_OFF_RDCAUSE]));
  printf("Disk writes:             %lu (refused %u, CSW errors %u), last %s: %s\n",
         rd32(r + DIAG_OFF_WRITES), rd16(r + DIAG_OFF_WRREFUSED), rd16(r + DIAG_OFF_WRCSWERR),
         dresname(r[DIAG_OFF_WRRES]), causename(r[DIAG_OFF_WRCAUSE]));
  printf("Last disk write:         %u sector(s) at LBA %lu, took %lu us\n",
         rd16(r + DIAG_OFF_WRCOUNT), rd32(r + DIAG_OFF_WRLBA), rd32(r + DIAG_OFF_WRUS));
  printf("Timeouts / drive gone:   %u / %u, stale completions %u\n",
         rd16(r + DIAG_OFF_TIMEOUTS), rd16(r + DIAG_OFF_GONE), rd16(r + DIAG_OFF_STALE));
  printf("Last failed USB command: CSW status %u, data residue %lu bytes\n",
         r[DIAG_OFF_CSWSTAT], rd32(r + DIAG_OFF_CSWRESID));
  return 0;
}

/* called by main() when a command failed: one screen with everything */
static void diag_after_failure(void) {
  if (!card_ok) return;
  printf("\nCard state after the failure:\n");
  printdiag();
}

static void printinfo(void) {
  char info[256];
  char *p, *label, *fs, *mb, *serial;
  if (xport_info(info, sizeof(info)) == 0) {
    printf("USB drive:     none mounted (CMD_DFSINFO is empty)\n");
    return;
  }
  printf("CMD_DFSINFO:   \"%s\"\n", info);
  label = info;
  fs = mb = serial = "";
  p = strchr(info, '|');
  if (p != NULL) { *p++ = 0; fs = p; p = strchr(p, '|'); }
  if (p != NULL) { *p++ = 0; mb = p; p = strchr(p, '|'); }
  if (p != NULL) { *p++ = 0; serial = p; }
  printf("USB drive:     label \"%s\", %s, %s MB, serial %s\n", label, fs, mb, serial);
}

static int cmd_info(void) {
  unsigned short ax;
  int len;
  printf("DFSDIAG v%s - PicoGUS PGDFS test tool\n", PVER);
  if (checkcard(1) != 0) return 1;
  printinfo();
  /* a DISKSPACE query is the cheapest end-to-end check */
  len = query(AL_DISKSPACE, NULL, 0, &ax);
  if (len < 0) return 1;
  if (len == 6) {
    unsigned short bx = buf[4] | (buf[5] << 8), cx = buf[6] | (buf[7] << 8), dx = buf[8] | (buf[9] << 8);
    printf("DISKSPACE:     %u sectors/cluster, %u bytes/sector, %u total clusters, %u free\n", ax, cx, bx, dx);
    printf("               %lu KB total, %lu KB free\n",
           ((unsigned long)bx * cx / 1024) * ax, ((unsigned long)dx * cx / 1024) * ax);
  } else {
    printf("WARNING: DISKSPACE answered %d payload bytes (expected 6), AX=%04Xh\n", len, ax);
  }
  printdiag();
  printf("PGDFS ready.\n");
  return 0;
}

static int cmd_echo(int rounds) {
  static const unsigned short sizes[3] = {64, 512, 4096};
  int s, r, len, fails = 0;
  unsigned short ax, size, i;
  unsigned short t0, t1;
  if (checkcard(0) != 0) return 1;
  for (s = 0; s < 3; s++) {
    size = sizes[s];
    if (size > chunk) size = chunk;
    printf("ECHO %4u bytes x %d: ", size, rounds);
    fflush(stdout);
    t0 = xport_ticks();
    for (r = 0; r < rounds; r++) {
      unsigned char seed = (unsigned char)(r * 31 + s * 7 + 1);
      for (i = 0; i < size; i++) buf[DFS_HDR_LEN + i] = (unsigned char)(i * 7 + seed);
      len = query(AL_ECHO, NULL, size, &ax);
      if (len < 0) { fails++; break; }
      if ((len != size) || (ax != 0)) {
        printf("\n  round %d: answer has %d payload bytes (sent %u), AX=%04Xh\n", r, len, size, ax);
        fails++;
        break;
      }
      for (i = 0; i < size; i++) {
        if (buf[DFS_HDR_LEN + i] != (unsigned char)(i * 7 + seed)) {
          printf("\n  round %d: data mismatch at offset %u: got %02Xh, expected %02Xh\n",
                 r, i, buf[DFS_HDR_LEN + i], (unsigned char)(i * 7 + seed));
          fails++;
          break;
        }
      }
      if (i != size) break;
    }
    t1 = xport_ticks();
    if (r == rounds) {
      double secs = (unsigned short)(t1 - t0) / TICKS_PER_SEC;
      double kb = (double)size * rounds / 1024.0;
      if (secs < 0.001) secs = 0.001;
      printf("ok, %.1f KB each way in %.2f s = %.1f KB/s (%.1f KB/s counting both directions)\n",
             kb, secs, kb / secs, 2 * kb / secs);
    }
  }
  if (fails) {
    printf("ECHO test FAILED (%d failure%s)\n", fails, fails == 1 ? "" : "s");
    return 1;
  }
  printf("ECHO test passed.\n");
  return 0;
}

/* normalizes a remote path: strips an optional drive letter, forces a
 * leading backslash, converts / to \ and upper-cases everything */
static void normpath(char *dst, const char *src, size_t max) {
  size_t n = 0;
  if ((src[0] != 0) && (src[1] == ':')) src += 2;
  if (*src != '\\' && *src != '/') dst[n++] = '\\';
  while ((*src != 0) && (n + 1 < max)) {
    char c = *src++;
    if (c == '/') c = '\\';
    dst[n++] = (char)toupper((unsigned char)c);
  }
  dst[n] = 0;
}

static void fcb2name(const unsigned char *fcb, char *name) {
  int i, n = 0;
  for (i = 0; i < 8 && fcb[i] != ' '; i++) name[n++] = fcb[i];
  if (fcb[8] != ' ') {
    name[n++] = '.';
    for (i = 8; i < 11 && fcb[i] != ' '; i++) name[n++] = fcb[i];
  }
  name[n] = 0;
}

static void printentry(const unsigned char *e) {
  /* e -> AfffffffffffttddssssCCpp (24 bytes) */
  char name[13];
  unsigned char attr = e[0];
  unsigned short tm = e[12] | (e[13] << 8), dt = e[14] | (e[15] << 8);
  unsigned long size = (unsigned long)e[16] | ((unsigned long)e[17] << 8) | ((unsigned long)e[18] << 16) | ((unsigned long)e[19] << 24);
  fcb2name(e + 1, name);
  printf("%-12s %c%c%c%c%c ", name,
         (attr & 0x10) ? 'D' : '-', (attr & 0x01) ? 'R' : '-', (attr & 0x02) ? 'H' : '-',
         (attr & 0x04) ? 'S' : '-', (attr & 0x20) ? 'A' : '-');
  if (attr & 0x10) printf("     <DIR> "); else printf("%10lu ", size);
  printf("%04u-%02u-%02u %02u:%02u:%02u\n",
         (dt >> 9) + 1980, (dt >> 5) & 15, dt & 31, tm >> 11, (tm >> 5) & 63, (tm & 31) * 2);
}

/* prints one FIND entry in the /LDIR format: short name, size, date, time
 * and the long name the card returns for \DIR\SHORT.EXT (DFS_AL_LONGNAME).
 * e is a private copy of the 24-byte entry, since the query reuses the
 * frame buffer. The long name bytes are passed to the screen exactly as
 * the card returns them (FatFs code page). Errors are shown per entry. */
static void printlongentry(const char *dir, const unsigned char *e) {
  char name[13], full[160];
  unsigned char attr = e[0];
  unsigned short tm = e[12] | (e[13] << 8), dt = e[14] | (e[15] << 8), ax = 0;
  unsigned long size = (unsigned long)e[16] | ((unsigned long)e[17] << 8) | ((unsigned long)e[18] << 16) | ((unsigned long)e[19] << 24);
  int lnlen = -1, asked = 0;
  fcb2name(e + 1, name);
  /* "." and ".." have no long name; anything else is asked for by the 8.3
   * path DOS would use, e.g. \DIR\LONGNA~1.TXT */
  if ((strcmp(name, ".") != 0) && (strcmp(name, "..") != 0)) {
    asked = 1;
    sprintf(full, "%s\\%s", (strcmp(dir, "\\") == 0) ? "" : dir, name);
    memcpy(buf + DFS_HDR_LEN, full, strlen(full));
    lnlen = query(AL_LONGNAME, NULL, strlen(full), &ax); /* -1: reported already */
  }
  printf("%-12s ", name);
  if (attr & 0x10) printf("     <DIR> "); else printf("%10lu ", size);
  printf("%04u-%02u-%02u %02u:%02u  ",
         (dt >> 9) + 1980, (dt >> 5) & 15, dt & 31, tm >> 11, (tm >> 5) & 63);
  if (!asked) {
    /* nothing to show */
  } else if (lnlen < 0) {
    printf("(LONGNAME request failed, see above)");
  } else if (ax != 0) {
    printf("(LONGNAME error: AX=%04Xh, DOS error %u)", ax, ax);
  } else {
    fwrite(buf + DFS_HDR_LEN, 1, lnlen, stdout);
  }
  printf("\n");
}

/* /DIR (longnames == 0) and /LDIR (longnames != 0): walk a directory with
 * FINDFIRST/FINDNEXT, attribute mask 16h (files, directories, hidden,
 * system) like DOS DIR does */
static int cmd_dir(const char *path, int longnames) {
  char dir[128], mask[160];
  unsigned char req[32], entry[24];
  unsigned short ax, dirid, pos;
  int len, n = 0;
  size_t l;
  if (checkcard(0) != 0) return 1;
  normpath(dir, (path != NULL) ? path : "\\", sizeof(dir));
  l = strlen(dir);
  if ((l > 1) && (dir[l - 1] == '\\')) dir[l - 1] = 0;
  sprintf(mask, "%s\\????????.???", (strcmp(dir, "\\") == 0) ? "" : dir);
  printf("Directory of %s%s\n\n", dir, longnames ? " (with long names)" : "");
  /* FINDFIRST: A + path with mask */
  req[0] = 0x16; /* look for hidden, system and directories, too */
  memcpy(buf + DFS_HDR_LEN, req, 1);
  memcpy(buf + DFS_HDR_LEN + 1, mask, strlen(mask));
  len = query(AL_FINDFIRST, NULL, 1 + strlen(mask), &ax);
  if (len < 0) return 1;
  if (ax != 0) {
    if (ax == 0x12) {
      printf("(empty)\n");
      return 0;
    }
    printf("ERROR: FINDFIRST \"%s\" answered AX=%04Xh (DOS error %u), %d payload bytes\n", mask, ax, ax, len);
    return 1;
  }
  for (;;) {
    if (len != 24) {
      printf("ERROR: FIND answer has %d payload bytes, expected 24\n", len);
      return 1;
    }
    /* keep a copy of the entry: the LONGNAME query overwrites the buffer */
    memcpy(entry, buf + DFS_HDR_LEN, 24);
    dirid = entry[20] | (entry[21] << 8);
    pos = entry[22] | (entry[23] << 8);
    if (longnames) printlongentry(dir, entry); else printentry(entry);
    n++;
    /* FINDNEXT: CC pp A + 11-byte template */
    req[0] = dirid & 0xFF; req[1] = dirid >> 8;
    req[2] = pos & 0xFF;   req[3] = pos >> 8;
    req[4] = 0x16;
    memset(req + 5, '?', 11);
    len = query(AL_FINDNEXT, req, 16, &ax);
    if (len < 0) return 1;
    if (ax != 0) {
      if (ax == 0x12) break; /* no more files */
      printf("ERROR: FINDNEXT (dir %04Xh, pos %u) answered AX=%04Xh (DOS error %u)\n", dirid, pos, ax, ax);
      return 1;
    }
  }
  printf("\n%d entries\n", n);
  return 0;
}

/* OPEN (or CREATE) a remote file; returns the 16-bit file id, or -1 */
static long openremote(const char *rpath, int create, unsigned long *size) {
  unsigned char req[6];
  char path[128];
  int len;
  normpath(path, rpath, sizeof(path));
  /* SS = stack word (attributes), CC = action, MM = mode, then the path */
  req[0] = 0; req[1] = 0;                     /* attributes: normal file */
  req[2] = create ? 0x12 : 0x01; req[3] = 0; /* action: create/truncate or open existing */
  req[4] = create ? 0x02 : 0x00; req[5] = 0; /* mode: read/write or read-only */
  memcpy(buf + DFS_HDR_LEN, req, 6);
  memcpy(buf + DFS_HDR_LEN + 6, path, strlen(path));
  len = query_ok(create ? AL_CREATE : AL_OPEN, NULL, 6 + strlen(path), create ? "CREATE" : "OPEN");
  if (len < 0) return -1;
  if (len != 25) {
    printf("ERROR: OPEN/CREATE answer has %d payload bytes, expected 25\n", len);
    return -1;
  }
  if (size != NULL) {
    *size = (unsigned long)buf[DFS_HDR_LEN + 16] | ((unsigned long)buf[DFS_HDR_LEN + 17] << 8) |
            ((unsigned long)buf[DFS_HDR_LEN + 18] << 16) | ((unsigned long)buf[DFS_HDR_LEN + 19] << 24);
  }
  return buf[DFS_HDR_LEN + 20] | (buf[DFS_HDR_LEN + 21] << 8);
}

static int closeremote(unsigned short id) {
  unsigned char req[2];
  req[0] = id & 0xFF; req[1] = id >> 8;
  return (query_ok(AL_CLSFIL, req, 2, "CLOSE") < 0) ? -1 : 0;
}

/* READ from offset; returns bytes read (in buf+4), or -1 */
static int readremote(unsigned short id, unsigned long offset, unsigned short want) {
  unsigned char req[8];
  int len;
  req[0] = offset & 0xFF; req[1] = (offset >> 8) & 0xFF; req[2] = (offset >> 16) & 0xFF; req[3] = (offset >> 24) & 0xFF;
  req[4] = id & 0xFF; req[5] = id >> 8;
  req[6] = want & 0xFF; req[7] = want >> 8;
  len = query_ok(AL_READFIL, req, 8, "READ");
  if (len < 0) return -1;
  if (len > want) {
    printf("ERROR: READ of %u bytes at offset %lu answered %d bytes\n", want, offset, len);
    return -1;
  }
  return len;
}

/* copies a remote file to out (stdout or a file). returns 0 on success */
static int dumpremote(const char *rpath, FILE *out) {
  long id;
  unsigned long size, offset = 0;
  int len;
  id = openremote(rpath, 0, &size);
  if (id < 0) return 1;
  for (;;) {
    len = readremote((unsigned short)id, offset, chunk);
    if (len < 0) { closeremote((unsigned short)id); return 1; }
    if (len == 0) break;
    if (fwrite(buf + DFS_HDR_LEN, 1, len, out) != (size_t)len) {
      printf("ERROR: local write failed\n");
      closeremote((unsigned short)id);
      return 1;
    }
    offset += len;
    if (len < chunk) break; /* short read = EOF */
  }
  if (closeremote((unsigned short)id) != 0) return 1;
  if (out != stdout) printf("%lu bytes copied (remote size %lu)\n", offset, size);
  if (offset != size) printf("WARNING: read %lu bytes but OPEN reported a size of %lu\n", offset, size);
  return 0;
}

static int cmd_type(const char *rpath) {
  if (checkcard(0) != 0) return 1;
  return dumpremote(rpath, stdout);
}

static int cmd_get(const char *rpath, const char *lpath) {
  FILE *f;
  int r;
  if (checkcard(0) != 0) return 1;
  f = fopen(lpath, "wb");
  if (f == NULL) {
    printf("ERROR: cannot create local file %s\n", lpath);
    return 1;
  }
  r = dumpremote(rpath, f);
  fclose(f);
  return r;
}

static int cmd_put(const char *lpath, const char *rpath) {
  FILE *f;
  long id;
  unsigned long offset = 0;
  unsigned short wchunk = chunk - 6;
  size_t n;
  int len;
  if (checkcard(0) != 0) return 1;
  wchunk = chunk - 6;
  f = fopen(lpath, "rb");
  if (f == NULL) {
    printf("ERROR: cannot open local file %s\n", lpath);
    return 1;
  }
  id = openremote(rpath, 1, NULL);
  if (id < 0) { fclose(f); return 1; }
  for (;;) {
    unsigned short written;
    n = fread(buf + DFS_HDR_LEN + 6, 1, wchunk, f);
    if (n == 0) break;
    /* OOOO SS data... */
    buf[DFS_HDR_LEN + 0] = offset & 0xFF; buf[DFS_HDR_LEN + 1] = (offset >> 8) & 0xFF;
    buf[DFS_HDR_LEN + 2] = (offset >> 16) & 0xFF; buf[DFS_HDR_LEN + 3] = (offset >> 24) & 0xFF;
    buf[DFS_HDR_LEN + 4] = id & 0xFF; buf[DFS_HDR_LEN + 5] = (id >> 8) & 0xFF;
    len = query_ok(AL_WRITEFIL, NULL, 6 + n, "WRITE");
    if (len < 0) { fclose(f); closeremote((unsigned short)id); return 1; }
    if (len != 2) {
      printf("ERROR: WRITE answer has %d payload bytes, expected 2\n", len);
      fclose(f); closeremote((unsigned short)id); return 1;
    }
    written = buf[DFS_HDR_LEN] | (buf[DFS_HDR_LEN + 1] << 8);
    if (written != n) {
      printf("ERROR: WRITE of %u bytes at offset %lu wrote only %u\n", (unsigned)n, offset, written);
      fclose(f); closeremote((unsigned short)id); return 1;
    }
    offset += written;
    if (n < wchunk) break;
  }
  fclose(f);
  if (closeremote((unsigned short)id) != 0) return 1;
  printf("%lu bytes copied\n", offset);
  return 0;
}

/* one WRITE of n bytes at offset; returns the count the card reports, or -1 */
static long writeremote(unsigned short id, unsigned long offset, unsigned short n) {
  int len;
  buf[DFS_HDR_LEN + 0] = offset & 0xFF; buf[DFS_HDR_LEN + 1] = (offset >> 8) & 0xFF;
  buf[DFS_HDR_LEN + 2] = (offset >> 16) & 0xFF; buf[DFS_HDR_LEN + 3] = (offset >> 24) & 0xFF;
  buf[DFS_HDR_LEN + 4] = id & 0xFF; buf[DFS_HDR_LEN + 5] = (id >> 8) & 0xFF;
  len = query_ok(AL_WRITEFIL, NULL, 6 + n, "WRITE");
  if (len < 0) return -1;
  if (len != 2) {
    printf("ERROR: WRITE answer has %d payload bytes, expected 2\n", len);
    return -1;
  }
  return buf[DFS_HDR_LEN] | (buf[DFS_HDR_LEN + 1] << 8);
}

static int deleteremote(const char *rpath) {
  char path[128];
  normpath(path, rpath, sizeof(path));
  memcpy(buf + DFS_HDR_LEN, path, strlen(path));
  return (query_ok(AL_DELETE, NULL, strlen(path), "DELETE") < 0) ? -1 : 0;
}

static int cmd_mkdir(const char *rpath) {
  char path[128];
  if (checkcard(0) != 0) return 1;
  normpath(path, rpath, sizeof(path));
  memcpy(buf + DFS_HDR_LEN, path, strlen(path));
  if (query_ok(AL_MKDIR, NULL, strlen(path), "MKDIR") < 0) return 1;
  printf("MKDIR %s: AX=0000h (ok)\n", path);
  return 0;
}

/* the write round trip in isolation: CREATE, one WRITE of n bytes (which the
 * card f_sync()s: data sector, directory entry and FAT go to the drive),
 * READ back and compare, CLOSE, DELETE. every step prints its result so a
 * failure can be placed without the TSR in the picture. */
static int cmd_wrtest(const char *rpath, unsigned short n) {
  long id, written;
  unsigned short i;
  int len, fails = 0;
  if (checkcard(0) != 0) return 1;
  if (n == 0) n = 512;
  if (n > chunk - 6) n = chunk - 6;
  printf("Write test on %s with %u bytes\n", rpath, n);
  id = openremote(rpath, 1, NULL);
  if (id < 0) return 1;
  printf("  CREATE: AX=0000h (ok), file id %ld\n", id);
  for (i = 0; i < n; i++) buf[DFS_HDR_LEN + 6 + i] = (unsigned char)(i * 5 + 3);
  written = writeremote((unsigned short)id, 0, n);
  if (written < 0) {
    fails++;
  } else if (written != n) {
    printf("  WRITE: AX=0000h but only %ld of %u bytes written (disk full?)\n", written, n);
    fails++;
  } else {
    printf("  WRITE: AX=0000h (ok), %ld bytes\n", written);
  }
  if (fails == 0) {
    len = readremote((unsigned short)id, 0, n);
    if (len < 0) {
      fails++;
    } else if (len != n) {
      printf("  READ: AX=0000h but %d of %u bytes came back\n", len, n);
      fails++;
    } else {
      for (i = 0; i < n; i++) {
        if (buf[DFS_HDR_LEN + i] != (unsigned char)(i * 5 + 3)) break;
      }
      if (i != n) {
        printf("  READ: AX=0000h, %d bytes, but data differs at offset %u (got %02Xh, expected %02Xh)\n",
               len, i, buf[DFS_HDR_LEN + i], (unsigned char)(i * 5 + 3));
        fails++;
      } else {
        printf("  READ: AX=0000h (ok), %d bytes, data verified\n", len);
      }
    }
  }
  if (closeremote((unsigned short)id) != 0) fails++; else printf("  CLOSE: AX=0000h (ok)\n");
  if (deleteremote(rpath) != 0) fails++; else printf("  DELETE: AX=0000h (ok)\n");
  if (fails) {
    printf("Write test FAILED (%d failure%s)\n", fails, fails == 1 ? "" : "s");
    return 1;
  }
  printf("Write test passed.\n");
  return 0;
}

static int cmd_time(void) {
  unsigned short t, d;
  if (checkcard(0) != 0) return 1;
  xport_getdostime(&t, &d);
  xport_settime(t, d);
  printf("Sent DOS time %04Xh (%02u:%02u:%02u) and date %04Xh (%04u-%02u-%02u) to the card\n",
         t, t >> 11, (t >> 5) & 63, (t & 31) * 2, d, (d >> 9) + 1980, (d >> 5) & 15, d & 31);
  return 0;
}

static void usage(void) {
  printf("DFSDIAG v%s - PicoGUS PGDFS transport and protocol test tool\n\n", PVER);
  printf("  DFSDIAG /INFO              card, protocol, data port, frame size, USB drive\n");
  printf("  DFSDIAG /ECHO [n]          echo 64/512/4096-byte payloads n times (default 10)\n");
  printf("  DFSDIAG /DIR [path]        list a directory (FINDFIRST/FINDNEXT)\n");
  printf("  DFSDIAG /LDIR [path]       list a directory with long file names (LONGNAME)\n");
  printf("  DFSDIAG /TYPE file         show a file (READ)\n");
  printf("  DFSDIAG /GET remote local  copy a file from the USB drive\n");
  printf("  DFSDIAG /PUT local remote  copy a file to the USB drive\n");
  printf("  DFSDIAG /MKDIR path        create a directory (MKDIR)\n");
  printf("  DFSDIAG /WRTEST path [n]   create, write n bytes (512), read back, delete\n");
  printf("  DFSDIAG /TIME              push the DOS clock to the card\n\n");
  printf("Remote paths are relative to the root of the USB drive, e.g. \\DIR\\FILE.TXT\n");
  printf("/INFO shows the card's disk and FatFs telemetry; a failed command prints it too.\n");
}

/* runs the command; -1 = unknown command or missing arguments */
static int dispatch(const char *cmd, int argc, char **argv) {
  if (stricmp(cmd, "INFO") == 0) return cmd_info();
  if (stricmp(cmd, "ECHO") == 0) {
    int n = (argc > 2) ? atoi(argv[2]) : 10;
    if (n < 1) n = 1;
    return cmd_echo(n);
  }
  if (stricmp(cmd, "DIR") == 0) return cmd_dir((argc > 2) ? argv[2] : NULL, 0);
  if (stricmp(cmd, "LDIR") == 0) return cmd_dir((argc > 2) ? argv[2] : NULL, 1);
  if ((stricmp(cmd, "TYPE") == 0) && (argc > 2)) return cmd_type(argv[2]);
  if ((stricmp(cmd, "GET") == 0) && (argc > 3)) return cmd_get(argv[2], argv[3]);
  if ((stricmp(cmd, "PUT") == 0) && (argc > 3)) return cmd_put(argv[2], argv[3]);
  if ((stricmp(cmd, "MKDIR") == 0) && (argc > 2)) return cmd_mkdir(argv[2]);
  if ((stricmp(cmd, "WRTEST") == 0) && (argc > 2)) {
    int n = (argc > 3) ? atoi(argv[3]) : 512;
    if (n < 1) n = 512;
    if (n > 4090) n = 4090;
    return cmd_wrtest(argv[2], (unsigned short)n);
  }
  if (stricmp(cmd, "TIME") == 0) return cmd_time();
  return -1;
}

int main(int argc, char **argv) {
  const char *cmd;
  int r;
  if (argc < 2) {
    usage();
    return 1;
  }
  cmd = argv[1];
  if ((cmd[0] == '/') || (cmd[0] == '-')) cmd++;
  r = dispatch(cmd, argc, argv);
  if (r < 0) {
    usage();
    return 1;
  }
  /* a failed command: add the card's view so one screen tells the story
   * (/INFO prints it as part of its normal output already) */
  if ((r != 0) && (stricmp(cmd, "INFO") != 0)) diag_after_failure();
  return r;
}
