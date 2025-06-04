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
#include "cdrom_image.h"
#include "cdrom_image_manager.h"
#include "pico/multicore.h"
#include "hardware/structs/timer.h"


/* The addresses sent from the guest are absolute, ie. a LBA of 0 corresponds to a MSF of 00:00:00. Otherwise, the counter displayed by the guest is wrong:
   there is a seeming 2 seconds in which audio plays but counter does not move, while a data track before audio jumps to 2 seconds before the actual start
   of the audio while audio still plays. With an absolute conversion, the counter is fine. */
#undef MSFtoLBA
#define MSFtoLBA(m, s, f)  ((((m * 60) + s) * 75) + f)

/* #define RAW_SECTOR_SIZE    2352 */
/* #define COOKED_SECTOR_SIZE 2048 */

#define MIN_SEEK           2000
#define MAX_SEEK           333333

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
        dev->image_list = cdman_list_images(&dev->image_count);
        dev->image_command = CD_COMMAND_NONE;
        dev->image_status = dev->image_list ? CD_STATUS_READY : CD_STATUS_ERROR;
        break;
    case CD_COMMAND_IMAGE_LOAD_INDEX:
        cdman_load_image_index(dev, dev->image_data);
        break;
    case CD_COMMAND_IMAGE_LOAD:
        printf("loading");
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
                dev->image_status = CD_STATUS_ERROR;
                break;
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
    fifo->data = malloc(size);    
    if(!fifo->data) fatal("Unable to allocate fifo");
    memset(fifo->data,0,size);
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


#if 0
int
cdrom_lba_to_msf_accurate(int lba)
{
    int pos;
    int m;
    int s;
    int f;

    pos = lba + 150;
    f   = pos % 75;
    pos -= f;
    pos /= 75;
    s = pos % 60;
    pos -= s;
    pos /= 60;
    m = pos;

    return ((m << 16) | (s << 8) | f);
}
#endif

#if 0
static double
cdrom_get_short_seek(cdrom_t *dev)
{
    switch (dev->cur_speed) {
        case 0:
            fatal("CD-ROM %i: 0x speed\n", dev->id);
            return 0.0;
        case 1:
            return 240.0;
        case 2:
            return 160.0;
        case 3:
            return 150.0;
        case 4:
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
        case 10:
        case 11:
            return 112.0;
        case 12:
        case 13:
        case 14:
        case 15:
            return 75.0;
        case 16:
        case 17:
        case 18:
        case 19:
            return 58.0;
        case 20:
        case 21:
        case 22:
        case 23:
        case 40:
        case 41:
        case 42:
        case 43:
        case 44:
        case 45:
        case 46:
        case 47:
        case 48:
            return 50.0;
        default:
            /* 24-32, 52+ */
            return 45.0;
    }
}

static double
cdrom_get_long_seek(cdrom_t *dev)
{
    switch (dev->cur_speed) {
        case 0:
            fatal("CD-ROM %i: 0x speed\n", dev->id);
            return 0.0;
        case 1:
            return 1446.0;
        case 2:
            return 1000.0;
        case 3:
            return 900.0;
        case 4:
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
        case 10:
        case 11:
            return 675.0;
        case 12:
        case 13:
        case 14:
        case 15:
            return 400.0;
        case 16:
        case 17:
        case 18:
        case 19:
            return 350.0;
        case 20:
        case 21:
        case 22:
        case 23:
        case 40:
        case 41:
        case 42:
        case 43:
        case 44:
        case 45:
        case 46:
        case 47:
        case 48:
            return 300.0;
        default:
            /* 24-32, 52+ */
            return 270.0;
    }
}

double
cdrom_seek_time(cdrom_t *dev)
{
    uint32_t diff = dev->seek_diff;
    double   sd   = (double) (MAX_SEEK - MIN_SEEK);

    if (diff < MIN_SEEK)
        return 0.0;
    if (diff > MAX_SEEK)
        diff = MAX_SEEK;

    diff -= MIN_SEEK;

    return cdrom_get_short_seek(dev) + ((cdrom_get_long_seek(dev) * ((double) diff)) / sd);
}
#endif

void
cdrom_stop(cdrom_t *dev)
{
    if (dev->cd_status > CD_STATUS_DATA_ONLY)
        dev->cd_status = CD_STATUS_STOPPED;
    fifo_set_state(&dev->audio_fifo, FIFO_STATE_STOPPED);
    fifo_reset(&dev->audio_fifo);
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

int
cdrom_is_pre(cdrom_t *dev, uint32_t lba)
{
    if (dev->ops && dev->ops->is_track_pre)
        return dev->ops->is_track_pre(dev, lba);

    return 0;
}

audio_fifo_t* cdrom_audio_fifo_peek(cdrom_t *dev) {
    return &dev->audio_fifo;
}
void cdrom_audio_fifo_init(cdrom_t *dev) {
    dev->current_sector_samples = (int16_t*)dev->audio_sector_buffer;
    fifo_init(&dev->audio_fifo);
}

bool cdrom_audio_callback(cdrom_t *dev, uint32_t len) { // len is int
    bool ret = true;

    if (dev->cd_status != CD_STATUS_PLAYING) {
        return false;
    }

    /*
    if (!dev->sound_on || (dev->cd_status != CD_STATUS_PLAYING) || dev->audio_muted_soft) {
        if (dev->cd_status == CD_STATUS_PLAYING) {
            dev->seek_pos += (len >> 11); // Original logic for seek advancement
        }
        // memset(output, 0, (size_t)len * sizeof(audio_sample_t));
        return 0;
    }
    */

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
        /*
        if (space_in_fifo == 0) {
            // FIFO is full. If fifo_level < len, it means len > FIFO capacity.
            // We cannot put more into FIFO now. We'll output what we have.
            cdrom_log("CD-ROM %i: FIFO full (%u samples), cannot satisfy len %u yet.\n",
                      dev->id, fifo_level(&dev->audio_fifo), len);
            break; // Exit fill loop: FIFO is full, will provide what's available
        }
        */

        // Determine how many samples to transfer from staging to FIFO
        uint32_t samples_to_transfer = (samples_remaining_in_sector < space_in_fifo) ?
                                       samples_remaining_in_sector : space_in_fifo;
        /*
        if (samples_to_transfer == 0) {
            // This could happen if samples_remaining_in_sector > 0 but space_in_fifo is 0
            // (already handled by 'if (space_in_fifo == 0)' break), or if
            // samples_remaining_in_sector was miscalculated or became 0 unexpectedly.
            cdrom_log("CD-ROM %i: No samples to transfer (staging: %d, fifo_space: %u). Unexpected.\n",
                      dev->id, samples_remaining_in_sector, space_in_fifo);
            break; // Safety break
        }
        */

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

    /*
    // --- Output data to the user from audio_fifo ---
    uint32_t samples_provided = fifo_get_samples(&dev->audio_fifo, output, len);

    // If we provided fewer samples than requested, fill the remainder of 'output' with silence.
    if (samples_provided < len) {
        cdrom_log("CD-ROM %i: Outputting %u samples, padding %u with silence.\n",
                  dev->id, samples_provided, (len - samples_provided));
        memset(&output[samples_provided], 0, (size_t)(len - samples_provided) * sizeof(audio_sample_t));

        // If ret was still 1 (no read error/EoT), but we couldn't provide all 'len' samples,
        // it means the FIFO couldn't be filled enough (e.g., FIFO smaller than len and became full,
        // or a break from the fill loop due to 'No samples to transfer').
        // In such cases, the operation isn't fully successful in providing 'len' samples.
        if (ret == 1) {
            ret = 0;
        }
    }
    */
    if (!ret) {
        /* fifo_set_state(&dev->audio_fifo, FIFO_STATE_STOPPED); */
        /* fifo_reset(&dev->audio_fifo); */
    }

    cdrom_log("CD-ROM %i: Audio cb. FIFO level: %u. Staging: %d/%d. Ret %d\n",
              dev->id, fifo_level(&dev->audio_fifo),
              dev->audio_sector_consumed_samples, dev->audio_sector_total_samples, ret);
    return ret;
}

/*
int
cdrom_audio_callback_old(cdrom_t *dev, int16_t *output, int len)
{
    int ret = 1;    
    
    if (!dev->sound_on || (dev->cd_status != CD_STATUS_PLAYING) || dev->audio_muted_soft) {
        //cdrom_log("CD-ROM %i: Audio callback while not playing\n", dev->id);
        if (dev->cd_status == CD_STATUS_PLAYING) {                                    
            dev->seek_pos += (len >> 11);                        
        }
        memset(output, 0, len * 2);        
        return 0;
    }
        
    while (dev->cd_buflen < len) {
        if (dev->seek_pos < dev->cd_end) {
            // Puts RAW_SECTOR_SIZE bytes of data into cd_buffer
            if (dev->ops->read_sector(dev, CD_READ_AUDIO, (uint8_t *) &(dev->cd_buffer[dev->cd_buflen]),
                                      dev->seek_pos)) {
                cdrom_log("CD-ROM %i: Read LBA %08X successful\n", dev->id, dev->seek_pos);
                dev->seek_pos++;
                dev->cd_buflen += (RAW_SECTOR_SIZE / 2);
                ret = 1;
            } else {
                cdrom_log("CD-ROM %i: Read LBA %08X failed\n", dev->id, dev->seek_pos);
                memset(&(dev->cd_buffer[dev->cd_buflen]), 0x00, (BUF_SIZE - dev->cd_buflen) * 2);            
                dev->cd_status = CD_STATUS_STOPPED;
                dev->cd_buflen = len;
                ret            = 0;
            }
        } else {
            cdrom_log("CD-ROM %i: Playing completed\n", dev->id);            
            memset(&dev->cd_buffer[dev->cd_buflen], 0x00, (BUF_SIZE - dev->cd_buflen) * 2);            
            dev->cd_status = CD_STATUS_PLAYING_COMPLETED;
            dev->cd_buflen = len;
            ret            = 0;
        }
    }    
    
    
    memcpy(output, dev->cd_buffer, len * 2);            
    memmove(dev->cd_buffer, &dev->cd_buffer[len], (BUF_SIZE - len) * 2);
        
    dev->cd_buflen -= len;

    cdrom_log("CD-ROM %i: Audio callback returning %i\n", dev->id, ret);
    return ret;
}
*/

#if 0
int
cdrom_audio_callback_add(cdrom_t *dev, int16_t *output, int len)
{
    uint16_t x;
       
    int ret = 1;    
    
    if (!dev->sound_on || (dev->cd_status != CD_STATUS_PLAYING) || dev->audio_muted_soft) {
        //cdrom_log("CD-ROM %i: Audio callback while not playing\n", dev->id);
        if (dev->cd_status == CD_STATUS_PLAYING) {                                    
            dev->seek_pos += (len >> 11);                        
        }
        //memset(output, 0, len * 2);        
        return 0;
    }
        
    while (dev->cd_buflen < len) {
        if (dev->seek_pos < dev->cd_end) {
            if (dev->ops->read_sector(dev, CD_READ_AUDIO, (uint8_t *) &(dev->cd_buffer[dev->cd_buflen]),
                                      dev->seek_pos)) {
                cdrom_log("CD-ROM %i: Read LBA %08X successful\n", dev->id, dev->seek_pos);
                dev->seek_pos++;
                dev->cd_buflen += (RAW_SECTOR_SIZE / 2);
                ret = 1;
            } else {
                cdrom_log("CD-ROM %i: Read LBA %08X failed\n", dev->id, dev->seek_pos);
                memset(&(dev->cd_buffer[dev->cd_buflen]), 0x00, (BUF_SIZE - dev->cd_buflen) * 2);            
                dev->cd_status = CD_STATUS_STOPPED;
                dev->cd_buflen = len;
                ret            = 0;
            }
        } else {
            cdrom_log("CD-ROM %i: Playing completed\n", dev->id);            
            memset(&dev->cd_buffer[dev->cd_buflen], 0x00, (BUF_SIZE - dev->cd_buflen) * 2);            
            dev->cd_status = CD_STATUS_PLAYING_COMPLETED;
            dev->cd_buflen = len;
            ret            = 0;
        }
    }    
        
    for(x=0;x<len*2;x++) {
        output[x] += dev->cd_buffer[x];
    }
    //memcpy(output, dev->cd_buffer, len * 2);            
    memmove(dev->cd_buffer, &dev->cd_buffer[len], (BUF_SIZE - len) * 2);
        
    dev->cd_buflen -= len;

    cdrom_log("CD-ROM %i: Audio callback returning %i\n", dev->id, ret);
    return ret;
}
#endif

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
    fifo_reset(&dev->audio_fifo);
    fifo_set_state(&dev->audio_fifo, FIFO_STATE_RUNNING);
    dev->cd_status = CD_STATUS_PLAYING;
    
    return 1;
}

#if 0
uint8_t
cdrom_audio_track_search(cdrom_t *dev, uint32_t pos, int type, uint8_t playbit)
{
    uint8_t m = 0;
    uint8_t s = 0;
    uint8_t f = 0;
    uint32_t pos2 = 0;

    if (dev->cd_status == CD_STATUS_DATA_ONLY)
        return 0;

    cdrom_log("Audio Track Search: MSF = %06x, type = %02x, playbit = %02x\n", pos, type, playbit);
    switch (type) {
        case 0x00:
            if (pos == 0xffffffff) {
                cdrom_log("CD-ROM %i: (type 0) Search from current position\n", dev->id);
                pos = dev->seek_pos;
            }
            dev->seek_pos = pos;
            break;
        case 0x40:
            m   = bcd2bin((pos >> 24) & 0xff);
            s   = bcd2bin((pos >> 16) & 0xff);
            f   = bcd2bin((pos >> 8) & 0xff);
            if (pos == 0xffffffff) {
                cdrom_log("CD-ROM %i: (type 1) Search from current position\n", dev->id);
                pos = dev->seek_pos;
            } else
                pos = MSFtoLBA(m, s, f) - 150;

            dev->seek_pos = pos;
            break;
        case 0x80:
            if (pos == 0xffffffff) {
                cdrom_log("CD-ROM %i: (type 2) Search from current position\n", dev->id);
                pos = dev->seek_pos;
            }
            dev->seek_pos = (pos >> 24) & 0xff;
            break;
        default:
            break;
    }

    pos2 = pos - 1;
    if (pos2 == 0xffffffff)
        pos2 = pos + 1;

    /* Do this at this point, since it's at this point that we know the
       actual LBA position to start playing from. */
    if (!(dev->ops->track_type(dev, pos2) & CD_TRACK_AUDIO)) {
        cdrom_log("CD-ROM %i: Track Search: LBA %08X not on an audio track\n", dev->id, pos);
        dev->audio_muted_soft = 1;
        if (dev->ops->track_type(dev, pos) & CD_TRACK_AUDIO)
            dev->audio_muted_soft = 0;
    } else
        dev->audio_muted_soft = 0;

    cdrom_log("Track Search Toshiba: Muted?=%d, LBA=%08X.\n", dev->audio_muted_soft, pos);
    dev->cd_buflen = 0;
    dev->cd_status = playbit ? CD_STATUS_PLAYING : CD_STATUS_PAUSED;
    return 1;
}


uint8_t
cdrom_audio_play_pioneer(cdrom_t *dev, uint32_t pos)
{
    uint8_t m = 0;
    uint8_t s = 0;
    uint8_t f = 0;

    if (dev->cd_status == CD_STATUS_DATA_ONLY)
        return 0;

    f   = bcd2bin((pos >> 24) & 0xff);
    s   = bcd2bin((pos >> 16) & 0xff);
    m   = bcd2bin((pos >> 8) & 0xff);
    pos = MSFtoLBA(m, s, f) - 150;
    dev->cd_end = pos;

    dev->audio_muted_soft = 0;
    dev->cd_buflen = 0;
    dev->cd_status = CD_STATUS_PLAYING;
    return 1;
}


uint8_t
cdrom_audio_scan(cdrom_t *dev, uint32_t pos, int type)
{
    uint8_t m = 0;
    uint8_t s = 0;
    uint8_t f = 0;

    if (dev->cd_status == CD_STATUS_DATA_ONLY)
        return 0;

    cdrom_log("Audio Scan: MSF = %06x, type = %02x\n", pos, type);
    switch (type) {
        case 0x00:
            if (pos == 0xffffffff) {
                cdrom_log("CD-ROM %i: (type 0) Search from current position\n", dev->id);
                pos = dev->seek_pos;
            }
            dev->seek_pos = pos;
            break;
        case 0x40:
            m   = bcd2bin((pos >> 24) & 0xff);
            s   = bcd2bin((pos >> 16) & 0xff);
            f   = bcd2bin((pos >> 8) & 0xff);
            if (pos == 0xffffffff) {
                cdrom_log("CD-ROM %i: (type 1) Search from current position\n", dev->id);
                pos = dev->seek_pos;
            } else
                pos = MSFtoLBA(m, s, f) - 150;

            dev->seek_pos = pos;
            break;
        case 0x80:
            dev->seek_pos = (pos >> 24) & 0xff;
            break;
        default:
            break;
    }

    dev->audio_muted_soft = 0;
    /* Do this at this point, since it's at this point that we know the
       actual LBA position to start playing from. */
    if (!(dev->ops->track_type(dev, pos) & CD_TRACK_AUDIO)) {
        cdrom_log("CD-ROM %i: LBA %08X not on an audio track\n", dev->id, pos);
        cdrom_stop(dev);
        return 0;
    }

    dev->cd_buflen = 0;
    return 1;
}
#endif

void
cdrom_audio_pause_resume(cdrom_t *dev, uint8_t resume)
{
    if ((dev->cd_status == CD_STATUS_PLAYING) || (dev->cd_status == CD_STATUS_PAUSED))
        dev->cd_status = (dev->cd_status & 0xfe) | (resume & 0x01);
    fifo_set_state(&dev->audio_fifo, (dev->cd_status == CD_STATUS_PLAYING) ? FIFO_STATE_RUNNING : FIFO_STATE_PAUSED);
    fifo_reset(&dev->audio_fifo);
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

#if 0
uint8_t
cdrom_get_current_subchannel(cdrom_t *dev, uint8_t *b, int msf)
{
    uint8_t      ret;
    subchannel_t subc;
    int          pos = 1;
    int          m;
    int          s;
    int          f;
    uint32_t     dat;

    dev->ops->get_subchannel(dev, dev->seek_pos, &subc);

    if (dev->cd_status == CD_STATUS_DATA_ONLY)
        ret = 0x15;
    else {
        if (dev->cd_status == CD_STATUS_PLAYING)
            ret = 0x11;
        else if (dev->cd_status == CD_STATUS_PAUSED)
            ret = 0x12;
        else
            ret = 0x13;
    }

    cdrom_log("CD-ROM %i: Returned subchannel absolute at %02i:%02i.%02i, relative at %02i:%02i.%02i, ret = %02x, seek pos = %08x, cd_end = %08x.\n", dev->id, subc.abs_m, subc.abs_s, subc.abs_f, subc.rel_m, subc.rel_s, subc.rel_f, ret, dev->seek_pos, dev->cd_end);

    if (b[pos] > 1) {
        cdrom_log("B[%i] = %02x, ret = %02x.\n", pos, b[pos], ret);
        return ret;
    }

    b[pos++] = subc.attr;
    b[pos++] = subc.track;
    b[pos++] = subc.index;

    if (msf) {
        b[pos] = 0;

//        /* NEC CDR-260 speaks BCD. */
//        if ((dev->type == CDROM_TYPE_NEC_260_100) || (dev->type == CDROM_TYPE_NEC_260_101)) { /*NEC*/
//            m = subc.abs_m;
//            s = subc.abs_s;
//            f = subc.abs_f;
//            msf_to_bcd(&m, &s, &f);
//            b[pos + 1] = m;
//            b[pos + 2] = s;
//            b[pos + 3] = f;
//        } else {
            b[pos + 1] = subc.abs_m;
            b[pos + 2] = subc.abs_s;
            b[pos + 3] = subc.abs_f;
//        }

        pos += 4;

        b[pos] = 0;

//        /* NEC CDR-260 speaks BCD. */
//        if ((dev->type == CDROM_TYPE_NEC_260_100) || (dev->type == CDROM_TYPE_NEC_260_101)) { /*NEC*/
//            m = subc.rel_m;
//            s = subc.rel_s;
//            f = subc.rel_f;
//            msf_to_bcd(&m, &s, &f);
//            b[pos + 1] = m;
//            b[pos + 2] = s;
//            b[pos + 3] = f;
//        } else {
            b[pos + 1] = subc.rel_m;
            b[pos + 2] = subc.rel_s;
            b[pos + 3] = subc.rel_f;
//        }

        pos += 4;
    } else {
        dat      = MSFtoLBA(subc.abs_m, subc.abs_s, subc.abs_f) - 150;
        b[pos++] = (dat >> 24) & 0xff;
        b[pos++] = (dat >> 16) & 0xff;
        b[pos++] = (dat >> 8) & 0xff;
        b[pos++] = dat & 0xff;
        dat      = MSFtoLBA(subc.rel_m, subc.rel_s, subc.rel_f);
        b[pos++] = (dat >> 24) & 0xff;
        b[pos++] = (dat >> 16) & 0xff;
        b[pos++] = (dat >> 8) & 0xff;
        b[pos++] = dat & 0xff;
    }

    return ret;
}



void
cdrom_get_current_subcodeq(cdrom_t *dev, uint8_t *b)
{
    subchannel_t subc;

    dev->ops->get_subchannel(dev, dev->seek_pos, &subc);

    b[0] = subc.attr;
    b[1] = bin2bcd(subc.track);
    b[2] = bin2bcd(subc.index);
    b[3] = bin2bcd(subc.rel_m);
    b[4] = bin2bcd(subc.rel_s);
    b[5] = bin2bcd(subc.rel_f);
    b[6] = bin2bcd(subc.abs_m);
    b[7] = bin2bcd(subc.abs_s);
    b[8] = bin2bcd(subc.abs_f);
}

uint8_t
cdrom_get_current_subcodeq_playstatus(cdrom_t *dev, uint8_t *b)
{
    uint8_t ret;
    subchannel_t subc;

    dev->ops->get_subchannel(dev, dev->seek_pos, &subc);

    if ((dev->cd_status == CD_STATUS_DATA_ONLY) ||
        (dev->cd_status == CD_STATUS_PLAYING_COMPLETED) ||
        (dev->cd_status == CD_STATUS_STOPPED))
        ret = 0x03;
    else
        ret = (dev->cd_status == CD_STATUS_PLAYING) ? 0x00 : dev->audio_op;

    /*If a valid audio track is detected with audio on, unmute it.*/
    if (dev->ops->track_type(dev, dev->seek_pos) & CD_TRACK_AUDIO)
        dev->audio_muted_soft = 0;

    cdrom_log("SubCodeQ: Play Status: Seek LBA=%08x, CDEND=%08x, mute=%d.\n", dev->seek_pos, dev->cd_end, dev->audio_muted_soft);
    b[0] = subc.attr;
    b[1] = bin2bcd(subc.track);
    b[2] = bin2bcd(subc.index);
    b[3] = bin2bcd(subc.rel_m);
    b[4] = bin2bcd(subc.rel_s);
    b[5] = bin2bcd(subc.rel_f);
    b[6] = bin2bcd(subc.abs_m);
    b[7] = bin2bcd(subc.abs_s);
    b[8] = bin2bcd(subc.abs_f);
    return ret;
}

void cdrom_debug(cdrom_t *dev) {
    track_info_t ti;
    uint8_t x;
    uint32_t temp;
    int m = 0;
    int s = 0;
    int f = 0;

    int first_track;
    int last_track;
    dev->ops->get_tracks(dev, &first_track, &last_track);    

    printf("CDROM DEBUG\n-----\n");
    printf("First: %x Last: %x\n",first_track,last_track);
    for(x=first_track;x<=last_track+1;x++) {
        dev->ops->get_track_info(dev, x, 0, &ti);
        printf("T:%x M: %x S: %x F: %x A: %x\n",ti.number,ti.m,ti.s,ti.f,ti.attr);
    }
    
    printf("CD-ROM capacity: %i sectors (%" PRIi64 " bytes)\n", dev->cdrom_capacity, ((uint64_t) dev->cdrom_capacity) << 11ULL);
       
}
#endif

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


#if 0
/* New API calls for Mitsumi CD-ROM. */
void
cdrom_get_track_buffer(cdrom_t *dev, uint8_t *buf)
{
    track_info_t ti;
    int          first_track;
    int          last_track;

    if (dev != NULL) {
        dev->ops->get_tracks(dev, &first_track, &last_track);
        buf[0] = 1;
        buf[1] = last_track + 1;
        dev->ops->get_track_info(dev, 1, 0, &ti);
        buf[2] = ti.m;
        buf[3] = ti.s;
        buf[4] = ti.f;
        dev->ops->get_track_info(dev, last_track + 1, 0, &ti);
        buf[5] = ti.m;
        buf[6] = ti.s;
        buf[7] = ti.f;
        buf[8] = 0x00;
    } else
        memset(buf, 0x00, 9);
}



static int
track_type_is_valid(UNUSED(uint8_t id), int type, int flags, int audio, int mode2)
{
    if (!(flags & 0x70) && (flags & 0xf8)) { /* 0x08/0x80/0x88 are illegal modes */
        cdrom_log("CD-ROM %i: [Any Mode] 0x08/0x80/0x88 are illegal modes\n", id);
        return 0;
    }

    if ((type != 1) && !audio) {
        if ((flags & 0x06) == 0x06) {
            cdrom_log("CD-ROM %i: [Any Data Mode] Invalid error flags\n", id);
            return 0;
        }

        if (((flags & 0x700) == 0x300) || ((flags & 0x700) > 0x400)) {
            cdrom_log("CD-ROM %i: [Any Data Mode] Invalid subchannel data flags (%02X)\n", id, flags & 0x700);
            return 0;
        }

        if ((flags & 0x18) == 0x08) { /* EDC/ECC without user data is an illegal mode */
            cdrom_log("CD-ROM %i: [Any Data Mode] EDC/ECC without user data is an illegal mode\n", id);
            return 0;
        }

        if (((flags & 0xf0) == 0x90) || ((flags & 0xf0) == 0xc0)) { /* 0x90/0x98/0xC0/0xC8 are illegal modes */
            cdrom_log("CD-ROM %i: [Any Data Mode] 0x90/0x98/0xC0/0xC8 are illegal modes\n", id);
            return 0;
        }

        if (((type > 3) && (type != 8)) || (mode2 && (mode2 & 0x03))) {
            if ((flags & 0xf0) == 0x30) { /* 0x30/0x38 are illegal modes */
                cdrom_log("CD-ROM %i: [Any XA Mode 2] 0x30/0x38 are illegal modes\n", id);
                return 0;
            }
            if (((flags & 0xf0) == 0xb0) || ((flags & 0xf0) == 0xd0)) { /* 0xBx and 0xDx are illegal modes */
                cdrom_log("CD-ROM %i: [Any XA Mode 2] 0xBx and 0xDx are illegal modes\n", id);
                return 0;
            }
        }
    }

    return 1;
}
#endif

static void
read_sector_to_buffer(cdrom_t *dev, uint8_t *rbuf, uint32_t msf, uint32_t lba, int mode2, int len)
{
    uint8_t *bb = rbuf;

    dev->ops->read_sector(dev, CD_READ_DATA, rbuf + 16, lba);

    /* Sync bytes */
    bb[0] = 0;
    memset(bb + 1, 0xff, 10);
    bb[11] = 0;
    bb += 12;

    /* Sector header */
    bb[0] = (msf >> 16) & 0xff;
    bb[1] = (msf >> 8) & 0xff;
    bb[2] = msf & 0xff;

    bb[3] = 1; /* mode 1 data */
    bb += mode2 ? 12 : 4;
    bb += len;
    if (mode2 && ((mode2 & 0x03) == 1))
        memset(bb, 0, 280);
    else if (!mode2)
        memset(bb, 0, 288);
}

static void
read_audio(cdrom_t *dev, uint32_t lba, uint8_t *b)
{
    dev->ops->read_sector(dev, CD_READ_RAW, raw_buffer, lba);

    // TODO avoid memcpy by reading the sector directly?
    memcpy(b, raw_buffer, 2352);

    cdrom_sector_size = 2352;
}

static void
read_mode1(cdrom_t *dev, int cdrom_sector_flags, uint32_t lba, uint32_t msf, int mode2, uint8_t *b)
{
    if ((dev->cd_status == CD_STATUS_DATA_ONLY) || (dev->ops->sector_size(dev, lba) == 2048))
        read_sector_to_buffer(dev, raw_buffer, msf, lba, mode2, 2048);
    else
        dev->ops->read_sector(dev, CD_READ_RAW, raw_buffer, lba);

    cdrom_sector_size = 0;

    if (cdrom_sector_flags & 0x80) {
        /* Sync */
        cdrom_log("CD-ROM %i: [Mode 1] Sync\n", dev->id);
        memcpy(b, raw_buffer, 12);
        cdrom_sector_size += 12;
        b += 12;
    }

    if (cdrom_sector_flags & 0x20) {
        /* Header */
        cdrom_log("CD-ROM %i: [Mode 1] Header\n", dev->id);
        memcpy(b, raw_buffer + 12, 4);
        cdrom_sector_size += 4;
        b += 4;
    }

    if (cdrom_sector_flags & 0x40) {
        /* Sub-header */
        if (!(cdrom_sector_flags & 0x10)) {
            /* No user data */
            cdrom_log("CD-ROM %i: [Mode 1] Sub-header\n", dev->id);
            memcpy(b, raw_buffer + 16, 8);
            cdrom_sector_size += 8;
            b += 8;
        }
    }

    if (cdrom_sector_flags & 0x10) {
        /* User data */
        cdrom_log("CD-ROM %i: [Mode 1] User data\n", dev->id);
        memcpy(b, raw_buffer + 16, 2048);
        cdrom_sector_size += 2048;
        b += 2048;
    }

    if (cdrom_sector_flags & 0x08) {
        /* EDC/ECC */
        cdrom_log("CD-ROM %i: [Mode 1] EDC/ECC\n", dev->id);
        memcpy(b, raw_buffer + 2064, 288);
        cdrom_sector_size += 288;
        b += 288;
    }
}

static void
read_mode2_non_xa(cdrom_t *dev, int cdrom_sector_flags, uint32_t lba, uint32_t msf, int mode2, uint8_t *b)
{
    if ((dev->cd_status == CD_STATUS_DATA_ONLY) || (dev->ops->sector_size(dev, lba) == 2336))
        read_sector_to_buffer(dev, raw_buffer, msf, lba, mode2, 2336);
    else
        dev->ops->read_sector(dev, CD_READ_RAW, raw_buffer, lba);

    cdrom_sector_size = 0;

    if (cdrom_sector_flags & 0x80) {
        /* Sync */
        cdrom_log("CD-ROM %i: [Mode 2 Formless] Sync\n", dev->id);
        memcpy(b, raw_buffer, 12);
        cdrom_sector_size += 12;
        b += 12;
    }

    if (cdrom_sector_flags & 0x20) {
        /* Header */
        cdrom_log("CD-ROM %i: [Mode 2 Formless] Header\n", dev->id);
        memcpy(b, raw_buffer + 12, 4);
        cdrom_sector_size += 4;
        b += 4;
    }

    /* Mode 1 sector, expected type is 1 type. */
    if (cdrom_sector_flags & 0x40) {
        /* Sub-header */
        cdrom_log("CD-ROM %i: [Mode 2 Formless] Sub-header\n", dev->id);
        memcpy(b, raw_buffer + 16, 8);
        cdrom_sector_size += 8;
        b += 8;
    }

    if (cdrom_sector_flags & 0x10) {
        /* User data */
        cdrom_log("CD-ROM %i: [Mode 2 Formless] User data\n", dev->id);
        memcpy(b, raw_buffer + 24, 2336);
        cdrom_sector_size += 2336;
        b += 2336;
    }
}

static void
read_mode2_xa_form1(cdrom_t *dev, int cdrom_sector_flags, uint32_t lba, uint32_t msf, int mode2, uint8_t *b)
{
    if ((dev->cd_status == CD_STATUS_DATA_ONLY) || (dev->ops->sector_size(dev, lba) == 2048))
        read_sector_to_buffer(dev, raw_buffer, msf, lba, mode2, 2048);
    else
        dev->ops->read_sector(dev, CD_READ_RAW, raw_buffer, lba);

    cdrom_sector_size = 0;

    if (cdrom_sector_flags & 0x80) {
        /* Sync */
        cdrom_log("CD-ROM %i: [XA Mode 2 Form 1] Sync\n", dev->id);
        memcpy(b, raw_buffer, 12);
        cdrom_sector_size += 12;
        b += 12;
    }

    if (cdrom_sector_flags & 0x20) {
        /* Header */
        cdrom_log("CD-ROM %i: [XA Mode 2 Form 1] Header\n", dev->id);
        memcpy(b, raw_buffer + 12, 4);
        cdrom_sector_size += 4;
        b += 4;
    }

    if (cdrom_sector_flags & 0x40) {
        /* Sub-header */
        cdrom_log("CD-ROM %i: [XA Mode 2 Form 1] Sub-header\n", dev->id);
        memcpy(b, raw_buffer + 16, 8);
        cdrom_sector_size += 8;
        b += 8;
    }

    if (cdrom_sector_flags & 0x10) {
        /* User data */
        cdrom_log("CD-ROM %i: [XA Mode 2 Form 1] User data\n", dev->id);
        memcpy(b, raw_buffer + 24, 2048);
        cdrom_sector_size += 2048;
        b += 2048;
    }

    if (cdrom_sector_flags & 0x08) {
        /* EDC/ECC */
        cdrom_log("CD-ROM %i: [XA Mode 2 Form 1] EDC/ECC\n", dev->id);
        memcpy(b, raw_buffer + 2072, 280);
        cdrom_sector_size += 280;
        b += 280;
    }
}

static void
read_mode2_xa_form2(cdrom_t *dev, int cdrom_sector_flags, uint32_t lba, uint32_t msf, int mode2, uint8_t *b)
{
    if ((dev->cd_status == CD_STATUS_DATA_ONLY) || (dev->ops->sector_size(dev, lba) == 2324))
        read_sector_to_buffer(dev, raw_buffer, msf, lba, mode2, 2324);
    else
        dev->ops->read_sector(dev, CD_READ_RAW, raw_buffer, lba);

    cdrom_sector_size = 0;

    if (cdrom_sector_flags & 0x80) {
        /* Sync */
        cdrom_log("CD-ROM %i: [XA Mode 2 Form 2] Sync\n", dev->id);
        memcpy(b, raw_buffer, 12);
        cdrom_sector_size += 12;
        b += 12;
    }

    if (cdrom_sector_flags & 0x20) {
        /* Header */
        cdrom_log("CD-ROM %i: [XA Mode 2 Form 2] Header\n", dev->id);
        memcpy(b, raw_buffer + 12, 4);
        cdrom_sector_size += 4;
        b += 4;
    }

    if (cdrom_sector_flags & 0x40) {
        /* Sub-header */
        cdrom_log("CD-ROM %i: [XA Mode 2 Form 2] Sub-header\n", dev->id);
        memcpy(b, raw_buffer + 16, 8);
        cdrom_sector_size += 8;
        b += 8;
    }

    if (cdrom_sector_flags & 0x10) {
        /* User data */
        cdrom_log("CD-ROM %i: [XA Mode 2 Form 2] User data\n", dev->id);
        memcpy(b, raw_buffer + 24, 2328);
        cdrom_sector_size += 2328;
        b += 2328;
    }
}

#if 0
int
cdrom_readsector_raw(cdrom_t *dev, uint8_t *buffer, int sector, int ismsf, int cdrom_sector_type,
                     int cdrom_sector_flags, int *len, uint8_t vendor_type)
{
    uint8_t *b;
    uint8_t *temp_b;
    uint32_t msf;
    uint32_t lba;
    int      audio = 0;
    int      mode2 = 0;
    uint8_t  m;
    uint8_t  s;
    uint8_t  f;

    if (dev->cd_status == CD_STATUS_EMPTY)
        return 0;

    b = temp_b = buffer;

    *len = 0;

    if (ismsf) {
        m   = (sector >> 16) & 0xff;
        s   = (sector >> 8) & 0xff;
        f   = sector & 0xff;
        lba = MSFtoLBA(m, s, f) - 150;
        msf = sector;
    } else {
        switch (vendor_type) {
            case 0x00:
                lba = sector;
                msf = cdrom_lba_to_msf_accurate(sector);
                break;
            case 0x40:
                m = bcd2bin((sector >> 24) & 0xff);
                s = bcd2bin((sector >> 16) & 0xff);
                f = bcd2bin((sector >> 8) & 0xff);
                lba = MSFtoLBA(m, s, f) - 150;
                msf = sector;
                break;
            case 0x80:
                lba = bcd2bin((sector >> 24) & 0xff);
                msf = sector;
                break;
            /* Never used values but the compiler complains. */
            default:
                lba = msf = 0;
        }
    }

    if (dev->ops->track_type)
        audio = dev->ops->track_type(dev, lba);

    mode2 = audio & ~CD_TRACK_AUDIO;
    audio &= CD_TRACK_AUDIO;

    memset(raw_buffer, 0, 2448);
    memset(extra_buffer, 0, 296);

    if ((cdrom_sector_flags & 0xf8) == 0x08) {
        /* 0x08 is an illegal mode */
        cdrom_log("CD-ROM %i: [Mode 1] 0x08 is an illegal mode\n", dev->id);
        return 0;
    }

    if (!track_type_is_valid(dev->id, cdrom_sector_type, cdrom_sector_flags, audio, mode2))
        return 0;

    if ((cdrom_sector_type > 5) && (cdrom_sector_type != 8)) {
        cdrom_log("CD-ROM %i: Attempting to read an unrecognized sector type from an image\n", dev->id);
        return 0;
    } else if (cdrom_sector_type == 1) {
        if (!audio || (dev->cd_status == CD_STATUS_DATA_ONLY)) {
            cdrom_log("CD-ROM %i: [Audio] Attempting to read an audio sector from a data image\n", dev->id);
            return 0;
        }

        read_audio(dev, lba, temp_b);
    } else if (cdrom_sector_type == 2) {
        if (audio || mode2) {
            cdrom_log("CD-ROM %i: [Mode 1] Attempting to read a sector of another type\n", dev->id);
            return 0;
        }

        read_mode1(dev, cdrom_sector_flags, lba, msf, mode2, temp_b);
    } else if (cdrom_sector_type == 3) {
        if (audio || !mode2 || (mode2 & 0x03)) {
            cdrom_log("CD-ROM %i: [Mode 2 Formless] Attempting to read a sector of another type\n", dev->id);
            return 0;
        }

        read_mode2_non_xa(dev, cdrom_sector_flags, lba, msf, mode2, temp_b);
    } else if (cdrom_sector_type == 4) {
        if (audio || !mode2 || ((mode2 & 0x03) != 1)) {
            cdrom_log("CD-ROM %i: [XA Mode 2 Form 1] Attempting to read a sector of another type\n", dev->id);
            return 0;
        }

        read_mode2_xa_form1(dev, cdrom_sector_flags, lba, msf, mode2, temp_b);
    } else if (cdrom_sector_type == 5) {
        if (audio || !mode2 || ((mode2 & 0x03) != 2)) {
            cdrom_log("CD-ROM %i: [XA Mode 2 Form 2] Attempting to read a sector of another type\n", dev->id);
            return 0;
        }

        read_mode2_xa_form2(dev, cdrom_sector_flags, lba, msf, mode2, temp_b);
    } else if (cdrom_sector_type == 8) {
        if (audio) {
            cdrom_log("CD-ROM %i: [Any Data] Attempting to read a data sector from an audio track\n", dev->id);
            return 0;
        }

        if (mode2 && ((mode2 & 0x03) == 1))
            read_mode2_xa_form1(dev, cdrom_sector_flags, lba, msf, mode2, temp_b);
        else if (!mode2)
            read_mode1(dev, cdrom_sector_flags, lba, msf, mode2, temp_b);
        else {
            cdrom_log("CD-ROM %i: [Any Data] Attempting to read a data sector whose cooked size is not 2048 bytes\n", dev->id);
            return 0;
        }
    } else {
        if (mode2) {
            if ((mode2 & 0x03) == 0x01)
                read_mode2_xa_form1(dev, cdrom_sector_flags, lba, msf, mode2, temp_b);
            else if ((mode2 & 0x03) == 0x02)
                read_mode2_xa_form2(dev, cdrom_sector_flags, lba, msf, mode2, temp_b);
            else
                read_mode2_non_xa(dev, cdrom_sector_flags, lba, msf, mode2, temp_b);
        } else {
            if (audio)
                read_audio(dev, lba, temp_b);
            else
                read_mode1(dev, cdrom_sector_flags, lba, msf, mode2, temp_b);
        }
    }

    if ((cdrom_sector_flags & 0x06) == 0x02) {
        /* Add error flags. */
        cdrom_log("CD-ROM %i: Error flags\n", dev->id);
        memcpy(b + cdrom_sector_size, extra_buffer, 294);
        cdrom_sector_size += 294;
    } else if ((cdrom_sector_flags & 0x06) == 0x04) {
        /* Add error flags. */
        cdrom_log("CD-ROM %i: Full error flags\n", dev->id);
        memcpy(b + cdrom_sector_size, extra_buffer, 296);
        cdrom_sector_size += 296;
    }

    if ((cdrom_sector_flags & 0x700) == 0x100) {
        cdrom_log("CD-ROM %i: Raw subchannel data\n", dev->id);
        memcpy(b + cdrom_sector_size, raw_buffer + 2352, 96);
        cdrom_sector_size += 96;
    } else if ((cdrom_sector_flags & 0x700) == 0x200) {
        cdrom_log("CD-ROM %i: Q subchannel data\n", dev->id);
        memcpy(b + cdrom_sector_size, raw_buffer + 2352, 16);
        cdrom_sector_size += 16;
    } else if ((cdrom_sector_flags & 0x700) == 0x400) {
        cdrom_log("CD-ROM %i: R/W subchannel data\n", dev->id);
        memcpy(b + cdrom_sector_size, raw_buffer + 2352, 96);
        cdrom_sector_size += 96;
    }

    *len = cdrom_sector_size;

    return 1;
}
#endif

/* Peform a master init on the entire module. */
void
cdrom_global_init(void)
{
    /* Clear the global data. */
    memset(cdrom, 0x00, sizeof(cdrom));
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





void
cdrom_hard_reset(void)
{
    cdrom_t *dev;

    for (uint8_t i = 0; i < CDROM_NUM; i++) {
        dev = &cdrom[i];
        /*
        if (dev->bus_type) {
            cdrom_log("CD-ROM %i: Hard reset\n", i);

            dev->id = i;

            cdrom_drive_reset(dev);

            dev->cd_status = CD_STATUS_EMPTY;

            if (dev->host_drive == 200) {
                if ((strlen(dev->image_path) >= 1) &&
                    (dev->image_path[strlen(dev->image_path) - 1] == '\\'))
                    dev->image_path[strlen(dev->image_path) - 1] = '/';
                cdrom_image_open(dev, dev->image_path);
            }
        }
        */
    }

//KM TODO 
//    sound_cd_thread_reset();
}

