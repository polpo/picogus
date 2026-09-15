/*
 * PGDFS - the USB drive of a PicoGUS ISA card as a DOS drive letter
 *
 * Derived from EtherDFS v0.8.2 - a network drive for DOS running over raw
 * ethernet, http://etherdfs.sourceforge.net
 *
 * Copyright (C) 2017, 2018 Mateusz Viste
 * Copyright (C) 2026 PicoGUS contributors (PicoGUS PGDFS modifications)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Modified for PicoGUS PGDFS (2026): the Ethernet packet driver transport
 * (packet driver init, receive callback, MAC addresses, checksums, sequence
 * numbers, retries) is replaced by the PicoGUS ISA I/O port transport in
 * xport.c, which is #included into the resident segment below. Frames use
 * the 4-byte PGDFS header instead of the 60-byte Ethernet/EDF5 one, the
 * payload is up to 4096 bytes, and one buffer holds both the request and
 * the answer. The DOS side (INT 2F handling, SDA/CDS/SFT usage, stack
 * switching, TSR mechanics) is kept as it was, see MEMNOTES.TXT in the
 * EtherDFS sources for the rules the resident part follows.
 */

#include <i86.h>     /* union INTPACK */
#include "chint.h"   /* _mvchain_intr() */
#include "version.h" /* program version */
#include "../common/picogus.h" /* PicoGUS ports, registers, status values */
#include "xport.h"   /* the PicoGUS transport */

/* set DEBUGLEVEL to 0, 1 or 2 to turn on debug mode with desired verbosity */
#define DEBUGLEVEL 0

/* define the maximum size of a frame, as sent or received by pgdfs: the
 * 4-byte header plus the largest payload the driver uses (4096 bytes) */
#define FRAMESIZE XPORT_FRAME_SIZE

#include "dosstruc.h" /* definitions of structures used by DOS */
#include "globals.h"  /* global variables used by pgdfs */

/* define NULL, for readability of the code */
#ifndef NULL
  #define NULL (void *)0
#endif

/* the DOS error reported when the card gives no usable answer (no USB drive
 * mounted, timeout, request rejected twice): 15h = "drive not ready" */
#define ERR_NOTREADY 0x15

/* all the resident code goes to segment 'BEGTEXT' */
#pragma code_seg(BEGTEXT, CODE)


/* copies l bytes from *s to *d */
static void copybytes(void far *d, void far *s, unsigned int l) {
  while (l != 0) {
    l--;
    *(unsigned char far *)d = *(unsigned char far *)s;
    d = (unsigned char far *)d + 1;
    s = (unsigned char far *)s + 1;
  }
}

static unsigned short mystrlen(void far *s) {
  unsigned short res = 0;
  while (*(unsigned char far *)s != 0) {
    res++;
    s = ((unsigned char far *)s) + 1;
  }
  return(res);
}

/* returns -1 if the NULL-terminated s string contains any wildcard (?, *)
 * character. otherwise returns the length of the string. */
static int len_if_no_wildcards(char far *s) {
  int r = 0;
  for (;;) {
    switch (*s) {
      case 0: return(r);
      case '?':
      case '*': return(-1);
    }
    r++;
    s++;
  }
}

/* the PicoGUS transport is part of the resident code (it follows the same
 * rules: no libc, no strings). see xport.h for what it provides. */
#include "xport.c"


/* translates a drive letter (either upper- or lower-case) into a number (A=0,
 * B=1, C=2, etc) */
#define DRIVETONUM(x) (((x) >= 'a') && ((x) <= 'z')?x-'a':x-'A')


/* all the calls I support are in the range AL=0..2Eh - the list below serves
 * as a convenience to compare AL (subfunction) values */
enum AL_SUBFUNCTIONS {
  AL_INSTALLCHK = 0x00,
  AL_RMDIR      = 0x01,
  AL_MKDIR      = 0x03,
  AL_CHDIR      = 0x05,
  AL_CLSFIL     = 0x06,
  AL_CMMTFIL    = 0x07,
  AL_READFIL    = 0x08,
  AL_WRITEFIL   = 0x09,
  AL_LOCKFIL    = 0x0A,
  AL_UNLOCKFIL  = 0x0B,
  AL_DISKSPACE  = 0x0C,
  AL_SETATTR    = 0x0E,
  AL_GETATTR    = 0x0F,
  AL_RENAME     = 0x11,
  AL_DELETE     = 0x13,
  AL_OPEN       = 0x16,
  AL_CREATE     = 0x17,
  AL_FINDFIRST  = 0x1B,
  AL_FINDNEXT   = 0x1C,
  AL_SKFMEND    = 0x21,
  AL_UNKNOWN_2D = 0x2D,
  AL_SPOPNFIL   = 0x2E,
  AL_UNKNOWN    = 0xFF
};

/* this table makes it easy to figure out if I want a subfunction or not */
static unsigned char supportedfunctions[0x2F] = {
  AL_INSTALLCHK,  /* 0x00 */
  AL_RMDIR,       /* 0x01 */
  AL_UNKNOWN,     /* 0x02 */
  AL_MKDIR,       /* 0x03 */
  AL_UNKNOWN,     /* 0x04 */
  AL_CHDIR,       /* 0x05 */
  AL_CLSFIL,      /* 0x06 */
  AL_CMMTFIL,     /* 0x07 */
  AL_READFIL,     /* 0x08 */
  AL_WRITEFIL,    /* 0x09 */
  AL_LOCKFIL,     /* 0x0A */
  AL_UNLOCKFIL,   /* 0x0B */
  AL_DISKSPACE,   /* 0x0C */
  AL_UNKNOWN,     /* 0x0D */
  AL_SETATTR,     /* 0x0E */
  AL_GETATTR,     /* 0x0F */
  AL_UNKNOWN,     /* 0x10 */
  AL_RENAME,      /* 0x11 */
  AL_UNKNOWN,     /* 0x12 */
  AL_DELETE,      /* 0x13 */
  AL_UNKNOWN,     /* 0x14 */
  AL_UNKNOWN,     /* 0x15 */
  AL_OPEN,        /* 0x16 */
  AL_CREATE,      /* 0x17 */
  AL_UNKNOWN,     /* 0x18 */
  AL_UNKNOWN,     /* 0x19 */
  AL_UNKNOWN,     /* 0x1A */
  AL_FINDFIRST,   /* 0x1B */
  AL_FINDNEXT,    /* 0x1C */
  AL_UNKNOWN,     /* 0x1D */
  AL_UNKNOWN,     /* 0x1E */
  AL_UNKNOWN,     /* 0x1F */
  AL_UNKNOWN,     /* 0x20 */
  AL_SKFMEND,     /* 0x21 */
  AL_UNKNOWN,     /* 0x22 */
  AL_UNKNOWN,     /* 0x23 */
  AL_UNKNOWN,     /* 0x24 */
  AL_UNKNOWN,     /* 0x25 */
  AL_UNKNOWN,     /* 0x26 */
  AL_UNKNOWN,     /* 0x27 */
  AL_UNKNOWN,     /* 0x28 */
  AL_UNKNOWN,     /* 0x29 */
  AL_UNKNOWN,     /* 0x2A */
  AL_UNKNOWN,     /* 0x2B */
  AL_UNKNOWN,     /* 0x2C */
  AL_UNKNOWN_2D,  /* 0x2D */
  AL_SPOPNFIL     /* 0x2E */
};

/*
an INTPACK struct contains following items:
regs.w.gs
regs.w.fs
regs.w.es
regs.w.ds
regs.w.di
regs.w.si
regs.w.bp
regs.w.sp
regs.w.bx
regs.w.dx
regs.w.cx
regs.w.ax
regs.w.ip
regs.w.cs
regs.w.flags (AND with INTR_CF to fetch the CF flag - INTR_CF is defined as 0x0001)

regs.h.bl
regs.h.bh
regs.h.dl
regs.h.dh
regs.h.cl
regs.h.ch
regs.h.al
regs.h.ah
*/


/* sends the query out, as found in glob_frame (the caller has copied the
 * payload to glob_frame+4 already), and awaits for an answer. the answer
 * overwrites the request in glob_frame. this function returns the length of
 * replyptr, or 0xFFFF on error (no USB drive mounted, timeout, or request
 * rejected twice - xport_transact() re-sends once on ABORTED). */
static unsigned short sendquery(unsigned char query, unsigned char drive, unsigned short bufflen, unsigned char **replyptr, unsigned short **replyax) {
  /* resolve remote drive - no need to validate it, it has been validated
   * already by inthandler() */
  drive = glob_data.ldrv[drive];

  /* bufflen provides payload's lenght, but the header wants the frame's len */
  bufflen += DFS_HDR_LEN;

  /* if query too long then quit */
  if (bufflen > FRAMESIZE) return(0xFFFFu);

  /* fill in the header: LL LL DD AL */
  glob_frame[0] = bufflen & 0xff;
  glob_frame[1] = bufflen >> 8;
  glob_frame[2] = drive;
  glob_frame[3] = query; /* AL value (query) */
  /* I do not copy anything more into glob_frame - the caller is expected to
   * have already copied all relevant data into glob_frame+4 */

  /* ship it, wait for the answer (up to ~30 s), read it back */
  if (xport_transact(glob_frame, FRAMESIZE) != XPORT_OK) return(0xFFFFu);

  /* return buffer (without header) */
  *replyptr = glob_frame + DFS_HDR_LEN;
  *replyax = (unsigned short *)(glob_frame + 2);
  return((glob_frame[0] | (glob_frame[1] << 8)) - DFS_HDR_LEN);
}


/* reset CF (set on error only) and AX (expected to contain the error code,
 * I might set it later) - I assume a success */
#define SUCCESSFLAG glob_intregs.w.ax = 0; glob_intregs.w.flags &= ~(INTR_CF);
#define FAILFLAG(x) {glob_intregs.w.ax = x; glob_intregs.w.flags |= INTR_CF;}

/* this function contains the logic behind INT 2F processing */
void process2f(void) {
#if DEBUGLEVEL > 0
  char far *dbg_msg = NULL;
#endif
  short i;
  unsigned char *answer;
  unsigned char *buff; /* pointer to the "query arguments" part of glob_frame */
  unsigned char subfunction;
  unsigned short *ax; /* used to collect the resulting value of AX */
  buff = glob_frame + DFS_HDR_LEN;

  /* DEBUG output (RED) */
#if DEBUGLEVEL > 0
  dbg_xpos &= 511;
  dbg_VGA[dbg_startoffset + dbg_xpos++] = 0x4e00 | ' ';
  dbg_VGA[dbg_startoffset + dbg_xpos++] = 0x4e00 | (dbg_hexc[(glob_intregs.h.al >> 4) & 0xf]);
  dbg_VGA[dbg_startoffset + dbg_xpos++] = 0x4e00 | (dbg_hexc[glob_intregs.h.al & 0xf]);
  dbg_VGA[dbg_startoffset + dbg_xpos++] = 0x4e00 | ' ';
#endif

  /* remember the AL register (0x2F subfunction id) */
  subfunction = glob_intregs.h.al;

  /* if we got here, then the call is definitely for us. set AX and CF to */
  /* 'success' (being a natural optimist I assume success) */
  SUCCESSFLAG;

  /* read-only mapping (/R): refuse anything that would modify the drive
   * without bothering the card. opens are refused only when asking for
   * write access (OPEN) or for create/truncate (SPOPNFIL). */
  if (glob_readonly != 0) {
    switch (subfunction) {
      case AL_RMDIR:
      case AL_MKDIR:
      case AL_WRITEFIL:
      case AL_SETATTR:
      case AL_RENAME:
      case AL_DELETE:
      case AL_CREATE:
        FAILFLAG(5); /* "access denied" */
        return;
      case AL_OPEN:
        if ((glob_sdaptr->open_mode & 3) != 0) {
          FAILFLAG(5);
          return;
        }
        break;
      case AL_SPOPNFIL:
        if (((glob_sdaptr->spop_mode & 3) != 0) || ((glob_sdaptr->spop_act & 0x12) != 0)) {
          FAILFLAG(5);
          return;
        }
        break;
    }
  }

  /* look what function is called exactly and process it */
  switch (subfunction) {
    case AL_RMDIR: /*** 01h: RMDIR ******************************************/
      /* RMDIR is like MKDIR, but I need to check if dir is not current first */
      for (i = 0; glob_sdaptr->fn1[i] != 0; i++) {
        if (glob_sdaptr->fn1[i] != glob_sdaptr->drive_cdsptr[i]) goto proceedasmkdir;
      }
      FAILFLAG(16); /* err 16 = "attempted to remove current directory" */
      break;
      proceedasmkdir:
    case AL_MKDIR: /*** 03h: MKDIR ******************************************/
      i = mystrlen(glob_sdaptr->fn1);
      /* fn1 must be at least 2 bytes long */
      if (i < 2) {
        FAILFLAG(3); /* "path not found" */
        break;
      }
      /* copy fn1 to buff (but skip drive part) */
      i -= 2;
      copybytes(buff, glob_sdaptr->fn1 + 2, i);
      /* send query providing fn1 */
      if (sendquery(subfunction, glob_reqdrv, i, &answer, &ax) != 0xFFFFu) {
        glob_intregs.w.ax = *ax;
        if (*ax != 0) glob_intregs.w.flags |= INTR_CF;
      } else {
        FAILFLAG(ERR_NOTREADY);
      }
      break;
    case AL_CHDIR: /*** 05h: CHDIR ******************************************/
      /* The INT 2Fh/1105h redirector callback is executed by DOS when
       * changing directories. The Phantom authors (and RBIL contributors)
       * clearly thought that it was the redirector's job to update the CDS,
       * but in fact the callback is only meant to validate that the target
       * directory exists; DOS subsequently updates the CDS. */
      /* fn1 must be at least 2 bytes long */
      i = mystrlen(glob_sdaptr->fn1);
      if (i < 2) {
        FAILFLAG(3); /* "path not found" */
        break;
      }
      /* copy fn1 to buff (but skip the drive: part) */
      i -= 2;
      copybytes(buff, glob_sdaptr->fn1 + 2, i);
      /* send query providing fn1 */
      if (sendquery(AL_CHDIR, glob_reqdrv, i, &answer, &ax) != 0xFFFFu) {
        glob_intregs.w.ax = *ax;
        if (*ax != 0) glob_intregs.w.flags |= INTR_CF;
      } else {
        FAILFLAG(ERR_NOTREADY);
      }
      break;
    case AL_CLSFIL: /*** 06h: CLSFIL ****************************************/
      /* my only job is to decrement the SFT's handle count (which I didn't
       * have to increment during OPENFILE since DOS does it... talk about
       * consistency. I also inform the server about this, just so it knows */
      /* ES:DI points to the SFT */
      {
      struct sftstruct far *sftptr = MK_FP(glob_intregs.x.es, glob_intregs.x.di);
      if (sftptr->handle_count > 0) sftptr->handle_count--;
      ((unsigned short *)buff)[0] = sftptr->start_sector;
      if (sendquery(AL_CLSFIL, glob_reqdrv, 2, &answer, &ax) != 0xFFFFu) {
        if (*ax != 0) FAILFLAG(*ax);
      }
      }
      break;
    case AL_CMMTFIL: /*** 07h: CMMTFIL **************************************/
      /* I have nothing to do here */
      break;
    case AL_READFIL: /*** 08h: READFIL **************************************/
      { /* ES:DI points to the SFT (whose file_pos needs to be updated) */
        /* CX = number of bytes to read (to be updated with number of bytes actually read) */
        /* SDA DTA = read buffer */
      struct sftstruct far *sftptr = MK_FP(glob_intregs.x.es, glob_intregs.x.di);
      unsigned short totreadlen;
      /* is the file open for write-only? */
      if (sftptr->open_mode & 1) {
        FAILFLAG(5); /* "access denied" */
        break;
      }
      /* return immediately if the caller wants to read 0 bytes */
      if (glob_intregs.x.cx == 0) break;
      /* do multiple read operations so chunks can fit in my frames */
      totreadlen = 0;
      for (;;) {
        unsigned short chunklen, len;
        if ((glob_intregs.x.cx - totreadlen) < glob_chunk) {
          chunklen = glob_intregs.x.cx - totreadlen;
        } else {
          chunklen = glob_chunk;
        }
        /* query is OOOOSSLL (offset, start sector, lenght to read) */
        ((unsigned long *)buff)[0] = sftptr->file_pos + totreadlen;
        ((unsigned short *)buff)[2] = sftptr->start_sector;
        ((unsigned short *)buff)[3] = chunklen;
        len = sendquery(AL_READFIL, glob_reqdrv, 8, &answer, &ax);
        if (len == 0xFFFFu) { /* transport error */
          FAILFLAG(ERR_NOTREADY);
          break;
        } else if (*ax != 0) { /* backend error */
          FAILFLAG(*ax);
          break;
        } else { /* success */
          if (len > chunklen) len = chunklen; /* never trust the card blindly */
          copybytes(glob_sdaptr->curr_dta + totreadlen, answer, len);
          totreadlen += len;
          if ((len < chunklen) || (totreadlen == glob_intregs.x.cx)) { /* EOF - update SFT and break out */
            sftptr->file_pos += totreadlen;
            glob_intregs.x.cx = totreadlen;
            break;
          }
        }
      }
      }
      break;
    case AL_WRITEFIL: /*** 09h: WRITEFIL ************************************/
      { /* ES:DI points to the SFT (whose file_pos needs to be updated) */
        /* CX = number of bytes to write (to be updated with number of bytes actually written) */
        /* SDA DTA = read buffer */
      struct sftstruct far *sftptr = MK_FP(glob_intregs.x.es, glob_intregs.x.di);
      unsigned short bytesleft, chunklen, written = 0;
      /* is the file open for read-only? */
      if ((sftptr->open_mode & 3) == 0) {
        FAILFLAG(5); /* "access denied" */
        break;
      }
      /* TODO FIXME I should update the file's time in the SFT here */
      /* do multiple write operations so chunks can fit in my frames */
      bytesleft = glob_intregs.x.cx;

      /* note: a zero-length write is sent as-is, it truncates the file at
       * file_pos (that's how DOS truncates files) */
      do {
        unsigned short len;
        chunklen = bytesleft;
        if (chunklen > glob_chunk - 6) chunklen = glob_chunk - 6;
        /* query is OOOOSS (file offset, start sector/fileid) */
        ((unsigned long *)buff)[0] = sftptr->file_pos;
        ((unsigned short *)buff)[2] = sftptr->start_sector;
        copybytes(buff + 6, glob_sdaptr->curr_dta + written, chunklen);
        len = sendquery(AL_WRITEFIL, glob_reqdrv, chunklen + 6, &answer, &ax);
        if (len == 0xFFFFu) { /* transport error */
          FAILFLAG(ERR_NOTREADY);
          break;
        } else if ((*ax != 0) || (len != 2)) { /* backend error */
          FAILFLAG(*ax);
          break;
        } else { /* success - write amount of bytes written into CX and update SFT */
          len = ((unsigned short *)answer)[0];
          written += len;
          bytesleft -= len;
          glob_intregs.x.cx = written;
          sftptr->file_pos += len;
          if (sftptr->file_pos > sftptr->file_size) sftptr->file_size = sftptr->file_pos;
          if (chunklen == 0) sftptr->file_size = sftptr->file_pos; /* truncated */
          if (len != chunklen) break; /* something bad happened on the other side */
        }
      } while (bytesleft > 0);
      }
      break;
    case AL_LOCKFIL: /*** 0Ah: LOCKFIL **************************************/
      {
      struct sftstruct far *sftptr = MK_FP(glob_intregs.x.es, glob_intregs.x.di);
      ((unsigned short *)buff)[0] = glob_intregs.x.cx;
      ((unsigned short *)buff)[1] = sftptr->start_sector;
      if (glob_intregs.h.bl > 1) FAILFLAG(2); /* BL should be either 0 (lock) or 1 (unlock) */
      /* copy 8*CX bytes from DS:DX to buff+4 (parameters block) */
      copybytes(buff + 4, MK_FP(glob_intregs.x.ds, glob_intregs.x.dx), glob_intregs.x.cx << 3);
      if (sendquery(AL_LOCKFIL + glob_intregs.h.bl, glob_reqdrv, (glob_intregs.x.cx << 3) + 4, &answer, &ax) == 0xFFFFu) {
        FAILFLAG(ERR_NOTREADY);
      }
      }
      break;
    case AL_UNLOCKFIL: /*** 0Bh: UNLOCKFIL **********************************/
      /* Nothing here - this isn't supposed to be used by DOS 4+ */
      FAILFLAG(2);
      break;
    case AL_DISKSPACE: /*** 0Ch: get disk information ***********************/
      if (sendquery(AL_DISKSPACE, glob_reqdrv, 0, &answer, &ax) == 6) {
        glob_intregs.w.ax = *ax; /* sectors per cluster */
        glob_intregs.w.bx = ((unsigned short *)answer)[0]; /* total clusters */
        glob_intregs.w.cx = ((unsigned short *)answer)[1]; /* bytes per sector */
        glob_intregs.w.dx = ((unsigned short *)answer)[2]; /* num of available clusters */
      } else {
        FAILFLAG(ERR_NOTREADY);
      }
      break;
    case AL_SETATTR: /*** 0Eh: SETATTR **************************************/
      /* sdaptr->fn1 -> file to set attributes for
         stack word -> new attributes (stack must not be changed!) */
      /* fn1 must be at least 2 characters long */
      i = mystrlen(glob_sdaptr->fn1);
      if (i < 2) {
        FAILFLAG(2);
        break;
      }
      /* */
      buff[0] = glob_reqstkword;
      /* copy fn1 to buff (but without the drive part) */
      copybytes(buff + 1, glob_sdaptr->fn1 + 2, i - 2);
    #if DEBUGLEVEL > 0
      dbg_VGA[dbg_startoffset + dbg_xpos++] = 0x1000 | dbg_hexc[(glob_reqstkword >> 4) & 15];
      dbg_VGA[dbg_startoffset + dbg_xpos++] = 0x1000 | dbg_hexc[glob_reqstkword & 15];
    #endif
      i = sendquery(AL_SETATTR, glob_reqdrv, i - 1, &answer, &ax);
      if ((unsigned short)i == 0xffffu) {
        FAILFLAG(ERR_NOTREADY);
      } else if (*ax != 0) {
        FAILFLAG(*ax);
      }
      break;
    case AL_GETATTR: /*** 0Fh: GETATTR **************************************/
      i = mystrlen(glob_sdaptr->fn1);
      if (i < 2) {
        FAILFLAG(2);
        break;
      }
      i -= 2;
      copybytes(buff, glob_sdaptr->fn1 + 2, i);
      i = sendquery(AL_GETATTR, glob_reqdrv, i, &answer, &ax);
      if ((unsigned short)i == 0xffffu) {
        FAILFLAG(ERR_NOTREADY);
      } else if ((i != 9) || (*ax != 0)) {
        FAILFLAG(*ax);
      } else { /* all good */
        /* CX = timestamp
         * DX = datestamp
         * BX:DI = fsize
         * AX = attr
         * NOTE: Undocumented DOS talks only about setting AX, no fsize, time
         *       and date, these are documented in RBIL and used by SHSUCDX */
        glob_intregs.w.cx = ((unsigned short *)answer)[0]; /* time */
        glob_intregs.w.dx = ((unsigned short *)answer)[1]; /* date */
        glob_intregs.w.bx = ((unsigned short *)answer)[3]; /* fsize hi word */
        glob_intregs.w.di = ((unsigned short *)answer)[2]; /* fsize lo word */
        glob_intregs.w.ax = answer[8];                     /* file attribs */
      }
      break;
    case AL_RENAME: /*** 11h: RENAME ****************************************/
      /* sdaptr->fn1 = old name
       * sdaptr->fn2 = new name */
      /* is the operation for the SAME drive? */
      if (glob_sdaptr->fn1[0] != glob_sdaptr->fn2[0]) {
        FAILFLAG(2);
        break;
      }
      /* prepare the query (LSSS...DDD...) */
      i = mystrlen(glob_sdaptr->fn1);
      if (i < 2) {
        FAILFLAG(2);
        break;
      }
      i -= 2; /* trim out the drive: part (C:\FILE --> \FILE) */
      buff[0] = i;
      copybytes(buff + 1, glob_sdaptr->fn1 + 2, i);
      i = len_if_no_wildcards(glob_sdaptr->fn2);
      if (i < 2) {
        FAILFLAG(3);
        break;
      }
      i -= 2; /* trim out the drive: part (C:\FILE --> \FILE) */
      copybytes(buff + 1 + buff[0], glob_sdaptr->fn2 + 2, i);
      /* send the query out */
      i = sendquery(AL_RENAME, glob_reqdrv, 1 + buff[0] + i, &answer, &ax);
      if ((unsigned short)i == 0xffffu) {
        FAILFLAG(ERR_NOTREADY);
      } else if (*ax != 0) {
        FAILFLAG(*ax);
      }
      break;
    case AL_DELETE: /*** 13h: DELETE ****************************************/
    #if DEBUGLEVEL > 0
      dbg_msg = glob_sdaptr->fn1;
    #endif
      /* compute length of fn1 and copy it to buff (w/o the 'drive:' part) */
      i = mystrlen(glob_sdaptr->fn1);
      if (i < 2) {
        FAILFLAG(2);
        break;
      }
      i -= 2;
      copybytes(buff, glob_sdaptr->fn1 + 2, i);
      /* send query */
      i = sendquery(AL_DELETE, glob_reqdrv, i, &answer, &ax);
      if ((unsigned short)i == 0xffffu) {
        FAILFLAG(ERR_NOTREADY);
      } else if ((i != 0) || (*ax != 0)) {
        FAILFLAG(*ax);
      }
      break;
    case AL_OPEN: /*** 16h: OPEN ********************************************/
    case AL_CREATE: /*** 17h: CREATE ****************************************/
    case AL_SPOPNFIL: /*** 2Eh: SPOPNFIL ************************************/
    #if DEBUGLEVEL > 0
      dbg_msg = glob_sdaptr->fn1;
    #endif
      /* fail if fn1 contains any wildcard, otherwise get len of fn1 */
      i = len_if_no_wildcards(glob_sdaptr->fn1);
      if (i < 2) {
        FAILFLAG(3);
        break;
      }
      i -= 2;
      /* prepare and send query (SSCCMMfff...) */
      ((unsigned short *)buff)[0] = glob_reqstkword; /* WORD from the stack */
      ((unsigned short *)buff)[1] = glob_sdaptr->spop_act; /* action code (SPOP only) */
      ((unsigned short *)buff)[2] = glob_sdaptr->spop_mode; /* open mode (SPOP only) */
      copybytes(buff + 6, glob_sdaptr->fn1 + 2, i);
      i = sendquery(subfunction, glob_reqdrv, i + 6, &answer, &ax);
      if ((unsigned short)i == 0xffffu) {
        FAILFLAG(ERR_NOTREADY);
      } else if ((i != 25) || (*ax != 0)) {
        FAILFLAG(*ax);
      } else {
        /* ES:DI contains an uninitialized SFT */
        struct sftstruct far *sftptr = MK_FP(glob_intregs.x.es, glob_intregs.x.di);
        /* special treatment for SPOP, (set open_mode and return CX, too) */
        if (subfunction == AL_SPOPNFIL) {
          glob_intregs.w.cx = ((unsigned short *)answer)[11];
        }
        if (sftptr->open_mode & 0x8000) { /* if bit 15 is set, then it's a "FCB open", and requires the internal DOS "Set FCB Owner" function to be called */
          /* TODO FIXME set_sft_owner() */
        #if DEBUGLEVEL > 0
          dbg_VGA[25*80] = 0x1700 | '$';
        #endif
        }
        sftptr->file_attr = answer[0];
        sftptr->dev_info_word = 0x8040 | glob_reqdrv; /* mark device as network & unwritten drive */
        sftptr->dev_drvr_ptr = NULL;
        sftptr->start_sector = ((unsigned short *)answer)[10];
        sftptr->file_time = ((unsigned long *)answer)[3];
        sftptr->file_size = ((unsigned long *)answer)[4];
        sftptr->file_pos = 0;
        sftptr->open_mode &= 0xff00u;
        sftptr->open_mode |= answer[24];
        sftptr->rel_sector = 0xffff;
        sftptr->abs_sector = 0xffff;
        sftptr->dir_sector = 0;
        sftptr->dir_entry_no = 0xff; /* why such value? no idea, PHANTOM.C uses that, too */
        copybytes(sftptr->file_name, answer + 1, 11);
      }
      break;
    case AL_FINDFIRST: /*** 1Bh: FINDFIRST **********************************/
    case AL_FINDNEXT:  /*** 1Ch: FINDNEXT ***********************************/
      {
      /* AX = 111Bh
      SS = DS = DOS DS
      [DTA] = uninitialized 21-byte findfirst search data
      (see #01626 at INT 21/AH=4Eh)
      SDA first filename pointer (FN1, 9Eh) -> fully-qualified search template
      SDA CDS pointer -> current directory structure for drive with file
      SDA search attribute = attribute mask for search

      Return:
      CF set on error
      AX = DOS error code (see #01680 at INT 21/AH=59h/BX=0000h)
           -> http://www.ctyme.com/intr/rb-3012.htm
      CF clear if successful
      [DTA] = updated findfirst search data
      (bit 7 of first byte must be set)
      [DTA+15h] = standard directory entry for file (see #01352)

      FindNext is the same, but only DTA should be used to fetch search params
      */
      struct sdbstruct far *dta;

#if DEBUGLEVEL > 0
      dbg_msg = glob_sdaptr->fn1;
#endif
      /* prepare the query buffer (i must provide query's length) */
      if (subfunction == AL_FINDFIRST) {
        dta = (struct sdbstruct far *)(glob_sdaptr->curr_dta);
        /* FindFirst needs to fetch search arguments from SDA */
        buff[0] = glob_sdaptr->srch_attr; /* file attributes to look for */
        /* copy fn1 (w/o drive) to buff */
        for (i = 2; glob_sdaptr->fn1[i] != 0; i++) buff[i-1] = glob_sdaptr->fn1[i];
        i--; /* adjust i because its one too much otherwise */
      } else { /* FindNext needs to fetch search arguments from DTA (es:di) */
        dta = MK_FP(glob_intregs.x.es, glob_intregs.x.di);
        ((unsigned short *)buff)[0] = dta->par_clstr;
        ((unsigned short *)buff)[1] = dta->dir_entry;
        buff[4] = dta->srch_attr;
        /* copy search template to buff */
        for (i = 0; i < 11; i++) buff[i+5] = dta->srch_tmpl[i];
        i += 5; /* i must provide the exact query's length */
      }
      /* send query to the card and wait for answer */
      i = sendquery(subfunction, glob_reqdrv, i, &answer, &ax);
      if ((unsigned short)i == 0xffffu) {
        FAILFLAG(ERR_NOTREADY);
        break;
      } else if ((*ax != 0) || (i != 24)) {
        FAILFLAG(*ax);
        break;
      }
      /* fill in the directory entry 'found_file' (32 bytes)
       * 00h unsigned char fname[11]
       * 0Bh unsigned char fattr (1=RO 2=HID 4=SYS 8=VOL 16=DIR 32=ARCH 64=DEV)
       * 0Ch unsigned char f1[10]
       * 16h unsigned short time_lstupd
       * 18h unsigned short date_lstupd
       * 1Ah unsigned short start_clstr  *optional*
       * 1Ch unsigned long fsize
       */
      copybytes(glob_sdaptr->found_file.fname, answer+1, 11); /* found file name */
      glob_sdaptr->found_file.fattr = answer[0]; /* found file attributes */
      glob_sdaptr->found_file.time_lstupd = ((unsigned short *)answer)[6]; /* time (word) */
      glob_sdaptr->found_file.date_lstupd = ((unsigned short *)answer)[7]; /* date (word) */
      glob_sdaptr->found_file.start_clstr = 0; /* start cluster (I don't care) */
      glob_sdaptr->found_file.fsize = ((unsigned long *)answer)[4]; /* fsize (word) */

      /* put things into DTA so I can understand where I left should FindNext
       * be called - this shall be a valid FindFirst structure (21 bytes):
       * 00h unsigned char drive letter (7bits, MSB must be set for remote drives)
       * 01h unsigned char search_tmpl[11]
       * 0Ch unsigned char search_attr (1=RO 2=HID 4=SYS 8=VOL 16=DIR 32=ARCH 64=DEV)
       * 0Dh unsigned short entry_count_within_directory
       * 0Fh unsigned short cluster number of start of parent directory
       * 11h unsigned char reserved[4]
       * -- RBIL says: [DTA+15h] = standard directory entry for file
       * 15h 11-bytes (FCB-style) filename+ext ("FILE0000TXT")
       * 20h unsigned char attr. of file found (1=RO 2=HID 4=SYS 8=VOL 16=DIR 32=ARCH 64=DEV)
       * 21h 10-bytes reserved
       * 2Bh unsigned short file time
       * 2Dh unsigned short file date
       * 2Fh unsigned short cluster
       * 31h unsigned long file size
       */
      /* init some stuff only on FindFirst (FindNext contains valid values already) */
      if (subfunction == AL_FINDFIRST) {
        dta->drv_lett = glob_reqdrv | 128; /* bit 7 set means 'network drive' */
        copybytes(dta->srch_tmpl, glob_sdaptr->fcb_fn1, 11);
        dta->srch_attr = glob_sdaptr->srch_attr;
      }
      dta->par_clstr = ((unsigned short *)answer)[10];
      dta->dir_entry = ((unsigned short *)answer)[11];
      /* the 32-byte directory entry at DTA+15h is the SDA's found_file, which
       * DOS itself copies to the caller's DTA; the original EtherDFS line here
       * did struct-pointer arithmetic (dta + 0x15 = +441 bytes) and wrote past
       * the SDB, so it is gone. */
      }
      break;
    case AL_SKFMEND: /*** 21h: SKFMEND **************************************/
    {
      struct sftstruct far *sftptr = MK_FP(glob_intregs.x.es, glob_intregs.x.di);
      ((unsigned short *)buff)[0] = glob_intregs.x.dx;
      ((unsigned short *)buff)[1] = glob_intregs.x.cx;
      ((unsigned short *)buff)[2] = sftptr->start_sector;
      /* send query to the card and wait for answer */
      i = sendquery(AL_SKFMEND, glob_reqdrv, 6, &answer, &ax);
      if ((unsigned short)i == 0xffffu) {
        FAILFLAG(ERR_NOTREADY);
      } else if ((*ax != 0) || (i != 4)) {
        FAILFLAG(*ax);
      } else { /* put new position into DX:AX */
        glob_intregs.w.ax = ((unsigned short *)answer)[0];
        glob_intregs.w.dx = ((unsigned short *)answer)[1];
      }
      break;
    }
    case AL_UNKNOWN_2D: /*** 2Dh: UNKNOWN_2D ********************************/
      /* this is only called in MS-DOS v4.01, its purpose is unknown. MSCDEX
       * returns AX=2 there, and so do I. */
      glob_intregs.w.ax = 2;
      break;
  }

  /* DEBUG */
#if DEBUGLEVEL > 0
  while ((dbg_msg != NULL) && (*dbg_msg != 0)) dbg_VGA[dbg_startoffset + dbg_xpos++] = 0x4f00 | *(dbg_msg++);
#endif
}

/* this function is hooked on INT 2Fh */
void __interrupt __far inthandler(union INTPACK r) {
  /* insert a static code signature so I can reliably patch myself later,
   * this will also contain the DS segment to use and actually set it */
  _asm {
    jmp SKIPTSRSIG
    TSRSIG DB 'P','G','d','f'
    SKIPTSRSIG:
    /* save AX */
    push ax
    /* switch to new (patched) DS */
    mov ax, 0
    mov ds, ax
    /* save one word from the stack (might be used by SETATTR later)
     * The original stack should be at SS:BP+30 */
    mov ax, ss:[BP+30]
    mov glob_reqstkword, ax
    /* restore AX */
    pop ax
  }

  /* DEBUG output (BLUE) */
#if DEBUGLEVEL > 1
  dbg_VGA[dbg_startoffset + dbg_xpos++] = 0x1e00 | (dbg_hexc[(r.h.ah >> 4) & 0xf]);
  dbg_VGA[dbg_startoffset + dbg_xpos++] = 0x1e00 | (dbg_hexc[r.h.ah & 0xf]);
  dbg_VGA[dbg_startoffset + dbg_xpos++] = 0x1e00 | (dbg_hexc[(r.h.al >> 4) & 0xf]);
  dbg_VGA[dbg_startoffset + dbg_xpos++] = 0x1e00 | (dbg_hexc[r.h.al & 0xf]);
  dbg_VGA[dbg_startoffset + dbg_xpos++] = 0;
#endif

  /* is it a multiplex call for me? */
  if (r.h.ah == glob_multiplexid) {
    if (r.h.al == 0) { /* install check */
      r.h.al = 0xff;    /* 'installed' */
      r.w.bx = 0x4750;  /* "PG"        */
      r.w.cx = 0x4644;  /* "DF"        */
      return;
    }
    if ((r.h.al == 1) && (r.x.cx == 0x4750)) { /* get shared data ptr (AX=0, ptr under BX:CX) */
      _asm {
        push ds
        pop glob_reqstkword
      }
      r.w.ax = 0; /* zero out AX */
      r.w.bx = glob_reqstkword; /* ptr returned at BX:CX */
      r.w.cx = FP_OFF(&glob_data);
      return;
    }
  }

  /* if not related to a redirector function (AH=11h), or the function is
   * an 'install check' (0), or the function is over our scope (2Eh), or it's
   * an otherwise unsupported function (as pointed out by supportedfunctions),
   * then call the previous INT 2F handler immediately */
  if ((r.h.ah != 0x11) || (r.h.al == AL_INSTALLCHK) || (r.h.al > 0x2E) || (supportedfunctions[r.h.al] == AL_UNKNOWN)) goto CHAINTOPREVHANDLER;

  /* DEBUG output (GREEN) */
#if DEBUGLEVEL > 0
  dbg_VGA[dbg_startoffset + dbg_xpos++] = 0x2e00 | (dbg_hexc[(r.h.al >> 4) & 0xf]);
  dbg_VGA[dbg_startoffset + dbg_xpos++] = 0x2e00 | (dbg_hexc[r.h.al & 0xf]);
  dbg_VGA[dbg_startoffset + dbg_xpos++] = 0;
#endif

  /* determine whether or not the query is meant for a drive I control,
   * and if not - chain to the previous INT 2F handler */
  if (((r.h.al >= AL_CLSFIL) && (r.h.al <= AL_UNLOCKFIL)) || (r.h.al == AL_SKFMEND) || (r.h.al == AL_UNKNOWN_2D)) {
  /* ES:DI points to the SFT: if the bottom 6 bits of the device information
   * word in the SFT are > last drive, then it relates to files not associated
   * with drives, such as LAN Manager named pipes. */
    struct sftstruct far *sft = MK_FP(r.w.es, r.w.di);
    glob_reqdrv = sft->dev_info_word & 0x3F;
  } else {
    switch (r.h.al) {
      case AL_FINDNEXT:
        glob_reqdrv = glob_sdaptr->sdb.drv_lett & 0x1F;
        break;
      case AL_SETATTR:
      case AL_GETATTR:
      case AL_DELETE:
      case AL_OPEN:
      case AL_CREATE:
      case AL_SPOPNFIL:
      case AL_MKDIR:
      case AL_RMDIR:
      case AL_CHDIR:
      case AL_RENAME: /* check sda.fn1 for drive */
        glob_reqdrv = DRIVETONUM(glob_sdaptr->fn1[0]);
        break;
      default: /* otherwise check out the CDS (at ES:DI) */
        {
        struct cdsstruct far *cds = MK_FP(r.w.es, r.w.di);
        glob_reqdrv = DRIVETONUM(cds->current_path[0]);
      #if DEBUGLEVEL > 0 /* DEBUG output (ORANGE) */
        dbg_VGA[dbg_startoffset + dbg_xpos++] = 0x6e00 | ('A' + glob_reqdrv);
        dbg_VGA[dbg_startoffset + dbg_xpos++] = 0x6e00 | ':';
      #endif
        }
        break;
    }
  }
  /* validate drive */
  if ((glob_reqdrv > 25) || (glob_data.ldrv[glob_reqdrv] == 0xff)) {
    goto CHAINTOPREVHANDLER;
  }

  /* This should not be necessary. DOS usually generates an FCB-style name in
   * the appropriate SDA area. However, in the case of user input such as
   * 'CD ..' or 'DIR ..' it leaves the fcb area all spaces, hence the need to
   * normalize the fcb area every time. */
  if (r.h.al != AL_DISKSPACE) {
    unsigned short i;
    unsigned char far *path = glob_sdaptr->fn1;

    /* fast forward 'path' to first character of the filename */
    for (i = 0;; i++) {
      if (glob_sdaptr->fn1[i] == '\\') path = glob_sdaptr->fn1 + i + 1;
      if (glob_sdaptr->fn1[i] == 0) break;
    }

    /* clear out fcb_fn1 by filling it with spaces */
    for (i = 0; i < 11; i++) glob_sdaptr->fcb_fn1[i] = ' ';

    /* copy 'path' into fcb_name using the fcb syntax ("FILE    TXT") */
    for (i = 0; *path != 0; path++) {
      if (*path == '.') {
        i = 8;
      } else {
        glob_sdaptr->fcb_fn1[i++] = *path;
      }
    }
  }

  /* copy interrupt registers into glob_intregs so the int handler can access them without using any stack */
  copybytes(&glob_intregs, &r, sizeof(union INTPACK));
  /* set stack to my custom memory */
  _asm {
    cli /* make sure to disable interrupts, so nobody gets in the way while I'm fiddling with the stack */
    mov glob_oldstack_seg, SS
    mov glob_oldstack_off, SP
    /* set SS to DS */
    mov ax, ds
    mov ss, ax
    /* set SP to the end of my DATASEGSZ (-2) */
    mov sp, DATASEGSZ-2
    sti
  }
  /* call the actual INT 2F processing function */
  process2f();
  /* switch stack back */
  _asm {
    cli
    mov SS, glob_oldstack_seg
    mov SP, glob_oldstack_off
    sti
  }
  /* copy all registers back so watcom will set them as required 'for real' */
  copybytes(&r, &glob_intregs, sizeof(union INTPACK));
  return;

  /* hand control to the previous INT 2F handler */
  CHAINTOPREVHANDLER:
  _mvchain_intr(MK_FP(glob_data.prev_2f_handler_seg, glob_data.prev_2f_handler_off));
}


/*********************** HERE ENDS THE RESIDENT PART ***********************/

#pragma code_seg("_TEXT", "CODE");

/* this function obviously does nothing - but I need it because it is a
 * 'low-water' mark for the end of my resident code (so I know how much memory
 * exactly I can trim when going TSR) */
void begtextend(void) {
}

/* all messages are 'far' strings: they land in a FAR_DATA segment that sits
 * outside DGROUP, so they neither get copied into the resident data segment
 * nor stay in memory after going TSR (EtherDFS used generated code for the
 * same purpose). They are '$'-terminated for INT 21h/AH=9. */
static const char far MSG_HELP[] =
  "PGUSDFS v" PVER " - the USB drive of a PicoGUS as a DOS drive letter\r\n"
  "Based on EtherDFS (C) 2017, 2018 Mateusz Viste - MIT license\r\n"
  "\r\n"
  "Usage: PGUSDFS X: [/Q] [/R]     map the PicoGUS USB drive to drive X:\r\n"
  "       PGUSDFS /U [/Q]          unload PGUSDFS from memory\r\n"
  "       PGUSDFS /T               push the DOS clock to the card and exit\r\n"
  "\r\n"
  "  /R   map the drive read-only\r\n"
  "  /Q   quiet: print nothing when loading or unloading succeeded\r\n"
  "  /T   only send the DOS date and time to the card (TSR not required)\r\n"
  "  /?   this help\r\n"
  "\r\n"
  "The data port comes from the card's settings (pgusinit /dfsport, default 1D4).\r\n$";
static const char far MSG_UNSUPDOS[] = "PGUSDFS requires DOS 5.0 or later.\r\n$";
static const char far MSG_NOREDIR[] = "The DOS network redirector API is not available (INT 2Fh/1100h).\r\n$";
static const char far MSG_NOTLOAD[] = "PGUSDFS is not loaded.\r\n$";
static const char far MSG_OTHERTSR[] = "Another TSR hooked INT 2Fh after PGUSDFS - cannot unload.\r\n$";
static const char far MSG_TSRCOMFA[] = "Communication with the resident PGUSDFS failed.\r\n$";
static const char far MSG_UNLOADED[] = "PGUSDFS unloaded.\r\n$";
static const char far MSG_ALRLOAD[] = "PGUSDFS is already loaded (use /U to unload it).\r\n$";
static const char far MSG_NOMULTPX[] = "No free INT 2Fh multiplex id found.\r\n$";
static const char far MSG_MAPFAIL[] = "Cannot map this drive letter (LASTDRIVE too low?).\r\n$";
static const char far MSG_DRVACTIV[] = "This drive letter is already in use.\r\n$";
static const char far MSG_MEMFAIL[] = "Memory allocation failed.\r\n$";
static const char far MSG_RELFAIL[] = "Internal error: cannot patch the resident code.\r\n$";
static const char far MSG_NOCARD[] = "PicoGUS not detected (nothing answers on port 1D0h).\r\n$";
static const char far MSG_PROTO1[] = "PicoGUS firmware uses protocol $";
static const char far MSG_PROTO2[] = ", PGDFS needs protocol 5 or later.\r\nPlease upgrade the PicoGUS firmware.\r\n$";
static const char far MSG_NOPGDFS[] = "This PicoGUS firmware has no PGDFS support - please upgrade the firmware.\r\n$";
static const char far MSG_DFSOFF[] = "PGDFS is disabled on this PicoGUS (enable it with pgusinit /dfsport 1D4).\r\n$";
static const char far MSG_DATAPORT[] = ", data port $";
static const char far MSG_USBDRV[] = "USB drive: $";
static const char far MSG_NOLABEL[] = "(no label)$";
static const char far MSG_MB[] = " MB)\r\n$";
static const char far MSG_NODRIVE[] = "No USB drive is mounted yet - insert one before using the drive letter.\r\n$";
static const char far MSG_TIMESET[] = "DOS date and time sent to the PicoGUS.\r\n$";
static const char far MSG_INSTLLED[] = "PGUSDFS installed: $";
static const char far MSG_INSTL2[] = " -> PicoGUS USB drive$";
static const char far MSG_RDONLY[] = " (read-only)$";
static const char far MSG_RESIDENT[] = ", resident size $";
static const char far MSG_BYTES[] = " bytes\r\n$";
static const char far MSG_CRLF[] = "\r\n$";

static struct sdastruct far *getsda(void) {
  /* DOS 3.0+ - GET ADDRESS OF SDA (Swappable Data Area)
   * AX = 5D06h
   *
   * CF set on error (AX=error code)
   * DS:SI -> sda pointer
   */
  unsigned short rds = 0, rsi = 0;
  _asm {
    mov ax, 5d06h
    push ds
    push si
    int 21h
    mov bx, ds
    mov cx, si
    pop si
    pop ds
    mov rds, bx
    mov rsi, cx
  }
  return(MK_FP(rds, rsi));
}

/* returns the CDS struct for drive. requires DOS 4+ */
static struct cdsstruct far *getcds(unsigned int drive) {
  /* static to preserve state: only do init once */
  static unsigned char far *dir;
  static int ok = -1;
  static unsigned char lastdrv;
  /* init of never inited yet */
  if (ok == -1) {
    /* DOS 3.x+ required - no CDS in earlier versions */
    ok = 1;
    /* offsets of CDS and lastdrv in the List of Lists depends on the DOS version:
     * DOS < 3   no CDS at all
     * DOS 3.0   lastdrv at 1Bh, CDS pointer at 17h
     * DOS 3.1+  lastdrv at 21h, CDS pointer at 16h */
    /* fetch lastdrv and CDS through a little bit of inline assembly */
    _asm {
      push si /* SI needs to be preserved */
      /* get the List of Lists into ES:BX */
      mov ah, 52h
      int 21h
      /* get the LASTDRIVE value */
      mov si, 21h /* 21h for DOS 3.1+, 1Bh on DOS 3.0 */
      mov ah, byte ptr es:[bx+si]
      mov lastdrv, ah
      /* get the CDS */
      mov si, 16h /* 16h for DOS 3.1+, 17h on DOS 3.0 */
      les bx, es:[bx+si]
      mov word ptr dir+2, es
      mov word ptr dir, bx
      /* restore the original SI value*/
      pop si
    }
    /* some OSes (at least OS/2) set the CDS pointer to FFFF:FFFF */
    if (dir == (unsigned char far *) -1l) ok = 0;
  } /* end of static initialization */
  if (ok == 0) return(NULL);
  if (drive > lastdrv) return(NULL);
  /* return the CDS array entry for drive - note that currdir_size depends on
   * DOS version: 0x51 on DOS 3.x, and 0x58 on DOS 4+ */
  return((struct cdsstruct __far *)((unsigned char __far *)dir + (drive * 0x58 /*currdir_size*/)));
}
/******* end of CDS-related stuff *******/

/* primitive message output used instead of printf() to limit memory usage
 * and binary size. s is a '$'-terminated far string. */
static void outmsg(const char far *s) {
  _asm {
    push ds
    lds dx, s   /* DS:DX -> string */
    mov ah, 9h  /* DOS 1+ - WRITE STRING TO STANDARD OUTPUT */
    int 21h
    pop ds
  }
}

/* prints a single character */
static void outchar(char c) {
  _asm {
    mov ah, 2h  /* DOS 1+ - WRITE CHARACTER TO STANDARD OUTPUT */
    mov dl, c
    int 21h
  }
}

/* prints a zero-terminated string from the data segment (or the stack) */
static void outstr(const char *s) {
  while (*s != 0) outchar(*(s++));
}

/* prints an unsigned number in decimal */
static void outdec(unsigned long v) {
  char buff[12];
  int i = 11;
  buff[i] = 0;
  do {
    buff[--i] = '0' + (v % 10);
    v /= 10;
  } while (v != 0);
  outstr(buff + i);
}

/* prints an unsigned number in hexadecimal (upper case, no leading zeros) */
static void outhex(unsigned short v) {
  char buff[6];
  int i = 5;
  buff[i] = 0;
  do {
    unsigned char d = v & 15;
    buff[--i] = (d < 10) ? ('0' + d) : ('A' + d - 10);
    v >>= 4;
  } while (v != 0);
  outstr(buff + i);
}

/* zero out an object of l bytes */
static void zerobytes(void *obj, unsigned short l) {
  unsigned char *o = obj;
  while (l-- != 0) {
    *o = 0;
    o++;
  }
}


#define ARGFL_QUIET 1
#define ARGFL_UNLOAD 4
#define ARGFL_READONLY 8
#define ARGFL_TIME 16
#define ARGFL_NOCHECK 32

/* a structure used to pass and decode arguments between main() and parseargv() */
struct argstruct {
  int argc;    /* original argc */
  char **argv; /* original argv */
  unsigned char flags; /* ARGFL_QUIET, ARGFL_UNLOAD, ARGFL_READONLY, ARGFL_TIME, ARGFL_NOCHECK */
};


/* parses (and applies) command-line arguments. returns 0 on success,
 * non-zero otherwise */
static int parseargv(struct argstruct *args) {
  int i, drivemapflag = 0;

  /* iterate through arguments, if any */
  for (i = 1; i < args->argc; i++) {
    char opt;
    /* is it a drive letter, like "x:"? */
    if ((args->argv[i][0] >= 'A') && (args->argv[i][1] == ':') && (args->argv[i][2] == 0)) {
      unsigned char ldrv;
      ldrv = DRIVETONUM(args->argv[i][0]);
      if (ldrv > 25) return(-2);
      if (drivemapflag != 0) return(-2); /* only one drive letter */
      glob_data.ldrv[ldrv] = 0; /* remote drive index 0 = the USB drive */
      drivemapflag = 1;
      continue;
    }
    /* not a drive letter -> is it an option? */
    if ((args->argv[i][0] == '/') || (args->argv[i][0] == '-')) {
      if ((args->argv[i][1] == 0) || (args->argv[i][2] != 0)) return(-3);
      opt = args->argv[i][1];
      /* normalize the option char to lower case */
      if ((opt >= 'A') && (opt <= 'Z')) opt += ('a' - 'A');
      /* what is the option about? */
      switch (opt) {
        case 'q':
          args->flags |= ARGFL_QUIET;
          break;
        case 'r':  /* read-only mapping */
          args->flags |= ARGFL_READONLY;
          break;
        case 'u':  /* unload PGDFS */
          args->flags |= ARGFL_UNLOAD;
          break;
        case 't':  /* push the clock and exit */
          args->flags |= ARGFL_TIME;
          break;
        case 'n':  /* undocumented: install without looking for the card
                    * (for testing the TSR mechanics in an emulator) */
          args->flags |= ARGFL_NOCHECK;
          break;
        default: /* invalid parameter (this includes /?) */
          return(-5);
      }
      continue;
    }
    /* neither a drive letter nor an option */
    return(-1);
  }

  /* fail if mapping+unload, mapping+time or unload+time */
  if (args->flags & (ARGFL_UNLOAD | ARGFL_TIME)) {
    if (drivemapflag != 0) return(-1);
    if ((args->flags & (ARGFL_UNLOAD | ARGFL_TIME)) == (ARGFL_UNLOAD | ARGFL_TIME)) return(-1);
    return(0);
  }

  /* did I get a drive letter? */
  if (drivemapflag == 0) return(-6);

  return(0);
}

/* allocates sz bytes of memory and returns the segment to allocated memory or
 * 0 on error. the allocation strategy is 'highest possible' (last fit) to
 * avoid memory fragmentation */
static unsigned short allocseg(unsigned short sz) {
  unsigned short volatile res = 0;
  /* sz should contains number of 16-byte paragraphs instead of bytes */
  sz += 15; /* make sure to allocate enough paragraphs */
  sz >>= 4;
  /* ask DOS for memory */
  _asm {
    push cx /* save cx */
    /* set strategy to 'last fit' */
    mov ah, 58h
    xor al, al  /* al = 0 means 'get strategy' */
    int 21h     /* now current strategy is in ax */
    mov cx, ax  /* copy current strategy to cx */
    mov ah, 58h
    mov al, 1   /* al = 1 means 'set strategy' */
    mov bl, 2   /* 2 or greater means 'last fit' */
    int 21h
    /* do the allocation now */
    mov ah, 48h     /* alloc memory (DOS 2+) */
    mov bx, sz      /* number of paragraphs to allocate */
    mov res, 0      /* pre-set res to failure (0) */
    int 21h         /* returns allocated segment in AX */
    /* check CF */
    jc failed
    mov res, ax     /* set res to actual result */
    failed:
    /* set strategy back to its initial setting */
    mov ah, 58h
    mov al, 1
    mov bx, cx
    int 21h
    pop cx    /* restore cx */
  }
  return(res);
}

/* free segment previously allocated through allocseg() */
static void freeseg(unsigned short segm) {
  _asm {
    mov ah, 49h   /* free memory (DOS 2+) */
    mov es, segm  /* put segment to free into ES */
    int 21h
  }
}

/* looks for the "PGdf" signature of inthandler() within the first 64 bytes
 * at ptr (its exact offset depends on the compiler's prologue). returns the
 * offset of the signature, or -1 if not found. */
static int findsig(unsigned char far *ptr) {
  int i;
  for (i = 0; i < 64; i++) {
    if ((ptr[i] == 'P') && (ptr[i+1] == 'G') && (ptr[i+2] == 'd') && (ptr[i+3] == 'f')) return(i);
  }
  return(-1);
}

/* patch the TSR routine so it uses my new DS.
 * return 0 on success, non-zero otherwise */
static int updatetsrds(void) {
  unsigned short newds;
  unsigned char far *ptr;
  unsigned short far *sptr;
  int sigoffs;
  newds = 0;
  _asm {
    push ds
    pop newds
  }

  /* patch the TSR routine: the word to patch is the immediate operand of
   * the "mov ax, 0" that follows the signature and the "push ax" */
  sigoffs = findsig((unsigned char far *)inthandler);
  if (sigoffs < 0) return(-1);
  ptr = (unsigned char far *)inthandler + sigoffs;
  sptr = (unsigned short far *)ptr;
  if ((ptr[4] != 0x50) || (ptr[5] != 0xB8)) return(-1); /* push ax / mov ax, imm16 */
  sptr[3] = newds;
  return(0);
}

/* scans the 2Fh interrupt for some available 'multiplex id' in the range
 * C0..FF. also checks for PGDFS presence at the same time. returns:
 *  - the available id if found
 *  - the id of the already-present pgdfs instance
 *  - 0 if no available id found
 * presentflag set to 0 if no pgdfs found loaded, non-zero otherwise. */
static unsigned char findfreemultiplex(unsigned char *presentflag) {
  unsigned char id = 0, freeid = 0, pflag = 0;
  _asm {
    mov id, 0C0h /* start scanning at C0h */
    checkid:
    xor al, al   /* subfunction is 'installation check' (00h) */
    mov ah, id
    int 2Fh
    /* is it free? (AL == 0) */
    test al, al
    jnz notfree    /* not free - is it me perhaps? */
    mov freeid, ah /* it's free - remember it, I may use it myself soon */
    jmp checknextid
    notfree:
    /* is it me? (AL=FF + BX=4750 CX=4644 ["PG" "DF"]) */
    cmp al, 0ffh
    jne checknextid
    cmp bx, 4750h
    jne checknextid
    cmp cx, 4644h
    jne checknextid
    /* if here, then it's me... */
    mov ah, id
    mov freeid, ah
    mov pflag, 1
    jmp gameover
    checknextid:
    /* if not me, then check next id */
    inc id
    jnz checkid /* if id is zero, then all range has been covered (C0..FF) */
    gameover:
  }
  *presentflag = pflag;
  return(freeid);
}

/* looks for a PicoGUS with PGDFS support: magic byte, protocol version, a
 * sane CMD_DFSMAXLEN and an enabled data port (CMD_DFSPORT != 0). prints a
 * message and returns non-zero on failure. on success sets glob_chunk
 * (largest payload per frame), xport_data_port and the CPU flag. */
static int detectcard(void) {
  unsigned short maxlen, port;
  unsigned char proto;

  xport_detect_cpu();

  if (xport_present() == 0) {
    outmsg(MSG_NOCARD);
    return(-1);
  }
  proto = xport_read8(CMD_PROTOCOL);
  if (proto < PICOGUS_PROTOCOL_VER) {
    outmsg(MSG_PROTO1);
    outdec(proto);
    outmsg(MSG_PROTO2);
    return(-1);
  }
  maxlen = xport_read16(CMD_DFSMAXLEN);
  port = xport_read16(CMD_DFSPORT);
  /* PGDFS switched off (pgusinit /dfsport 0): the card then reports a data
   * port of 0 and a maximum length of 0 as well, so look at both before
   * concluding that the firmware has no PGDFS at all */
  if ((port == 0) && (maxlen == 0)) {
    outmsg(MSG_DFSOFF);
    return(-1);
  }
  if ((maxlen < XPORT_MAXLEN_MIN) || (maxlen > XPORT_MAXLEN_MAX)) {
    outmsg(MSG_NOPGDFS);
    return(-1);
  }
  if (port == 0) {
    outmsg(MSG_DFSOFF);
    return(-1);
  }
  port &= 0xFFFEu; /* the window sits at an even base, bit 0 is ignored */
  if ((port < 0x100) || (port > 0x3FE)) {
    /* not a value this protocol produces: PGDFS firmware older than the
     * configurable data port answers FF00h for the unknown register */
    outmsg(MSG_NOPGDFS);
    return(-1);
  }
  xport_data_port = port;
  glob_chunk = XPORT_MAX_PAYLOAD;
  if (maxlen < glob_chunk) glob_chunk = maxlen;
  return(0);
}

/* reads the CMD_DFSINFO string ("LABEL|FS|MB|SERIAL") and prints it in a
 * friendly way. returns 0 if no drive is mounted, non-zero otherwise. */
static int printdriveinfo(void) {
  char info[80];
  char *label, *fs, *mb;
  int i;

  if (xport_info(info, sizeof(info)) == 0) {
    outmsg(MSG_NODRIVE);
    return(0);
  }
  /* split the string at the '|' separators */
  label = info;
  fs = mb = NULL;
  for (i = 0; info[i] != 0; i++) {
    if (info[i] != '|') continue;
    info[i] = 0;
    if (fs == NULL) {
      fs = info + i + 1;
    } else if (mb == NULL) {
      mb = info + i + 1;
    } else {
      break; /* the serial follows, I do not print it */
    }
  }
  outmsg(MSG_USBDRV);
  if (label[0] == 0) {
    outmsg(MSG_NOLABEL);
  } else {
    outstr(label);
  }
  if (fs != NULL) {
    outchar(' ');
    outchar('(');
    outstr(fs);
    if (mb != NULL) {
      outchar(',');
      outchar(' ');
      outstr(mb);
      outmsg(MSG_MB);
      return(1);
    }
    outchar(')');
  }
  outmsg(MSG_CRLF);
  return(1);
}

/* reads the DOS clock and sends it to the card (CMD_DFSTIME) */
static void pushtime(void) {
  unsigned short t, d;
  xport_getdostime(&t, &d);
  xport_settime(t, d);
}

int main(int argc, char **argv) {
  struct argstruct args;
  struct cdsstruct far *cds;
  unsigned char tmpflag = 0;
  int i;
  unsigned short volatile newdataseg; /* 'volatile' just in case the compiler would try to optimize it out, since I set it through in-line assembly */

  /* set all drive mappings as 'unused' */
  for (i = 0; i < 26; i++) glob_data.ldrv[i] = 0xff;

  /* parse command-line arguments */
  zerobytes(&args, sizeof(args));
  args.argc = argc;
  args.argv = argv;
  if (parseargv(&args) != 0) {
    outmsg(MSG_HELP);
    return(1);
  }

  /* check DOS version - I require DOS 5.0+ */
  _asm {
    mov ax, 3306h
    int 21h
    mov tmpflag, bl
    inc al /* if AL was 0xFF ("unsupported function"), it is 0 now */
    jnz done
    mov tmpflag, 0 /* if AL is 0 (hence was 0xFF), set dosver to 0 */
    done:
  }
  if (tmpflag < 5) { /* tmpflag contains DOS version or 0 for 'unknown' */
    outmsg(MSG_UNSUPDOS);
    return(1);
  }

  /* is it all about pushing the clock to the card? this needs no TSR and
   * works whether or not one is loaded */
  if ((args.flags & ARGFL_TIME) != 0) {
    if (detectcard() != 0) return(1);
    pushtime();
    if ((args.flags & ARGFL_QUIET) == 0) outmsg(MSG_TIMESET);
    return(0);
  }

  /* look whether or not it's ok to install a network redirector at int 2F */
  _asm {
    mov tmpflag, 0
    mov ax, 1100h
    int 2Fh
    dec ax /* if AX was set to 1 (ie. "not ok to install"), it's zero now */
    jnz goodtogo
    mov tmpflag, 1
    goodtogo:
  }
  if (tmpflag != 0) {
    outmsg(MSG_NOREDIR);
    return(1);
  }

  /* is it all about unloading myself? */
  if ((args.flags & ARGFL_UNLOAD) != 0) {
    unsigned char pgdfsid;
    unsigned short myseg, myoff, mydataseg;
    struct tsrshareddata far *tsrdata;
    unsigned char far *int2fptr;

    /* am I loaded at all? */
    pgdfsid = findfreemultiplex(&tmpflag);
    if (tmpflag == 0) { /* not loaded, cannot unload */
      outmsg(MSG_NOTLOAD);
      return(1);
    }
    /* am I still at the top of the int 2Fh chain? */
    _asm {
      /* save AX, BX and ES */
      push ax
      push bx
      push es
      /* fetch int vector */
      mov ax, 352Fh  /* AH=35h 'GetVect' for int 2Fh */
      int 21h
      mov myseg, es
      mov myoff, bx
      /* restore AX, BX and ES */
      pop es
      pop bx
      pop ax
    }
    int2fptr = (unsigned char far *)MK_FP(myseg, myoff);
    /* look for the "PGdf" signature */
    if (findsig(int2fptr) < 0) {
      outmsg(MSG_OTHERTSR);
      return(1);
    }
    /* get the ptr to TSR's data */
    _asm {
      push ax
      push bx
      push cx
      pushf
      mov ah, pgdfsid
      mov al, 1
      mov cx, 4750h
      mov myseg, 0ffffh
      int 2Fh /* AX should be 0, and BX:CX contains the address */
      test ax, ax
      jnz fail
      mov myseg, bx
      mov myoff, cx
      fail:
      popf
      pop cx
      pop bx
      pop ax
    }
    if (myseg == 0xffffu) {
      outmsg(MSG_TSRCOMFA);
      return(1);
    }
    tsrdata = MK_FP(myseg, myoff);
    mydataseg = myseg;
    /* restore previous int 2f handler (under DS:DX, AH=25h, INT 21h)*/
    myseg = tsrdata->prev_2f_handler_seg;
    myoff = tsrdata->prev_2f_handler_off;
    _asm {
      /* save AX, DS and DX */
      push ax
      push ds
      push dx
      /* set DS:DX */
      mov ax, myseg
      push ax
      pop ds
      mov dx, myoff
      /* call INT 21h,25h for int 2Fh */
      mov ax, 252Fh
      int 21h
      /* restore AX, DS and DX */
      pop dx
      pop ds
      pop ax
    }
    /* set all mapped drives as 'not available' */
    for (i = 0; i < 26; i++) {
      if (tsrdata->ldrv[i] == 0xff) continue;
      cds = getcds(i);
      if (cds != NULL) cds->flags = 0;
    }
    /* free TSR's data/stack seg and its PSP */
    freeseg(mydataseg);
    freeseg(tsrdata->pspseg);
    /* all done */
    if ((args.flags & ARGFL_QUIET) == 0) {
      outmsg(MSG_UNLOADED);
    }
    return(0);
  }

  /* is there a PicoGUS with PGDFS support? (also sets glob_chunk, the data
   * port and the CPU flag, all of which live in the data segment that is
   * copied to the resident block later on; /N keeps the default port) */
  if ((args.flags & ARGFL_NOCHECK) == 0) {
    if (detectcard() != 0) return(1);
  } else {
    xport_detect_cpu();
    glob_chunk = XPORT_MAX_PAYLOAD;
  }

  /* remember current int 2f handler, we might over-write it soon (also I
   * use it to see if I'm already loaded) */
  _asm {
    mov ax, 352fh; /* AH=GetVect AL=2F */
    push es /* save ES and BX (will be overwritten) */
    push bx
    int 21h
    mov word ptr [glob_data + GLOB_DATOFF_PREV2FHANDLERSEG], es
    mov word ptr [glob_data + GLOB_DATOFF_PREV2FHANDLEROFF], bx
    pop bx
    pop es
  }

  /* is the TSR installed already? */
  glob_multiplexid = findfreemultiplex(&tmpflag);
  if (tmpflag != 0) { /* already loaded */
    outmsg(MSG_ALRLOAD);
    return(1);
  } else if (glob_multiplexid == 0) { /* no free multiplex id found */
    outmsg(MSG_NOMULTPX);
    return(1);
  }

  /* if any of the to-be-mapped drives is already active, fail */
  for (i = 0; i < 26; i++) {
    if (glob_data.ldrv[i] == 0xff) continue;
    cds = getcds(i);
    if (cds == NULL) {
      outmsg(MSG_MAPFAIL);
      return(1);
    }
    if (cds->flags != 0) {
      outmsg(MSG_DRVACTIV);
      return(1);
    }
  }

  /* tell the card what time it is, and show what is plugged in */
  if ((args.flags & ARGFL_NOCHECK) == 0) {
    pushtime();
    if ((args.flags & ARGFL_QUIET) == 0) printdriveinfo();
  }

  /* read-only mapping? */
  if ((args.flags & ARGFL_READONLY) != 0) glob_readonly = 1;

  /* allocate a new segment for all my internal needs, and use it right away
   * as DS */
  newdataseg = allocseg(DATASEGSZ);
  if (newdataseg == 0) {
    outmsg(MSG_MEMFAIL);
    return(1);
  }

  /* copy current DS into the new segment and switch to new DS/SS */
  _asm {
    /* save registers on the stack */
    push es
    push cx
    push si
    push di
    pushf
    /* copy the memory block */
    mov cx, DATASEGSZ  /* copy cx bytes */
    xor si, si         /* si = 0*/
    xor di, di         /* di = 0 */
    cld                /* clear direction flag (increment si/di) */
    mov es, newdataseg /* load es with newdataseg */
    rep movsb          /* execute copy DS:SI -> ES:DI */
    /* restore registers (but NOT es, instead save it into AX for now) */
    popf
    pop di
    pop si
    pop cx
    pop ax
    /* switch to the new DS _AND_ SS now */
    push es
    push es
    pop ds
    pop ss
    /* restore ES */
    push ax
    pop es
  }

  /* patch the TSR so it uses my new DS */
  if (updatetsrds() != 0) {
    outmsg(MSG_RELFAIL);
    freeseg(newdataseg);
    return(1);
  }

  /* remember the SDA address (will be useful later) */
  glob_sdaptr = getsda();

  /* set all drives as being 'network' drives (also add the PHYSICAL bit,
   * otherwise MS-DOS 6.0 will ignore the drive) */
  for (i = 0; i < 26; i++) {
    if (glob_data.ldrv[i] == 0xff) continue;
    cds = getcds(i);
    cds->flags = CDSFLAG_NET | CDSFLAG_PHY;
    /* set 'current path' to root, to avoid inheriting any garbage */
    cds->current_path[0] = 'A' + i;
    cds->current_path[1] = ':';
    cds->current_path[2] = '\\';
    cds->current_path[3] = 0;
  }

  if ((args.flags & ARGFL_QUIET) == 0) {
    unsigned long residentsz;
    outmsg(MSG_INSTLLED);
    for (i = 0; i < 26; i++) {
      if (glob_data.ldrv[i] == 0xff) continue;
      outchar('A' + i);
      outchar(':');
    }
    outmsg(MSG_INSTL2);
    if (glob_readonly != 0) outmsg(MSG_RDONLY);
    outmsg(MSG_DATAPORT);
    outhex(xport_data_port);
    outchar('h');
    /* resident size = PSP + resident code (rounded up to paragraphs) + the
     * data/stack segment (rounded up to paragraphs) */
    residentsz = ((FP_OFF(begtextend) + 256 + 15) & 0xFFF0u) + ((DATASEGSZ + 15) & 0xFFF0u);
    outmsg(MSG_RESIDENT);
    outdec(residentsz);
    outmsg(MSG_BYTES);
  }

  /* get the segment of the PSP (might come handy later) */
  _asm {
    mov ah, 62h          /* get current PSP address */
    int 21h              /* returns the segment of PSP in BX */
    mov word ptr [glob_data + GLOB_DATOFF_PSPSEG], bx  /* copy PSP segment to glob_pspseg */
  }

  /* free the environment (env segment is at offset 2C of the PSP) */
  _asm {
    mov es, word ptr [glob_data + GLOB_DATOFF_PSPSEG] /* load ES with PSP's segment */
    mov es, es:[2Ch]    /* get segment of the env block */
    mov ah, 49h         /* free memory (DOS 2+) */
    int 21h
  }

  /* set up the TSR (INT 2F catching) */
  _asm {
    cli
    mov ax, 252fh /* AH=set interrupt vector  AL=2F */
    push ds /* preserve DS and DX */
    push dx
    push cs /* set DS to current CS, that is provide the */
    pop ds  /* int handler's segment */
    mov dx, offset inthandler /* int handler's offset */
    int 21h
    pop dx /* restore DS and DX to previous values */
    pop ds
    sti
  }

  /* Turn self into a TSR and free memory I won't need any more. That is, I
   * free all the libc startup code and my init functions by passing the
   * number of paragraphs to keep resident to INT 21h, AH=31h. How to compute
   * the number of paragraphs? Simple: look at the memory map and note down
   * the size of the BEGTEXT segment (that's where I store all TSR routines).
   * then: (sizeof(BEGTEXT) + sizeof(PSP) + 15) / 16
   * PSP is 256 bytes of course. And +15 is needed to avoid truncating the
   * last (partially used) paragraph. */
  _asm {
    mov ax, 3100h  /* AH=31 'terminate+stay resident', AL=0 exit code */
    mov dx, offset begtextend /* DX = offset of resident code end     */
    add dx, 256    /* add size of PSP (256 bytes)                     */
    add dx, 15     /* add 15 to avoid truncating last paragraph       */
    shr dx, 1      /* convert bytes to number of 16-bytes paragraphs  */
    shr dx, 1      /* the 8086/8088 CPU supports only a 1-bit version */
    shr dx, 1      /* of SHR, so I have to repeat it as many times as */
    shr dx, 1      /* many bits I need to shift.                      */
    int 21h
  }

  return(0); /* never reached, but compiler complains if not present */
}
