/*
 * This file is part of the etherdfs project.
 * http://etherdfs.sourceforge.net
 *
 * Copyright (C) 2017 Mateusz Viste
 *
 * Contains definitions of DOS structures used by etherdfs.
 *
 * Modified for PicoGUS PGDFS (2026): unchanged apart from this note.
 * Distributed under the MIT license, see LICENSE.
 */

#ifndef DOSSTRUCTS_SENTINEL
#define DOSSTRUCTS_SENTINEL


/* make sure structs are packed tightly (required since that's how DOS packs its CDS) */
#pragma pack(1)


/* CDS (current directory structure), as used by DOS 4+ */
#define CDSFLAG_SUB 0x1000u  /* SUBST drive */
#define CDSFLAG_JOI 0x2000u  /* JOINed drive */
#define CDSFLAG_PHY 0x4000u  /* Physical drive */
#define CDSFLAG_NET 0x8000u  /* Network drive */
struct cdsstruct {
  unsigned char current_path[67]; /* current path */
  unsigned short flags; /* indicates whether the drive is physical, networked, substed or joined*/
  unsigned char far *dpb; /* a pointer to the Drive Parameter Block */
  union {
    struct { /* used for local disks */
      unsigned short start_cluster;
      unsigned long unknown;
    } LOCAL;
    struct { /* used for network disks */
      unsigned long redirifs_record_ptr;
      unsigned short parameter;
    } NET;
  } u;
  unsigned short backslash_offset; /* offset in current_path of '\' (always 2, unless it's a SUBST drive) */
  /* DOS 4 and newer have 7 extra bytes here */
  unsigned char f2[7];
}; /* 88 bytes total */


/* called 'srchrec' in phantom.c */
struct sdbstruct {
  unsigned char drv_lett;
  unsigned char srch_tmpl[11];
  unsigned char srch_attr;
  unsigned short dir_entry;
  unsigned short par_clstr;
  unsigned char f1[4];
};


struct foundfilestruct {
  unsigned char fname[11];
  unsigned char fattr; /* (1=RO 2=HID 4=SYS 8=VOL 16=DIR 32=ARCH 64=DEVICE) */
  unsigned char f1[10];
  unsigned short time_lstupd; /* 16 bits: hhhhhmmm mmmsssss */
  unsigned short date_lstupd; /* 16 bits: YYYYYYYM MMMDDDDD */
  unsigned short start_clstr; /* (optional) */
  unsigned long fsize;
};


/*
 * Pointers to SDA fields. Layout:
 *                             DOS4+   DOS 3, DR-DOS
 * DTA ptr                      0Ch     0Ch
 * First filename buffer        9Eh     92h
 * Search data block (SDB)     19Eh    192h
 * Dir entry for found file    1B3h    1A7h
 * Search attributes           24Dh    23Ah
 * File access/sharing mode    24Eh    23Bh
 * Ptr to current CDS          282h    26Ch
 * Extended open mode          2E1h    Not supported
 *
 * The struct below is matching FreeDOS and MSDOS 4+
 */
struct sdastruct {
  unsigned char f0[12];
  unsigned char far *curr_dta;
  unsigned char f1[32];
  unsigned char dd;
  unsigned char mm;
  unsigned short yy_1980;
  unsigned char f2[106];
  unsigned char fn1[128];
  unsigned char fn2[128];
  struct sdbstruct sdb;
  struct foundfilestruct found_file;
  struct cdsstruct drive_cdscopy; /* 88 bytes total */
  unsigned char fcb_fn1[11];
  unsigned char f3;
  unsigned char fcb_fn2[11];
  unsigned char f4[11];
  unsigned char srch_attr;
  unsigned char open_mode;
  unsigned char f5[51];
  unsigned char far *drive_cdsptr;
  unsigned char f6[12];
  unsigned short fn1_csofs;
  unsigned short fn2_csofs;
  unsigned char f7[71];
  unsigned short spop_act;
  unsigned short spop_attr;
  unsigned short spop_mode;
  unsigned char f8[29];
  struct {
    unsigned char drv_lett;
    unsigned char srch_tmpl[11];
    unsigned char srch_attr;
    unsigned short dir_entry;
    unsigned short par_clstr;
    unsigned char f1[4];
  } ren_srcfile;
  struct {
    unsigned char fname[11];
    unsigned char fattr; /* (1=RO 2=HID 4=SYS 8=VOL 16=DIR 32=ARCH 64=DEVICE) */
    unsigned char f1[10];
    unsigned short time_lstupd; /* 16 bits: hhhhhmmm mmmsssss */
    unsigned short date_lstupd; /* 16 bits: YYYYYYYM MMMDDDDD */
    unsigned short start_clstr; /* (optional) */
    unsigned long fsize;
  } ren_file;
};


/* DOS System File Table entry - ALL DOS VERSIONS
 * Some of the fields below are defined by the redirector, and differ
 * from the SFT normally found under DOS */
struct sftstruct {
  unsigned int handle_count;  /* count of handles referring to this file */
  unsigned int open_mode;     /* open mode, bit 15 set if opened via FCB */
  unsigned char file_attr;    /* file attributes */
  unsigned int dev_info_word; /* device info word */
  unsigned char far *dev_drvr_ptr; /* ??? */
  unsigned int start_sector; /* starting cluster of file */
  unsigned long file_time;   /* file date and time */
  unsigned long file_size;   /* file length */
  unsigned long file_pos;    /* current file position */
  unsigned int rel_sector;
  unsigned int abs_sector;
  unsigned int dir_sector;
  unsigned char dir_entry_no;
  char file_name[11];
};

#endif
