/*
 * PGDFS - PicoGUS DOS file system: ISA I/O port transport
 *
 * Copyright (C) 2026 PicoGUS contributors
 * Distributed under the MIT license, see LICENSE.
 *
 * See XPORT.H for the interface and sw/dfs/PROTOCOL.md for the protocol.
 *
 * RESIDENT CODE RULES (this file is #included into the BEGTEXT segment of
 * PGUSDFS.C): no libc calls of any kind, no string literals, only static
 * initializers that need no startup code (plain constants), no stack checks
 * (-s). inp()/outp() are compiler intrinsics and compile to IN/OUT
 * instructions. The code must run on an 8086, so REP INSW/OUTSW (80186+)
 * are only used after xport_detect_cpu() said so, and they are emitted as
 * raw bytes because the assembler rejects them with -0.
 */

#include <conio.h>  /* inp() / outp() / inpw() */
#include <i86.h>    /* MK_FP() */
#include "xport.h"

/* force IN/OUT instructions instead of libc calls, whatever the -o options */
#pragma intrinsic(inp, inpw, outp, outpw)

unsigned char xport_cpu186;
unsigned char xport_last_status;
unsigned char xport_retried;
unsigned short xport_data_port = DFS_DEFAULT_DATA_PORT;

unsigned char xport_detect_cpu(void) {
  unsigned char r = 0;
  /* the 8086/8088 push SP after decrementing it, every later CPU pushes the
   * value SP had before the push */
  _asm {
    push sp
    pop ax
    cmp ax, sp
    jne is8086
    mov r, 1
    is8086:
  }
  xport_cpu186 = r;
  return(r);
}

void xport_select(unsigned char reg) {
  outp(CONTROL_PORT, 0xCC); /* knock on the door... */
  outp(CONTROL_PORT, reg);  /* ...and select the register */
}

unsigned char xport_read8(unsigned char reg) {
  xport_select(reg);
  return(inp(DATA_PORT_HIGH));
}

unsigned short xport_read16(unsigned char reg) {
  xport_select(reg);
  return(inpw(DATA_PORT_LOW));
}

int xport_present(void) {
  return(xport_read8(CMD_MAGIC) == 0xDD);
}

unsigned short xport_ticks(void) {
  return(*(unsigned short volatile far *)MK_FP(0x40, 0x6C));
}

/* The data window is two consecutive ports that feed the same byte stream,
 * so a 16-bit OUT/IN at the (even) base moves two stream bytes: the bus
 * splits it into a byte cycle at port (low byte, stream byte n) and one at
 * port+1 (high byte, stream byte n+1). That halves the CPU work per byte
 * compared to byte transfers. n/2 words go out as words, an odd last byte
 * as one byte access to the base port. */
void xport_out_bytes(const unsigned char *src, unsigned short n) {
  unsigned short port = xport_data_port;
  unsigned short words = n >> 1;
  if (n == 0) return;
  if (words != 0) {
    if (xport_cpu186 != 0) {
      _asm {
        mov si, src
        mov cx, words
        mov dx, port
        cld
        db 0F3h, 6Fh   /* rep outsw (DS:SI -> port DX, word by word) */
      }
    } else {
      /* OUT DX,AX on an 8086/8088 performs two byte cycles: AL to port DX,
       * then AH to port DX+1, which is exactly the stream order */
      _asm {
        mov si, src
        mov cx, words
        mov dx, port
        cld
        outwnext:
        lodsw
        out dx, ax
        loop outwnext
      }
    }
  }
  if (n & 1) outp(port, src[n - 1]);
}

void xport_in_bytes(unsigned char *dst, unsigned short n) {
  unsigned short port = xport_data_port;
  unsigned short words = n >> 1;
  if (n == 0) return;
  if (words != 0) {
    if (xport_cpu186 != 0) {
      _asm {
        push es
        push ds
        pop es
        mov di, dst
        mov cx, words
        mov dx, port
        cld
        db 0F3h, 6Dh   /* rep insw (port DX -> ES:DI, word by word) */
        pop es
      }
    } else {
      /* IN AX,DX on an 8086/8088 reads port DX into AL, then DX+1 into AH */
      _asm {
        push es
        push ds
        pop es
        mov di, dst
        mov cx, words
        mov dx, port
        cld
        inwnext:
        in ax, dx
        stosw
        loop inwnext
        pop es
      }
    }
  }
  if (n & 1) dst[n - 1] = inp(port);
}

unsigned char xport_status(void) {
  xport_select(CMD_DFSSTAT);
  xport_last_status = inp(DATA_PORT_HIGH);
  return(xport_last_status);
}

void xport_abort(void) {
  xport_select(CMD_DFSSTAT);
  outp(DATA_PORT_HIGH, 0); /* any write to CMD_DFSSTAT aborts */
}

int xport_transact(unsigned char *buf, unsigned short bufsize) {
  unsigned short len, start;
  unsigned long polls;
  unsigned char st;
  int attempt;

  xport_retried = 0;
  len = buf[0] | (buf[1] << 8);
  if ((len < DFS_HDR_LEN) || (len > bufsize)) return(XPORT_TOOLONG);

  for (attempt = 0;; attempt++) {
    /* open the request buffer and stream the whole frame in */
    xport_select(CMD_DFSREQ);
    xport_out_bytes(buf, len);
    /* execute */
    xport_select(CMD_DFSEXEC);
    outp(DATA_PORT_HIGH, 1);
    /* poll the status until READY, ABORTED, NODRIVE or timeout. the poll
     * counter is a backstop for the (abnormal) case where interrupts are
     * off and the BIOS tick counter does not advance. */
    xport_select(CMD_DFSSTAT);
    start = xport_ticks();
    polls = 0;
    for (;;) {
      unsigned short now;
      st = inp(DATA_PORT_HIGH);
      if ((st == DFS_STATUS_READY) || (st == DFS_STATUS_ABORTED) || (st == DFS_STATUS_NODRIVE)) break;
      polls++;
      now = xport_ticks();
      if (now < start) start = now; /* the BIOS resets the tick count at midnight */
      if (((unsigned short)(now - start) >= XPORT_TIMEOUT_TICKS) || ((polls & 0x00FFFFFFul) == 0)) {
        xport_last_status = st;
        xport_abort();
        return(XPORT_TIMEOUT);
      }
    }
    xport_last_status = st;
    if (st == DFS_STATUS_READY) break;
    if (st == DFS_STATUS_NODRIVE) return(XPORT_NODRIVE);
    /* ABORTED: the request buffer is still intact (nothing has been read
     * back yet), so send it once more */
    if (attempt != 0) return(XPORT_ABORTED);
    xport_retried = 1;
  }

  /* fetch the answer: header first, then as many payload bytes as it says */
  xport_select(CMD_DFSRESP);
  xport_in_bytes(buf, DFS_HDR_LEN);
  len = buf[0] | (buf[1] << 8);
  if ((len < DFS_HDR_LEN) || (len > bufsize)) {
    xport_abort();
    return(XPORT_BADLEN);
  }
  xport_in_bytes(buf + DFS_HDR_LEN, len - DFS_HDR_LEN);
  return(XPORT_OK);
}

unsigned short xport_info(char *dst, unsigned short max) {
  unsigned short i = 0, n = 0;
  unsigned char c;
  xport_select(CMD_DFSINFO);
  /* the string is at most 255 bytes; reading the terminator rewinds the
   * card's pointer, selecting the register again rewinds it as well */
  for (n = 0; n < 255; n++) {
    c = inp(DATA_PORT_HIGH);
    if ((c == 0) || (c == 0xFF)) break; /* 0xFF: nobody home */
    if (i + 1 < max) dst[i++] = c;
  }
  if (max != 0) dst[i] = 0;
  return(i);
}

void xport_settime(unsigned short dostime, unsigned short dosdate) {
  xport_select(CMD_DFSTIME);
  outp(DATA_PORT_HIGH, dostime & 0xFF);
  outp(DATA_PORT_HIGH, dostime >> 8);
  outp(DATA_PORT_HIGH, dosdate & 0xFF);
  outp(DATA_PORT_HIGH, dosdate >> 8);
}

void xport_getdostime(unsigned short *dostime, unsigned short *dosdate) {
  /* (variable names avoid register and directive names like ss, dd) */
  unsigned char hr = 0, mn = 0, sc = 0, mo = 1, dy = 1;
  unsigned short yr = 1980;
  _asm {
    mov ah, 2Ch  /* get system time: CH=hour CL=min DH=sec DL=1/100 */
    int 21h
    mov hr, ch
    mov mn, cl
    mov sc, dh
    mov ah, 2Ah  /* get system date: CX=year DH=month DL=day */
    int 21h
    mov yr, cx
    mov mo, dh
    mov dy, dl
  }
  if (yr < 1980) yr = 1980;
  *dostime = (hr << 11) | (mn << 5) | (sc >> 1);
  *dosdate = ((yr - 1980) << 9) | (mo << 5) | dy;
}
