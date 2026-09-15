/*
 * This file is part of the etherdfs project.
 * http://etherdfs.sourceforge.net
 *
 * Copyright (C) 2017 Mateusz Viste
 *
 * Contains all global variables used by etherdfs.
 *
 * Modified for PicoGUS PGDFS (2026): the packet driver buffers and state are
 * gone, replaced by the single request/answer frame buffer of the PicoGUS
 * transport plus the chunk size, the read-only flag and the CPU flag.
 * Distributed under the MIT license, see LICENSE.
 */

#ifndef GLOBALS_SENTINEL
#define GLOBALS_SENTINEL

/* required size (in bytes) of the data segment - this must be bigh enough as
 * to accomodate all "DATA" segments AND the stack, which will be located at
 * the very end of the data segment. The resident stack is used by the INT 2F
 * handler and by any hardware interrupt that fires while it runs - 1K should
 * be safe... It is important that DATASEGSZ can contain a stack of AT LEAST
 * the size of the stack used by the transient code, since the transient part
 * of the program will switch to it and expects the stack to not become
 * corrupted in the process. Check the DGROUP size in the map file after
 * every change: DATASEGSZ must be at least that big. */
#define DATASEGSZ 5888

/* a few globals useful only for debug messages */
#if DEBUGLEVEL > 0
static unsigned short dbg_xpos = 0;
static unsigned short far *dbg_VGA = (unsigned short far *)(0xB8000000l);
static unsigned char dbg_hexc[16] = "0123456789ABCDEF";
#define dbg_startoffset 80*16
#endif

/* whenever the tsrshareddata structure changes, offsets below MUST be
 * adjusted (these are required by assembly routines) */
#define GLOB_DATOFF_PREV2FHANDLERSEG 0
#define GLOB_DATOFF_PREV2FHANDLEROFF 2
#define GLOB_DATOFF_PSPSEG 4
static struct tsrshareddata {
/*offs*/
/*  0 */ unsigned short prev_2f_handler_seg; /* seg:off of the previous 2F handler */
/*  2 */ unsigned short prev_2f_handler_off; /* (so I can call it for all queries  */
                                            /* that do not relate to my drive     */
/*  4 */ unsigned short pspseg;    /* segment of the program's PSP block */

         unsigned char ldrv[26]; /* local to remote drives mappings (0=A:, 1=B, etc */
} glob_data;

/* the one and only frame buffer: the request is built here (4-byte header
 * followed by the payload) and the answer overwrites it, exactly as on the
 * card. FRAMESIZE is defined in pgusdfs.c. */
static unsigned char glob_frame[FRAMESIZE];

/* largest payload that goes into one frame: min(4096, CMD_DFSMAXLEN). READ
 * and WRITE requests are chunked to fit. set at install time. */
static unsigned short glob_chunk;

/* non-zero when the drive is mapped read-only (/R): everything that would
 * modify the drive is refused with "access denied" before talking to the card */
static unsigned char glob_readonly;

static unsigned char glob_reqdrv;  /* the requested drive, set by the INT 2F *
                                    * handler and read by process2f()        */

static unsigned short glob_reqstkword; /* WORD saved from the stack (used by SETATTR) */
static struct sdastruct far *glob_sdaptr; /* pointer to DOS SDA (set by main() at *
                                           * startup, used later by process2f()   */

/* seg:off addresses of the old (DOS) stack */
static unsigned short glob_oldstack_seg;
static unsigned short glob_oldstack_off;

/* the INT 2F "multiplex id" registerd by PGDFS */
static unsigned char glob_multiplexid;

/* an INTPACK structure used to store registers as set when INT2F is called */
static union INTPACK glob_intregs;


#endif
