/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Generic CD-ROM drive core header.
 *
 * Authors: Miran Grca, <mgrca8@gmail.com>,
 *          Kevin Moonlight, <me@yyzkevin.com>
 *          Ian Scott, <ian@polpo.org>
 *
 *          Copyright 2016-2019 Miran Grca.
 *          Copyright (C) 2024 Kevin Moonlight
 *          Copyright (C) 2025 Ian Scott
 *          Copyright (C) 2025 Daniel Arnold
 */

#ifndef EMU_CDROM_H
#define EMU_CDROM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CD_STATUS_EMPTY             0
#define CD_STATUS_DATA_ONLY         1
#define CD_STATUS_PAUSED            2
#define CD_STATUS_PLAYING           3
#define CD_STATUS_STOPPED           4
#define CD_STATUS_PLAYING_COMPLETED 5

/* Medium changed flag. */
#define CD_STATUS_MEDIUM_CHANGED 0x80

#define CD_TRACK_AUDIO           0x08
#define CD_TRACK_MODE2           0x04

#define CD_READ_DATA             0
#define CD_READ_AUDIO            1
#define CD_READ_RAW              2

#define CD_TOC_NORMAL            0
#define CD_TOC_SESSION           1
#define CD_TOC_RAW               2

#define CD_IMAGE_HISTORY         4

// #define BUF_SIZE                 32768
#define BUF_SIZE                 4096

#define CDROM_IMAGE              200

#define RAW_SECTOR_SIZE     2352
#define SAMPLES_PER_SECTOR  1176

#define STAT_READY	    0x01
#define STAT_PLAY  	    0x08
#define STAT_ERROR	    0x10
#define STAT_DISK	    0x40
#define STAT_TRAY	    0x80

/* This is so that if/when this is changed to something else,
   changing this one define will be enough. */
#define CDROM_EMPTY !dev->host_drive

#include "../../common/picogus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* To shut up the GCC compilers. */
struct cdrom;

typedef enum
{
    CD_COMMAND_NONE,
    CD_COMMAND_IMAGE_LOAD,
    CD_COMMAND_IMAGE_LOAD_INDEX,
    CD_COMMAND_IMAGE_EJECT,
    CD_COMMAND_IMAGE_LIST
} cdrom_image_command_t;

/*
typedef enum
{
    CD_STATUS_ERROR = -1,
    CD_STATUS_IDLE,
    CD_STATUS_BUSY,
    CD_STATUS_READY,
} cdrom_image_status_t;
*/

typedef struct subchannel_t {
    uint8_t attr;
    uint8_t track;
    uint8_t index;
    uint8_t abs_m;
    uint8_t abs_s;
    uint8_t abs_f;
    uint8_t rel_m;
    uint8_t rel_s;
    uint8_t rel_f;
} subchannel_t;

typedef struct track_info_t {
    int     number;
    uint8_t attr;
    uint8_t m;
    uint8_t s;
    uint8_t f;
} track_info_t;

/* Define the various CD-ROM drive operations (ops). */
typedef struct cdrom_ops_t {
    void (*get_tracks)(struct cdrom *dev, int *first, int *last);
    void (*get_track_info)(struct cdrom *dev, uint32_t track, int end, track_info_t *ti);
    void (*get_subchannel)(struct cdrom *dev, uint32_t lba, subchannel_t *subc);
    int (*is_track_pre)(struct cdrom *dev, uint32_t lba);
    int (*sector_size)(struct cdrom *dev, uint32_t lba);
    int (*read_sector)(struct cdrom *dev, int type, uint8_t *b, uint32_t lba);
    int (*track_type)(struct cdrom *dev, uint32_t lba);
    void (*exit)(struct cdrom *dev);
} cdrom_ops_t;


typedef struct cdrom_fifo_t {
    uint8_t *data;
    uint16_t size;
    volatile uint16_t head;
    volatile uint16_t tail;
} cdrom_fifo_t;


#if USE_CD_AUDIO_FIFO
#include "../audio_fifo.h"
#endif

typedef struct cdrom {
    uint8_t id;

    cdrom_fifo_t info_fifo;
    cdrom_fifo_t data_fifo;
    
    volatile uint8_t req_m;
    volatile uint8_t req_s;
    volatile uint8_t req_f;
    volatile uint8_t req_cur;
    volatile uint8_t req_total;

    uint8_t media_changed;        
    cdrom_image_command_t image_command;
    cdrom_image_status_t image_status;
    uint8_t image_data;
    uint8_t disk_loaded;

    uint8_t cd_status; /* Struct variable reserved for
                          media status. */
    uint8_t speed;
    uint8_t cur_speed;


    uint8_t errors[8];
    
    int   is_dir;
    void *priv;

    char **image_list;
    int image_count;
    char image_path[128];

    uint32_t sound_on;
    uint32_t cdrom_capacity;
    volatile uint32_t seek_pos;
    volatile uint32_t seek_diff;
    uint32_t cd_end;
    uint32_t type;

    int host_drive;
    int prev_host_drive;
    volatile int cd_buflen;
    int audio_op;
    int audio_muted_soft;

    const cdrom_ops_t *ops;

    void *image;

    void (*insert)(void *priv);
    void (*close)(void *priv);
    uint32_t (*get_volume)(void *p, int channel);
    uint32_t (*get_channel)(void *p, int channel);

    const char *error_str;

    // int16_t cd_buffer[BUF_SIZE];
    uint8_t audio_sector_buffer[RAW_SECTOR_SIZE];
    int16_t *current_sector_samples;       // Convenience pointer: (int16_t*)audio_sector_buffer
    int audio_sector_total_samples;        // Samples available in current_sector_samples after a read
    int audio_sector_consumed_samples;     // Samples consumed from current_sector_samples

#if USE_CD_AUDIO_FIFO
    audio_fifo_t audio_fifo;
#endif
} cdrom_t;

extern cdrom_t cdrom;

extern void cdrom_error(cdrom_t *dev,uint8_t code);
extern void cdrom_read_errors(cdrom_t *dev,uint8_t *b);
extern uint8_t cdrom_has_errors(cdrom_t *dev);

void cdrom_fifo_init(cdrom_fifo_t *fifo,uint16_t size);
void cdrom_fifo_write(cdrom_fifo_t *fifo,uint8_t val);
void cdrom_fifo_write_multiple(cdrom_fifo_t *fifo, const uint8_t *data, size_t data_size);
uint16_t cdrom_fifo_level(cdrom_fifo_t *fifo);
uint8_t cdrom_fifo_read(cdrom_fifo_t *fifo);
void cdrom_fifo_clear(cdrom_fifo_t *fifo);

void cdrom_read_data(cdrom_t *dev);
void cdrom_tasks(cdrom_t *dev);

void cdrom_output_status(cdrom_t *dev);
uint8_t cdrom_status(cdrom_t *dev);

#if USE_CD_AUDIO_FIFO
extern audio_fifo_t* cdrom_audio_fifo_peek(cdrom_t *dev);
void cdrom_audio_fifo_init(cdrom_t *dev);
#endif

extern int     cdrom_lba_to_msf_accurate(int lba);
extern double  cdrom_seek_time(cdrom_t *dev);
extern void    cdrom_stop(cdrom_t *dev);
extern int     cdrom_is_pre(cdrom_t *dev, uint32_t lba);
extern bool    cdrom_audio_callback(cdrom_t *dev, uint32_t len);
extern uint32_t cdrom_audio_callback_simple(cdrom_t *dev, int16_t *buffer, uint32_t len, bool pad);

extern uint8_t cdrom_audio_track_search(cdrom_t *dev, uint32_t pos, int type, uint8_t playbit);
extern uint8_t cdrom_audio_track_search_pioneer(cdrom_t *dev, uint32_t pos, uint8_t playbit);
extern uint8_t cdrom_audio_play_pioneer(cdrom_t *dev, uint32_t pos);
extern uint8_t cdrom_audio_play_toshiba(cdrom_t *dev, uint32_t pos, int type);
extern void    cdrom_audio_pause_resume(cdrom_t *dev, uint8_t resume);

//Kevin
extern uint8_t  cdrom_read_toc(cdrom_t *dev, unsigned char *b, uint8_t track);
extern uint8_t  cdrom_disc_info(cdrom_t *dev,unsigned char *b);
extern uint8_t  cdrom_disc_capacity(cdrom_t *dev,unsigned char *b);
extern uint8_t cdrom_seek(cdrom_t *dev, int m, int s, int f);
extern uint8_t cdrom_audio_playmsf(cdrom_t *dev, int m,int s, int f, int M, int S, int F);
extern uint8_t cdrom_get_subq(cdrom_t *dev,uint8_t *b);

extern void cdrom_close_handler(uint8_t id);
extern void cdrom_insert(uint8_t id);
extern void cdrom_eject(uint8_t id);
extern void cdrom_reload(uint8_t id);

extern int  cdrom_image_open(cdrom_t *dev, const char *fn);
extern void cdrom_image_close(cdrom_t *dev);
extern void cdrom_image_reset(cdrom_t *dev);

extern void cdrom_ioctl_eject(void);
extern void cdrom_ioctl_load(void);
extern int  cdrom_ioctl_open(cdrom_t *dev, const char d);

extern void cdrom_update_cdb(uint8_t *cdb, int lba_pos,
                             int number_of_blocks);

extern int find_cdrom_for_scsi_id(uint8_t scsi_id);

extern void cdrom_close(void);
extern void cdrom_global_init(void);

extern int32_t cd_audio_volume; 
extern void cdrom_set_volume_scale(uint8_t percent);

#ifdef __cplusplus
}
#endif

#endif /*EMU_CDROM_H*/
