/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Generic CD-ROM drive core.
 *
 *
 *
 * Authors: Miran Grca, <mgrca8@gmail.com>,
 *          Kevin Moonlight, <me@yyzkevin.com>
 *          Ian Scott, <ian@polpo.org>
 *
 *          Copyright 2018-2021 Miran Grca.
 *          Copyright (C) 2024 Kevin Moonlight
 *          Copyright (C) 2025 Ian Scott
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#define HAVE_STDARG_H
#include "86box_compat.h"
#include "cdrom.h"
#include "cdrom_image_manager.h"
#include "cdrom_error_msg.h"
#include "pico/multicore.h"
#include "hardware/structs/timer.h"
#include "../volctrl.h"


/* The addresses sent from the guest are absolute, ie. a LBA of 0 corresponds to a MSF of 00:00:00. Otherwise, the counter displayed by the guest is wrong:
   there is a seeming 2 seconds in which audio plays but counter does not move, while a data track before audio jumps to 2 seconds before the actual start
   of the audio while audio still plays. With an absolute conversion, the counter is fine. */
#undef MSFtoLBA
#define MSFtoLBA(m, s, f)  ((((m * 60) + s) * 75) + f)


#pragma pack(push, 1)
typedef struct {
    uint8_t user_data[2048],
        ecc[288];
} m1_data_t;

typedef struct {
    uint8_t sub_header[8],
        user_data[2328];
} m2_data_t;

typedef union {
    m1_data_t m1_data;
    m2_data_t m2_data;
    uint8_t   raw_data[2336];
} sector_data_t;

typedef struct {
    uint8_t       sync[12];
    uint8_t       header[4];
    sector_data_t data;
} sector_raw_data_t;

typedef union {
    sector_raw_data_t sector_data;
    uint8_t           raw_data[2352];
} sector_t;

typedef struct {
    sector_t sector;
    uint8_t  c2[296];
    uint8_t  subchannel_raw[96];
    uint8_t  subchannel_q[16];
    uint8_t  subchannel_rw[96];
} cdrom_sector_t;

typedef union {
    cdrom_sector_t cdrom_sector;
    uint8_t        buffer[2856];
} sector_buffer_t;
#pragma pack(pop)

static int     cdrom_sector_size;
static uint8_t raw_buffer[2856]; /* Needs to be the same size as sector_buffer_t in the structs. */
static uint8_t extra_buffer[296];



/* int cdrom_interface_current; */

//#define ENABLE_CDROM_LOG 1

#ifdef ENABLE_CDROM_LOG
int cdrom_do_log = ENABLE_CDROM_LOG;

void
cdrom_log(const char *fmt, ...)
{
    va_list ap;

    if (cdrom_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define cdrom_log(fmt, ...)
#endif

/*
static __inline int
bin2bcd(int x)
{
    return (x % 10) | ((x / 10) << 4);
}
*/
/* unused
// Avoid div and modulo by using magic multiplication, this is about 10 cycles
static __inline uint8_t
bin2bcd(uint8_t x)
{
    // Calculate tens digit: value / 10 = (value * 205) >> 11
    uint8_t tens = (uint8_t)(((uint32_t)x * 205) >> 11);
    return (tens << 4) | (x - (tens * 10));
}
static __inline uint8_t
bcd2bin(uint8_t x)
{
    return (x >> 4) * 10 + (x & 0x0f);
}
*/

void cdrom_tasks(cdrom_t *dev) {
    // Will almost always be CD_COMMAND_NONE, so use __builtin_expect to tell the compiler
    switch (__builtin_expect(dev->image_command, CD_COMMAND_NONE)) {
    case CD_COMMAND_NONE:
        cdrom_read_data(dev);
        break;
    case CD_COMMAND_IMAGE_LIST:
        cdrom_errorstr_clear();
        dev->image_list = cdman_list_images(&dev->image_count);
        dev->image_command = CD_COMMAND_NONE;
        dev->image_status = dev->image_list ? CD_STATUS_READY : CD_STATUS_ERROR;
        break;
    case CD_COMMAND_IMAGE_LOAD_INDEX:
        cdman_load_image_index(dev, dev->image_data);
        break;
    case CD_COMMAND_IMAGE_LOAD:
        printf("loading");
        cdrom_errorstr_clear();
        if (dev->disk_loaded) {
            dev->disk_loaded = 0;
            dev->media_changed = 1;
            cdrom_image_close(dev);
            printf("Disk Removed.\n");     
            cdrom_error(dev, 0x11);   //media changed
        }
        if (dev->image_path[0]) {
            printf("Opening %s...",dev->image_path);
            if (cdrom_image_open(dev,dev->image_path)) {
                dev->image_command = CD_COMMAND_NONE;
                dev->image_status = CD_STATUS_ERROR;
                cdman_clear_image();
                break;
            } else {
                cdman_set_image_index(dev);
            }
            printf("Done.\n");
            dev->disk_loaded = 1;
            dev->media_changed = 1;
            cdrom_error(dev, 0x11);   //media changed
        }                    
        dev->image_command = CD_COMMAND_NONE;
        dev->image_status = CD_STATUS_IDLE;
        break;
    }
}


void cdrom_fifo_init(cdrom_fifo_t *fifo,uint16_t size) {
    fifo->data = calloc(1, size);    
    if(!fifo->data) fatal("Unable to allocate fifo");
    fifo->size = size;
}


void cdrom_fifo_write(cdrom_fifo_t *fifo,uint8_t val) {
    /* if(!fifo) return; */
    fifo->data[fifo->tail]=val;
    fifo->tail++;
    if(fifo->tail >= fifo->size) fifo->tail=0;        
}

void cdrom_fifo_write_multiple(cdrom_fifo_t *fifo, const uint8_t *data, size_t data_size) {
    // Calculate the number of bytes until the end of the buffer
    size_t bytes_until_end = fifo->size - fifo->tail;
    
    if (bytes_until_end >= data_size) {
        // Simple case - no wrap-around needed
        memcpy(fifo->data + fifo->tail, data, data_size);
        if (bytes_until_end == data_size) {
            fifo->tail = 0;
        } else {
            fifo->tail += data_size;
        }
    } else {
        // Handle wrap-around: first copy until the end of the buffer
        memcpy(fifo->data + fifo->tail, data, bytes_until_end);
        
        // Then copy the remaining data to the beginning of the buffer
        memcpy(fifo->data, data + bytes_until_end, data_size - bytes_until_end);
        fifo->tail = data_size - bytes_until_end;
    }
}

uint16_t __inline cdrom_fifo_level(cdrom_fifo_t *fifo) {
    /* if(!fifo) return 0; */
    if(fifo->head <= fifo->tail) return fifo->tail - fifo->head;
    return fifo->tail - fifo->head + fifo->size;    
}
uint8_t cdrom_fifo_read(cdrom_fifo_t *fifo) {    
    uint8_t x;        
    if(!cdrom_fifo_level(fifo)) {
        printf("FIFO UNDERRUN(%p)\n",fifo);
        return 0;
    }  

    x = fifo->data[fifo->head];
    fifo->head++;
    if(fifo->head >= fifo->size) fifo->head=0;
    return x;    
}
void cdrom_fifo_clear(cdrom_fifo_t *fifo) {
    if(cdrom_fifo_level(fifo)) {    
        printf("*** DISCARD %u *** (%p)\n",cdrom_fifo_level(fifo),fifo);        
        fifo->tail = fifo->head;        
    }
}


void
cdrom_stop(cdrom_t *dev)
{
    if (dev->cd_status > CD_STATUS_DATA_ONLY)
        dev->cd_status = CD_STATUS_STOPPED;
#if USE_CD_AUDIO_FIFO
    fifo_reset(&dev->audio_fifo);
#endif
}

uint8_t cdrom_seek(cdrom_t *dev, int m, int s, int f) {
    uint32_t pos;
    if (!dev) return 0;
        
    cdrom_log("CD-ROM %i: Seek to M:%x S:%x F:%x\n", dev->id, m,s,f);
    pos = MSFtoLBA(m, s, f) - 150;    
    //TODO: Should i check if this is a valid seek?
    dev->seek_pos = pos;
    cdrom_stop(dev);
    return 1;
}

#if USE_CD_AUDIO_FIFO
audio_fifo_t* cdrom_audio_fifo_peek(cdrom_t *dev) {
    return &dev->audio_fifo;
}
void cdrom_audio_fifo_init(cdrom_t *dev) {
    fifo_init(&dev->audio_fifo);
}

bool cdrom_audio_callback(cdrom_t *dev, uint32_t len) {
    bool ret = true;

    if (dev->cd_status != CD_STATUS_PLAYING) {
        return false;
    }

    // --- Fill audio_fifo using data from audio_sector_buffer, reading new sectors as needed ---
    // Loop until FIFO has 'len' samples, or is full, or an error/EoT occurs.
    while (fifo_level(&dev->audio_fifo) < len && ret) {
        uint32_t samples_remaining_in_sector = SAMPLES_PER_SECTOR - dev->audio_sector_consumed_samples;

        if (samples_remaining_in_sector == 0) { // Need to read a new sector
            if (dev->seek_pos < dev->cd_end) {
                bool read_successful;
                if (dev->audio_muted_soft) {
                    // Muted: "Fake" a read by filling the sector buffer with silence
                    cdrom_log("CD-ROM %i: Muted. Faking read of LBA %08X with silence.\n", dev->id, dev->seek_pos);
                    read_successful = true;
                    memset(dev->audio_sector_buffer, 0, RAW_SECTOR_SIZE);
                } else {
                    read_successful = dev->ops->read_sector(dev, CD_READ_AUDIO, dev->audio_sector_buffer, dev->seek_pos);
                }
                if (read_successful) {
                    /* putchar('r'); */
                    cdrom_log("CD-ROM %i: Read LBA %08X successful\n", dev->id, dev->seek_pos);
                    dev->seek_pos++;
                    /* dev->audio_sector_total_samples = SAMPLES_PER_SECTOR; */
                    dev->audio_sector_consumed_samples = 0;
                    samples_remaining_in_sector = SAMPLES_PER_SECTOR; // Update for current iteration
                } else {
                    cdrom_log("CD-ROM %i: Read LBA %08X failed\n", dev->id, dev->seek_pos);
                    dev->cd_status = CD_STATUS_STOPPED;
                    ret = false; // Mark error
                    break;   // Exit fill loop: read error
                }
            } else { // End of disc
                cdrom_log("CD-ROM %i: Playing completed (reached cd_end)\n", dev->id);
                dev->cd_status = CD_STATUS_PLAYING_COMPLETED;
                ret = false; // Mark end of track
                break;   // Exit fill loop: end of track
            }
        }

        // At this point, if ret is still true, samples_remaining_in_sector should be > 0.
        uint32_t space_in_fifo = fifo_free_space(&dev->audio_fifo);

        // Determine how many samples to transfer from staging to FIFO
        uint32_t samples_to_transfer = (samples_remaining_in_sector < space_in_fifo) ?
                                       samples_remaining_in_sector : space_in_fifo;

        /* printf("%u\n", samples_to_transfer); */
        // Add samples from sector buffer to FIFO
        bool added_ok = fifo_add_samples(&dev->audio_fifo,
                                         &dev->current_sector_samples[dev->audio_sector_consumed_samples],
                                         /* &dev->current_sector_samples[dev->audio_sector_consumed_samples], */
                                         samples_to_transfer);
        if (added_ok) {
            /* putchar('y'); */
            dev->audio_sector_consumed_samples += samples_to_transfer;
        } else {
            putchar('n');
            // This implies fifo_add_samples failed even though we thought there was space.
            // Could be a bug in fifo_free_space or fifo_add_samples, or a race condition in a threaded env (not here).
            cdrom_log("CD-ROM %i: fifo_add_samples failed unexpectedly! Space was %u, tried to add %u.\n",
                      dev->id, space_in_fifo, samples_to_transfer);
            ret = false; // Treat as an error
            break;
        }
    } // End while (fifo_level < len && ret)

    cdrom_log("CD-ROM %i: Audio cb. FIFO level: %u. Staging: %d/%d. Ret %d\n",
              dev->id, fifo_level(&dev->audio_fifo),
              dev->audio_sector_consumed_samples, dev->audio_sector_total_samples, ret);
    return ret;
}
#endif // USE_CD_AUDIO_FIFO

uint32_t cdrom_audio_callback_simple(cdrom_t *dev, int16_t *buffer, uint32_t len, bool pad) {
    if (dev->cd_status != CD_STATUS_PLAYING) {
        return 0;
    }

    uint32_t samples_produced = 0;
    // --- Fill buffer using data from audio_sector_buffer, reading new sectors as needed ---
    // Loop until FIFO has 'len' samples, or is full, or an error/EoT occurs.
    while (samples_produced < len) {
        uint32_t samples_remaining_in_sector = SAMPLES_PER_SECTOR - dev->audio_sector_consumed_samples;

        if (samples_remaining_in_sector == 0) { // Need to read a new sector
            if (dev->seek_pos < dev->cd_end) {
                bool read_successful;
                if (dev->audio_muted_soft) {
                    // Muted: "Fake" a read by filling the sector buffer with silence
                    cdrom_log("CD-ROM %i: Muted. Faking read of LBA %08X with silence.\n", dev->id, dev->seek_pos);
                    read_successful = true;
                    memset(dev->audio_sector_buffer, 0, RAW_SECTOR_SIZE);
                } else {
                    read_successful = dev->ops->read_sector(dev, CD_READ_AUDIO, dev->audio_sector_buffer, dev->seek_pos);
                }
                if (read_successful) {
                    /* putchar('r'); */
                    cdrom_log("CD-ROM %i: Read LBA %08X successful\n", dev->id, dev->seek_pos);
                    dev->seek_pos++;
                    /* dev->audio_sector_total_samples = SAMPLES_PER_SECTOR; */
                    dev->audio_sector_consumed_samples = 0;
                    samples_remaining_in_sector = SAMPLES_PER_SECTOR; // Update for current iteration
                } else {
                    cdrom_log("CD-ROM %i: Read LBA %08X failed\n", dev->id, dev->seek_pos);
                    dev->cd_status = CD_STATUS_STOPPED;
                    break;   // Exit fill loop: read error
                }
            } else { // End of disc
                cdrom_log("CD-ROM %i: Playing completed (reached cd_end)\n", dev->id);
                dev->cd_status = CD_STATUS_PLAYING_COMPLETED;
                break;   // Exit fill loop: end of track
            }
        }

        // At this point, if ret is still true, samples_remaining_in_sector should be > 0.
        uint32_t space_in_buffer = len - samples_produced;
        // Determine how many samples to transfer from staging to buffer
        uint32_t samples_to_transfer = (samples_remaining_in_sector < space_in_buffer) ?
                                       samples_remaining_in_sector : space_in_buffer;

        /* printf("%u\n", samples_to_transfer); */
        // Add samples from sector buffer to FIFO
        //memcpy(buffer + samples_produced, &dev->current_sector_samples[dev->audio_sector_consumed_samples], samples_to_transfer * sizeof(int16_t));

        for (uint32_t i = 0; i < samples_to_transfer; i++)
        {
            int32_t sample = dev->current_sector_samples[dev->audio_sector_consumed_samples + i];       
            buffer[samples_produced + i] = (int16_t)scale_sample(sample, cd_audio_volume, 0);
        }

        samples_produced += samples_to_transfer;
        dev->audio_sector_consumed_samples += samples_to_transfer;
    } // End while (fifo_level < len && ret)

    if (!pad) {
        return samples_produced;
    } else {
        // If we provided fewer samples than requested, fill the remainder of buffer with silence.
        if (samples_produced < len) {
            cdrom_log("CD-ROM %i: Outputting %u samples, padding %u with silence.\n",
                      dev->id, samples_produced, (len - samples_produced));
            memset(buffer + samples_produced, 0, (size_t)(len - samples_produced) * sizeof(int16_t));
        }
        return len;
    }
}


void __inline cdrom_read_data(cdrom_t *dev) {
    uint32_t pos;    
    /* uint16_t x; */
    /* uint8_t m,s,f;     */
    /*
    TODO, need to handle  when multiple blocks are requested.
    */   
    if(dev->req_total) {
    /* while(dev->req_total) { */
        if(cdrom_fifo_level(&dev->data_fifo) >= 2048) return;//need to be empty.        
        pos = MSFtoLBA(dev->req_m,dev->req_s,dev->req_f) - 150;    
        pos += dev->req_cur;
        // Read CD sector directly from fatfs into data fifo - note this assumes
        // 2048 byte sectors
        dev->ops->read_sector(dev, CD_READ_DATA, dev->data_fifo.data + dev->data_fifo.tail, pos);
        dev->data_fifo.tail = (dev->data_fifo.tail + 2048) & 4095;
        dev->req_cur++;
        if(dev->req_cur == dev->req_total) {
            cdrom_output_status(dev);
            dev->req_total=0;
        }
    }
}

uint8_t cdrom_status(cdrom_t *dev) {
    uint8_t status;    
    status |= 2;//this bit seems to always be set?
                //bit 4 never set?
    if(dev->cd_status == CD_STATUS_PLAYING)  status |= STAT_PLAY;
    if(dev->cd_status == CD_STATUS_PAUSED)  status |= STAT_PLAY;    
    if(cdrom_has_errors(dev)) status |= 0x10;    
    status |= 0x20;//always set?
    status |= STAT_TRAY;
    if(dev->disk_loaded) {
        status |= STAT_DISK;        
        status |= STAT_READY;
    }

    return status;
}
void cdrom_output_status(cdrom_t *dev) {
    cdrom_fifo_write(&dev->info_fifo,cdrom_status(dev));  
    dev->media_changed=0;  
}


uint8_t cdrom_audio_playmsf(cdrom_t *dev, int m,int s, int f, int M, int S, int F) {
    uint32_t pos;
    uint32_t pos2;
    

    //TODO
    /*
    This debug sequence is required to prevent crashing during quick seeking.
    Unable to find what is happening, but it is related to  core1 cdrom audio callback interacting with the 
    buffer,   while a command comes that also changes it,    goes nuts ends up corrupting memory.
    */
    /*
    printf("Buf1: %i\n",dev->cd_buflen);
    busy_wait_us(5);
    printf("Buf2: %i\n",dev->cd_buflen);
    */

    if (dev->cd_status == CD_STATUS_DATA_ONLY) {
        return 0;
    }
    
    dev->audio_muted_soft = 0;
    /* Do this at this point, since it's at this point that we know the
       actual LBA position to start playing from. */

    pos = MSFtoLBA(m,s,f) - 150;
    pos2 = MSFtoLBA(M,S,F) - 150;    

    if (!(dev->ops->track_type(dev, pos) & CD_TRACK_AUDIO)) {
        printf("CD-ROM %i: LBA %08X not on an audio track\n", dev->id, pos);
        cdrom_log("CD-ROM %i: LBA %08X not on an audio track\n", dev->id, pos);
        cdrom_error(dev,0x0E);
        cdrom_error(dev,0x10);

        cdrom_stop(dev);
        return 0;
    }    
    
    /*
    Changing these seek positions during audio playback  is a problemo.
    */

    dev->seek_pos  = pos;
    dev->cd_end    = pos2;
#if USE_CD_AUDIO_FIFO
    fifo_reset(&dev->audio_fifo);
#endif
    dev->cd_status = CD_STATUS_PLAYING;
    
    return 1;
}

void
cdrom_audio_pause_resume(cdrom_t *dev, uint8_t resume)
{
    if ((dev->cd_status == CD_STATUS_PLAYING) || (dev->cd_status == CD_STATUS_PAUSED))
        dev->cd_status = (dev->cd_status & 0xfe) | (resume & 0x01);
#if USE_CD_AUDIO_FIFO
    fifo_reset(&dev->audio_fifo);
#endif
}


uint8_t cdrom_get_subq(cdrom_t *dev,uint8_t *b) {    
    subchannel_t subc;    
    dev->ops->get_subchannel(dev, dev->seek_pos, &subc);
    b[0] = 0x80; //?
    b[1] = subc.attr;
    b[2] = subc.track;
    b[3] = subc.index;
    b[4] = subc.abs_m;
    b[5] = subc.abs_s;
    b[6] = subc.abs_f;
    b[7] = subc.rel_m;
    b[8] = subc.rel_s;
    b[9] = subc.rel_f;
    b[10]=0; //??    
}

uint8_t cdrom_read_toc(cdrom_t *dev, unsigned char *b, uint8_t track) {
    track_info_t ti;
    int first_track;
    int last_track;
    dev->ops->get_tracks(dev, &first_track, &last_track);    
    if(track > last_track)  return 0;    //should we allow +1 here?
    dev->ops->get_track_info(dev, track, 0, &ti);
    b[0]=0x0;
    b[1]=ti.attr;
    b[2]=ti.number;
    b[3]=0;
    b[4]=ti.m;
    b[5]=ti.s;
    b[6]=ti.f;
    b[7]=0;    
    return 1;    

}    
uint8_t cdrom_disc_info(cdrom_t *dev, unsigned char *b) {
    track_info_t ti;
    int first_track;
    int last_track;
    dev->ops->get_tracks(dev, &first_track, &last_track);        
    dev->ops->get_track_info(dev, last_track+1, 0, &ti);    
    b[0]=0x0;
    b[1]=first_track;
    b[2]=last_track;
    b[3]=ti.m;
    b[4]=ti.s;
    b[5]=ti.f;    
    return 1;    

}    

uint8_t cdrom_disc_capacity(cdrom_t *dev, unsigned char *b) {
    track_info_t ti;
    int first_track;
    int last_track;
    dev->ops->get_tracks(dev, &first_track, &last_track);        
    dev->ops->get_track_info(dev, last_track+1, 0, &ti);            
    b[0]=ti.m;
    b[1]=ti.s;
    b[2]=ti.f-1; //TODO THIS NEEDS TO HANDLE   FRAME 0,  JUST BEING LAZY 6AM
    b[3]=0x08;
    b[4]=0x00;    
    return 1;    

}    

/* Peform a master init on the entire module. */
void
cdrom_global_init(void)
{
    /* Clear the global data. */
    memset(&cdrom, 0x00, sizeof(cdrom));
    cdrom.error_str = cdrom_errorstr_get();
    cdrom.current_sector_samples = (int16_t*)cdrom.audio_sector_buffer;
}

static void
cdrom_drive_reset(cdrom_t *dev)
{
    dev->priv        = NULL;
    dev->insert      = NULL;
    dev->close       = NULL;
    dev->get_volume  = NULL;
    dev->get_channel = NULL;
}


void cdrom_error(cdrom_t *dev,uint8_t code) {
    uint8_t x;
    if(!code) return;
    if(!dev) return;
    for(x=0;x<8;x++) {
        if(!dev->errors[x]) dev->errors[x]=code;
    }
}
void cdrom_read_errors(cdrom_t *dev,uint8_t *b) {
    uint8_t x;
    if(!dev) return;
    if(!b) return;
    for(x=0;x<8;x++) {
        b[x]=dev->errors[x];
        dev->errors[x]=0;
    }
}
uint8_t cdrom_has_errors(cdrom_t *dev) {
    uint8_t x;
    if(!dev) return 0;
    for(x=0;x<8;x++) {
        if(dev->errors[x]) return 1;
    }
    return 0;
}    
