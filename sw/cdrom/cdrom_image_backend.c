/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          CD-ROM image file handling module, translated to C from
 *          cdrom_dosbox.cpp.
 *
 *
 *
 * Authors: Miran Grca, <mgrca8@gmail.com>
 *          Fred N. van Kempen, <decwiz@yahoo.com>
 *          The DOSBox Team, <unknown>
 *
 *          Copyright 2016-2020 Miran Grca.
 *          Copyright 2017-2020 Fred N. van Kempen.
 *          Copyright 2002-2020 The DOSBox Team.
 */
#define __STDC_FORMAT_MACROS
#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#    include <string.h>
#    include <sys/types.h>
#else
#    include <libgen.h>
#endif
#define HAVE_STDARG_H
#include "cdrom_image_backend.h"
#include "86box_compat.h"


/* #define CDROM_BCD(x)        (((x) % 10) | (((x) / 10) << 4)) */
// Avoid div and modulo by using magic multiplication, this is about 10 cycles
static inline uint8_t cdrom_bcd(uint8_t value) {
    // Calculate tens digit: value / 10 = (value * 205) >> 11
    uint8_t tens = (uint8_t)(((uint32_t)value * 205) >> 11);
    return (tens << 4) | (value - (tens * 10));
}

// Avoid multiple 64-bit divs (!!) by doing simple subtraction loops
static inline void frames_to_msf(uint32_t total_frames, uint8_t *pM, uint8_t *pS, uint8_t *pF) {
    // subtract "minute-chunks" of frames
    uint8_t calculated_minutes = 0; // Minutes won't exceed 80-ish on a CD
    while (total_frames >= CD_FPM)
    {
        total_frames -= CD_FPM;
        calculated_minutes++;
    }
    *pM = calculated_minutes;

    // subtracts "second-chunks" of frames
    uint8_t calculated_seconds = 0; // Seconds will not exceed 59
    while (total_frames >= CD_FPS)
    {
        total_frames -= CD_FPS;
        calculated_seconds++;
    }
    *pS = calculated_seconds;

    // total_frames is now < CD_FPS
    *pF = (uint8_t)total_frames;
}

#define MAX_LINE_LENGTH     512
#define MAX_FILENAME_LENGTH 256
#define CROSS_LEN           512

static char temp_keyword[64];

#ifdef ENABLE_CDROM_IMAGE_BACKEND_LOG
int cdrom_image_backend_do_log = ENABLE_CDROM_IMAGE_BACKEND_LOG;

void cdrom_image_backend_log(const char *fmt, ...) {
    char    temp[1024];
    va_list ap;
    char   *sp;
    va_start(ap, fmt);
    vsprintf(temp, fmt, ap);
    printf("%s\n\r",temp);    
    va_end(ap);
}
/*
cdrom_image_backend_log(const char *fmt, ...)
{
    va_list ap;

    if (cdrom_image_backend_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
*/
#else
#    define cdrom_image_backend_log(fmt, ...)
#endif

/* #include "pico/stdlib.h" */
/* extern uint LED_PIN; */
/* Binary file functions. */
static int
/* bin_read(void *priv, uint8_t *buffer, uint64_t seek, size_t count) */
bin_read(void *priv, uint8_t *buffer, uint32_t seek, size_t count)
{
    unsigned int bytes_read;
    track_file_t *tf = (track_file_t *) priv;

    /* cdrom_image_backend_log("CDROM: binary_read(%08lx, pos=%" PRIu64 " count=%lu\n", */
    cdrom_image_backend_log("CDROM: binary_read(%08lx, pos=%" PRIu32 " count=%lu\n",
                            tf->fp, seek, count);

    if (tf->fp == NULL)
        return 0;    
    //if (fseeko64(tf->fp, seek, SEEK_SET) == -1) {    
    if (f_lseek(tf->fp, seek) != FR_OK) {
#ifdef ENABLE_CDROM_IMAGE_BACKEND_LOG
        cdrom_image_backend_log("CDROM: binary_read failed during seek!\n");
#endif
        return 0;
    }

    //if (fread(buffer, count, 1, tf->fp) != 1) {
    if (f_read(tf->fp,buffer, count,&bytes_read) != FR_OK) {
#ifdef ENABLE_CDROM_IMAGE_BACKEND_LOG
        cdrom_image_backend_log("CDROM: binary_read failed during read!\n");
#endif
        return 0;
    }
    /* gpio_xor_mask(LED_PIN); */

    return 1;
}

/* static uint64_t */
static uint32_t
bin_get_length(void *priv)
{
    /* off64_t       len; */
    /* uint64_t flen; */
    off_t       len;
    uint32_t flen;
    track_file_t *tf = (track_file_t *) priv;

    cdrom_image_backend_log("CDROM: binary_length(%08lx)\n", tf->fp);

    if (tf->fp == NULL)
        return 0;

    //fseeko64(tf->fp, 0, SEEK_END);
    //len = ftello64(tf->fp);
    len = f_size(tf->fp);
    /* cdrom_image_backend_log("CDROM: binary_length(%08lx) = %" PRIu64 "\n", tf->fp, len); */
    cdrom_image_backend_log("CDROM: binary_length(%08lx) = %" PRIu32 "\n", tf->fp, len);

    return len;
}

static void
bin_close(void *priv)
{
    track_file_t *tf = (track_file_t *) priv;

    if (tf == NULL)
        return;

    if (tf->fp != NULL) {
        //fclose(tf->fp);
        f_close(tf->fp);
        tf->fp = NULL;
    }

    memset(tf->fn, 0x00, sizeof(tf->fn));

    free(priv);
}

static track_file_t *
bin_init(const char *filename, int *error)
{
    track_file_t *tf = (track_file_t *) malloc(sizeof(track_file_t));
    struct stat   stats;
    
    if (tf == NULL) {
        *error = 1;
        cdrom_image_backend_log("can't malloc\n");
        return NULL;
    }

    memset(tf->fn, 0x00, sizeof(tf->fn));
    strncpy(tf->fn, filename, sizeof(tf->fn) - 1);
    //tf->fp = plat_fopen64(tf->fn, "rb");    
    
    tf->fp = (FIL *) malloc(sizeof(FIL));
    FRESULT result = f_open(tf->fp, tf->fn, FA_READ);
    cdrom_image_backend_log("CDROM: binary_open(%s) = %08lx, result %d\n", tf->fn, tf->fp, result);
    FSIZE_t len = f_size(tf->fp);
    cdrom_image_backend_log("file size: %u", len);

    /* if (f_stat(tf->fn, &stats) != 0) { */
    /*     #<{(| Use a blank structure if stat failed. |)}># */
    /*     memset(&stats, 0, sizeof(struct stat)); */
    /* } */
    /* *error = ((tf->fp == NULL) || ((stats.st_mode & S_IFMT) == S_IFDIR)); */
    /*  */
    /* #<{(| Set the function pointers. |)}># */
    /* if (!*error) { */
    if (result == FR_OK) {
        cdrom_image_backend_log("all good\n");
        tf->read       = bin_read;
        tf->get_length = bin_get_length;
        tf->close      = bin_close;
    } else {
    /*     #<{(| From the check above, error may still be non-zero if opening a directory. */
    /*      * The error is set for viso to try and open the directory following this function. */
    /*      * However, we need to make sure the descriptor is closed. |)}># */
    /*     if ((tf->fp != NULL) && ((stats.st_mode & S_IFMT) == S_IFDIR)) { */
    /*         #<{(| tf is freed by bin_close |)}># */
    /*         bin_close(tf); */
    /*     } else { */
            free(tf);
    /*     } */
        tf = NULL;
    }

    return tf;
}

static track_file_t *
track_file_init(const char *filename, int *error)
{
    /* Current we only support .BIN files, either combined or one per
       track. In the future, more is planned. */
    return bin_init(filename, error);
}

static void
track_file_close(track_t *trk)
{
    if (trk == NULL)
        return;

    if (trk->file == NULL)
        return;

    if (trk->file->close == NULL)
        return;

    trk->file->close(trk->file);
    trk->file = NULL;
}

/* Root functions. */
static void
cdi_clear_tracks(cd_img_t *cdi)
{
    const track_file_t *last = NULL;
    track_t            *cur  = NULL;

    if ((cdi->tracks == NULL) || (cdi->tracks_num == 0))
        return;

    for (int i = 0; i < cdi->tracks_num; i++) {
        cur = &cdi->tracks[i];

        /* Make sure we do not attempt to close a NULL file. */
        if (cur->file != last) {
            last = cur->file;
            track_file_close(cur);
        } else
            cur->file = NULL;
    }

    /* Now free the array. */
    free(cdi->tracks);
    cdi->tracks = NULL;

    /* Mark that there's no tracks. */
    cdi->tracks_num = 0;
}

void
cdi_close(cd_img_t *cdi)
{
    cdi_clear_tracks(cdi);
    free(cdi);
}

int
cdi_set_device(cd_img_t *cdi, const char *path)
{
    int ret;

    if ((ret = cdi_load_cue(cdi, path)))
        return ret;

    if ((ret = cdi_load_iso(cdi, path)))
        return ret;

    return 0;
}

void
cdi_get_audio_tracks(cd_img_t *cdi, int *st_track, int *end, TMSF *lead_out)
{
    *st_track = 1;
    *end      = cdi->tracks_num - 1;
    frames_to_msf(cdi->tracks[*end].start + 150, &lead_out->min, &lead_out->sec, &lead_out->fr);
}

void
cdi_get_audio_tracks_lba(cd_img_t *cdi, int *st_track, int *end, uint32_t *lead_out)
{
    *st_track = 1;
    *end      = cdi->tracks_num - 1;
    *lead_out = cdi->tracks[*end].start;
}

int
cdi_get_audio_track_pre(cd_img_t *cdi, int track)
{
    const track_t *trk = &cdi->tracks[track - 1];

    if ((track < 1) || (track > cdi->tracks_num))
        return 0;

    return trk->pre;
}

/* This replaces both Info and EndInfo, they are specified by a variable. */
int
cdi_get_audio_track_info(cd_img_t *cdi, UNUSED(int end), int track, int *track_num, TMSF *start, uint8_t *attr)
{
    const track_t *trk = &cdi->tracks[track - 1];
    int      pos = trk->start + 150;

    if ((track < 1) || (track > cdi->tracks_num))
        return 0;

    pos = trk->start + 150;

    frames_to_msf(pos, &start->min, &start->sec, &start->fr);

    *track_num = trk->track_number;
    *attr      = trk->attr;

    return 1;
}

int
cdi_get_audio_track_info_lba(cd_img_t *cdi, UNUSED(int end), int track, int *track_num, uint32_t *start, uint8_t *attr)
{
    const track_t *trk = &cdi->tracks[track - 1];

    if ((track < 1) || (track > cdi->tracks_num))
        return 0;

    *start = (uint32_t) trk->start;

    *track_num = trk->track_number;
    *attr      = trk->attr;

    return 1;
}

int
cdi_get_track(cd_img_t *cdi, uint32_t sector)
{
    const track_t *cur;
    const track_t *next;

    /* There must be at least two tracks - data and lead out. */
    if (cdi->tracks_num < 2)
        return -1;

    /* This has a problem - the code skips the last track, which is
       lead out - is that correct? */
    for (int i = 0; i < (cdi->tracks_num - 1); i++) {
        cur  = &cdi->tracks[i];
        next = &cdi->tracks[i + 1];

        /* Take into account cue sheets that do not start on sector 0. */
        if ((i == 0) && (sector < cur->start))
            return cur->number;

        if ((cur->start <= sector) && (sector < next->start))
            return cur->number;
    }

    return -1;
}

/* TODO: See if track start is adjusted by 150 or not. */
int
cdi_get_audio_sub(cd_img_t *cdi, uint32_t sector, uint8_t *attr, uint8_t *track, uint8_t *index, TMSF *rel_pos, TMSF *abs_pos)
{
    int            cur_track = cdi_get_track(cdi, sector);
    const track_t *trk;

    if (cur_track < 1)
        return 0;

    *track = (uint8_t) cur_track;
    trk    = &cdi->tracks[*track - 1];
    *attr  = trk->attr;
    *index = 1;

    frames_to_msf(sector + 150, &abs_pos->min, &abs_pos->sec, &abs_pos->fr);

    /* Absolute position should be adjusted by 150, not the relative ones. */
    frames_to_msf(sector - trk->start, &rel_pos->min, &rel_pos->sec, &rel_pos->fr);

    return 1;
}

int
cdi_read_sector(cd_img_t *cdi, uint8_t *buffer, int raw, uint32_t sector)
{
    size_t   length;
    int      track = cdi_get_track(cdi, sector) - 1;
    /* uint64_t sect  = (uint64_t) sector; */
    /* uint64_t seek; */
    /* uint32_t sect  = (uint32_t) sector; */
    uint32_t seek;
    track_t *trk;
    int      track_is_raw;
    int      ret;
    int      raw_size;
    int      cooked_size;
    /* uint64_t offset = 0ULL; */
    uint32_t offset = 0;
    uint8_t      m = 0;
    uint8_t      s = 0;
    uint8_t      f = 0;

    if (track < 0)
        return 0;

    trk          = &cdi->tracks[track];
    track_is_raw = ((trk->sector_size == RAW_SECTOR_SIZE) || (trk->sector_size == 2448));

    /* seek = trk->skip + ((sect - trk->start) * trk->sector_size); */
    seek = trk->skip + ((sector - trk->start) * trk->sector_size);

    if (track_is_raw)
        raw_size = trk->sector_size;
    else
        raw_size = 2448;

    if (trk->mode2 && (trk->form != 1)) {
        if (trk->form == 2)
            cooked_size = (track_is_raw ? 2328 : trk->sector_size); /* Both 2324 + ECC and 2328 variants are valid. */
        else
            cooked_size = 2336;
    } else
        cooked_size = COOKED_SECTOR_SIZE;

    length = (raw ? raw_size : cooked_size);

    if (trk->mode2 && (trk->form >= 1))
        offset = 24;
        /* offset = 24ULL; */
    else
        offset = 16;
        /* offset = 16ULL; */

    if (raw && !track_is_raw) {
        memset(buffer, 0x00, 2448);
        ret = trk->file->read(trk->file, buffer + offset, seek, length);
        if (!ret)
            return 0;
        /* Construct the rest of the raw sector. */
        memset(buffer + 1, 0xff, 10);
        buffer += 12;
        frames_to_msf(sector + 150, &m, &s, &f);
        /* These have to be BCD. */
        buffer[0] = cdrom_bcd(m);
        buffer[1] = cdrom_bcd(s);
        buffer[2] = cdrom_bcd(f);
        /* Data, should reflect the actual sector type. */
        buffer[3] = trk->mode2 ? 2 : 1;
        return 1;
    } else if (!raw && track_is_raw)
        return trk->file->read(trk->file, buffer, seek + offset, length);
    else {
        return trk->file->read(trk->file, buffer, seek, length);
    }
}

int
cdi_read_sectors(cd_img_t *cdi, uint8_t *buffer, int raw, uint32_t sector, uint32_t num)
{
    int      sector_size;
    int      success = 1;
    uint8_t *buf;
    uint32_t buf_len;

    /* TODO: This fails to account for Mode 2. Shouldn't we have a function
             to get sector size? */
    sector_size = raw ? RAW_SECTOR_SIZE : COOKED_SECTOR_SIZE;
    buf_len     = num * sector_size;
    buf         = (uint8_t *) malloc(buf_len * sizeof(uint8_t));

    for (uint32_t i = 0; i < num; i++) {
        success = cdi_read_sector(cdi, &buf[i * sector_size], raw, sector + i);
        if (!success)
            break;
        /* Based on the DOSBox patch, but check all 8 bytes and makes sure it's not an
           audio track. */
        if (raw && sector < cdi->tracks[0].length && !cdi->tracks[0].mode2 && (cdi->tracks[0].attr != AUDIO_TRACK) && *(uint64_t *) &(buf[i * sector_size + 2068])) {
            free(buf);
            return 0;
        }
    }

    memcpy((void *) buffer, buf, buf_len);
    free(buf);
    buf = NULL;

    return success;
}

/* TODO: Do CUE+BIN images with a sector size of 2448 even exist? */
int
cdi_read_sector_sub(cd_img_t *cdi, uint8_t *buffer, uint32_t sector)
{
    int      track = cdi_get_track(cdi, sector) - 1;
    track_t *trk;
    /* uint64_t s = (uint64_t) sector; */
    /* uint64_t seek; */
    uint32_t seek;

    if (track < 0)
        return 0;

    trk  = &cdi->tracks[track];
    seek = trk->skip + ((sector - trk->start) * trk->sector_size);
    if (trk->sector_size != 2448)
        return 0;

    return trk->file->read(trk->file, buffer, seek, 2448);
}

int
cdi_get_sector_size(cd_img_t *cdi, uint32_t sector)
{
    int            track = cdi_get_track(cdi, sector) - 1;
    const track_t *trk;

    if (track < 0)
        return 0;

    trk = &cdi->tracks[track];
    return trk->sector_size;
}

int
cdi_is_mode2(cd_img_t *cdi, uint32_t sector)
{
    int            track = cdi_get_track(cdi, sector) - 1;
    const track_t *trk;

    if (track < 0)
        return 0;

    trk = &cdi->tracks[track];

    return !!(trk->mode2);
}

int
cdi_get_mode2_form(cd_img_t *cdi, uint32_t sector)
{
    int            track = cdi_get_track(cdi, sector) - 1;
    const track_t *trk;

    if (track < 0)
        return 0;

    trk = &cdi->tracks[track];

    return trk->form;
}

static int
/* cdi_can_read_pvd(track_file_t *file, uint64_t sector_size, int mode2, int form) */
cdi_can_read_pvd(track_file_t *file, uint32_t sector_size, int mode2, int form)
{
    uint8_t  pvd[COOKED_SECTOR_SIZE];
    /* uint64_t seek = 16ULL * sector_size; #<{(| First VD is located at sector 16. |)}># */
    uint64_t seek = 16 * sector_size; /* First VD is located at sector 16. */

    if ((!mode2 || (form == 0)) && (sector_size == RAW_SECTOR_SIZE))
        seek += 16;
    if (mode2 && (form >= 1))
        seek += 24;

    file->read(file, pvd, seek, COOKED_SECTOR_SIZE);

    return ((pvd[0] == 1 && !strncmp((char *) (&pvd[1]), "CD001", 5) && pvd[6] == 1) || (pvd[8] == 1 && !strncmp((char *) (&pvd[9]), "CDROM", 5) && pvd[14] == 1));
}

/* This reallocates the array and returns the pointer to the last track. */
static void
cdi_track_push_back(cd_img_t *cdi, track_t *trk)
{
    /* This has to be done so situations in which realloc would misbehave
       can be detected and reported to the user. */
    if ((cdi->tracks != NULL) && (cdi->tracks_num == 0))
        fatal("CD-ROM Image: Non-null tracks array at 0 loaded tracks\n");
    if ((cdi->tracks == NULL) && (cdi->tracks_num != 0))
        fatal("CD-ROM Image: Null tracks array at non-zero loaded tracks\n");

    cdi->tracks = realloc(cdi->tracks, (cdi->tracks_num + 1) * sizeof(track_t));
    memcpy(&(cdi->tracks[cdi->tracks_num]), trk, sizeof(track_t));
    cdi->tracks_num++;
}

int
cdi_load_iso(cd_img_t *cdi, const char *filename)
{
    int     error;
    int     ret = 2;
    track_t trk;

    cdi->tracks     = NULL;
    cdi->tracks_num = 0;

    memset(&trk, 0, sizeof(track_t));

    /* Data track (shouldn't there be a lead in track?). */
    trk.file = bin_init(filename, &error);
    if (error) {
        if ((trk.file != NULL) && (trk.file->close != NULL))
            trk.file->close(trk.file);
        ret      = 3;
//KM        trk.file = viso_init(filename, &error);
        if (error) {
            if ((trk.file != NULL) && (trk.file->close != NULL))
                trk.file->close(trk.file);
            return 0;
        }
    }
    trk.number       = 1;
    trk.track_number = 1;
    trk.attr         = DATA_TRACK;

    /* Try to detect ISO type. */
    trk.form  = 0;
    trk.mode2 = 0;
    /* TODO: Merge the first and last cases since they result in the same thing. */
    if (cdi_can_read_pvd(trk.file, RAW_SECTOR_SIZE, 0, 0))
        trk.sector_size = RAW_SECTOR_SIZE;
    else if (cdi_can_read_pvd(trk.file, 2336, 1, 0)) {
        trk.sector_size = 2336;
        trk.mode2       = 1;
    } else if (cdi_can_read_pvd(trk.file, 2324, 1, 2)) {
        trk.sector_size = 2324;
        trk.mode2       = 1;
        trk.form        = 2;
    } else if (cdi_can_read_pvd(trk.file, RAW_SECTOR_SIZE, 1, 0)) {
        trk.sector_size = RAW_SECTOR_SIZE;
        trk.mode2       = 1;
    } else {
        /* We use 2048 mode 1 as the default. */
        trk.sector_size = COOKED_SECTOR_SIZE;
    }

    trk.length = trk.file->get_length(trk.file) / trk.sector_size;
    /* cdrom_image_backend_log("ISO: Data track: length = %" PRIu64 ", sector_size = %i\n", trk.length, trk.sector_size); */
    cdrom_image_backend_log("ISO: Data track: length = %" PRIu32 ", sector_size = %i\n", trk.length, trk.sector_size);
    cdi_track_push_back(cdi, &trk);

    /* Lead out track. */
    trk.number       = 2;
    trk.track_number = 0xAA;
    trk.attr         = 0x16; /* Was originally 0x00, but I believe 0x16 is appropriate. */
    trk.start        = trk.length;
    trk.length       = 0;
    trk.file         = NULL;
    cdi_track_push_back(cdi, &trk);

    return ret;
}

static int
cdi_cue_get_buffer(char *str, char **line, int up)
{
    char *s     = *line;
    char *p     = str;
    int   quote = 0;
    int   done  = 0;
    int   space = 1;

    /* Copy to local buffer until we have end of string or whitespace. */
    while (!done) {
        switch (*s) {
            case '\0':
                if (quote) {
                    /* Ouch, unterminated string.. */
                    return 0;
                }
                done = 1;
                break;

            case '\"':
                quote ^= 1;
                break;

            case ' ':
            case '\t':
                if (space)
                    break;

                if (!quote) {
                    done = 1;
                    break;
                }
                fallthrough;

            default:
                if (up && islower((int) *s))
                    *p++ = toupper((int) *s);
                else
                    *p++ = *s;
                space = 0;
                break;
        }

        if (!done)
            s++;
    }
    *p = '\0';

    *line = s;

    return 1;
}

static int
cdi_cue_get_keyword(char **dest, char **line)
{
    int success;

    success = cdi_cue_get_buffer(temp_keyword, line, 1);
    if (success)
        *dest = temp_keyword;

    return success;
}

/* Get a string from the input line, handling quotes properly. */
/* static uint64_t */
static uint32_t
cdi_cue_get_number(char **line)
{
    char     temp[128];
    uint32_t num;

    if (!cdi_cue_get_buffer(temp, line, 0))
        return 0;

    /* if (sscanf(temp, "%" PRIu64, &num) != 1) */
    if (sscanf(temp, "%" PRIu32, &num) != 1)
        return 0;

    return num;
}

static int
/* cdi_cue_get_frame(uint64_t *frames, char **line) */
cdi_cue_get_frame(uint32_t *frames, char **line)
{
    char temp[128];
    int  min;
    int  sec;
    int  fr;
    int  success;

    success = cdi_cue_get_buffer(temp, line, 0);
    if (!success)
        return 0;

    success = sscanf(temp, "%d:%d:%d", &min, &sec, &fr) == 3;
    if (!success)
        return 0;

    *frames = MSF_TO_FRAMES(min, sec, fr);

    return 1;
}

static int
cdi_cue_get_flags(track_t *cur, char **line)
{
    char temp[128];
    char temp2[128];
    int  success;

    success = cdi_cue_get_buffer(temp, line, 0);
    if (!success)
        return 0;

    memset(temp2, 0x00, sizeof(temp2));
    success = sscanf(temp, "%s", temp2) == 1;
    if (!success)
        return 0;

    cur->pre = (strstr(temp2, "PRE") != NULL);

    return 1;
}

static int
/* cdi_add_track(cd_img_t *cdi, track_t *cur, uint64_t *shift, uint64_t prestart, uint64_t *total_pregap, uint64_t cur_pregap) */
cdi_add_track(cd_img_t *cdi, track_t *cur, uint32_t *shift, uint32_t prestart, uint32_t *total_pregap, uint32_t cur_pregap)
{
    /* Frames between index 0 (prestart) and 1 (current track start) must be skipped. */
    /* uint64_t skip; */
    /* uint64_t temp; */
    uint32_t skip;
    uint32_t temp;
    track_t *prev = NULL;

    /* Skip *MUST* be calculated even if prestart is 0. */
    if (prestart >= 0) {
        if (prestart > cur->start)
            return 0;
        skip = cur->start - prestart;
    } else
        skip = 0;
        /* skip = 0ULL; */

    if ((cdi->tracks != NULL) && (cdi->tracks_num != 0))
        prev = &cdi->tracks[cdi->tracks_num - 1];
    else if ((cdi->tracks == NULL) && (cdi->tracks_num != 0)) {
        fatal("NULL cdi->tracks with non-zero cdi->tracks_num\n");
        return 0;
    }

    /* First track (track number must be 1). */
    if ((prev == NULL) || (cdi->tracks_num == 0)) {
        /* I guess this makes sure the structure is not filled with invalid data. */
        if (cur->number != 1)
            return 0;
        cur->skip = skip * cur->sector_size;
        cur->start += cur_pregap;
        *total_pregap = cur_pregap;
        cdi_track_push_back(cdi, cur);
        return 1;
    }

    /* Current track consumes data from the same file as the previous. */
    if (prev->file == cur->file) {
        cur->start += *shift;
        prev->length = cur->start + *total_pregap - prev->start - skip;
        cur->skip += prev->skip + (prev->length * prev->sector_size) + (skip * cur->sector_size);
        *total_pregap += cur_pregap;
        cur->start += *total_pregap;
    } else {
        temp         = prev->file->get_length(prev->file) - (prev->skip);
        /* prev->length = temp / ((uint64_t) prev->sector_size); */
        prev->length = temp / prev->sector_size;
        if ((temp % prev->sector_size) != 0)
            prev->length++;
        /* Padding. */

        cur->start += prev->start + prev->length + cur_pregap;
        cur->skip = skip * cur->sector_size;
        *shift += prev->start + prev->length;
        *total_pregap = cur_pregap;
    }

    /* Error checks. */
    if (cur->number <= 1)
        return 0;
    if ((prev->number + 1) != cur->number)
        return 0;
    if (cur->start < (prev->start + prev->length))
        return 0;

    cdi_track_push_back(cdi, cur);

    return 1;
}

int
cdi_load_cue(cd_img_t *cdi, const char *cuefile)
{
    track_t  trk;
    char     pathname[MAX_FILENAME_LENGTH];
    char     filename[MAX_FILENAME_LENGTH];
    char     temp[MAX_FILENAME_LENGTH];
    /* uint64_t shift = 0ULL; */
    /* uint64_t prestart = 0ULL; */
    /* uint64_t cur_pregap = 0ULL; */
    /* uint64_t total_pregap = 0ULL; */
    /* uint64_t frame = 0ULL; */
    /* uint64_t index; */
    uint32_t shift = 0;
    uint32_t prestart = 0;
    uint32_t cur_pregap = 0;
    uint32_t total_pregap = 0;
    uint32_t frame = 0;
    uint32_t index;
    int      success;
    int      error;
    int      can_add_track = 0;
    FIL      fp;
    char     buf[MAX_LINE_LENGTH];
    char     ansi[MAX_FILENAME_LENGTH];
    char    *line;
    char    *command;
    char    *type;
    unsigned int bytes_read;

    cdi->tracks     = NULL;
    cdi->tracks_num = 0;
    
    memset(&trk, 0, sizeof(track_t));

    /* Get a copy of the filename into pathname, we need it later. */
    memset(pathname, 0, MAX_FILENAME_LENGTH * sizeof(char));

//KM
//    path_get_dirname(pathname, cuefile);

    /* Open the file. */
//KM
//    fp = plat_fopen(cuefile, "r");

    putchar('a');
    /* fp = (FIL *) malloc(sizeof(FIL)); */
    FRESULT result = f_open(&fp, cuefile, FA_READ);      
    cdrom_image_backend_log("result: %u\n", result);
    if (result != FR_OK)
        return 0;

    FSIZE_t len = f_size(&fp);
    cdrom_image_backend_log("file size: %u", len);

    success = 0;
    /* while (f_gets(buf, sizeof buf, &fp)) { */
    /*     puts(buf); */
    /* } */
    /* int err = f_error(&fp); */
    /* cdrom_image_backend_log("error: %u", err); */
    /* return 0; */
    for (;;) {
        line = buf;
        /* Read a line from the cuesheet file. */
        //if (feof(fp) || fgets(buf, sizeof(buf), fp) == NULL || ferror(fp))

        if(f_eof(&fp)) {
           cdrom_image_backend_log("eof\n");
           break;
        } 
        if (f_gets(buf, sizeof(buf), &fp) == NULL) {
            cdrom_image_backend_log("no more read\n");
            break;        
        }
        puts(buf);
        
        /* Do two iterations to make sure to nuke even if it's \r\n or \n\r,
           but do checks to make sure we're not nuking other bytes. */
        for (uint8_t i = 0; i < 2; i++) {
            if (strlen(buf) > 0) {
                if (buf[strlen(buf) - 1] == '\n')
                    buf[strlen(buf) - 1] = '\0';
                /* nuke trailing newline */
                else if (buf[strlen(buf) - 1] == '\r')
                    buf[strlen(buf) - 1] = '\0';
                /* nuke trailing newline */
            }
        }

        success = cdi_cue_get_keyword(&command, &line);
        cdrom_image_backend_log("command: %s\n", command);

        if (!strcmp(command, "TRACK")) {            
            if (can_add_track) {                            
                success = cdi_add_track(cdi, &trk, &shift, prestart, &total_pregap, cur_pregap);
            }
            else {
                success = 1;
            }

            if (!success)
                break;
            
            trk.start  = 0;
            trk.skip   = 0;
            cur_pregap = 0;
            prestart   = 0;

            trk.number       = cdi_cue_get_number(&line);
            trk.track_number = trk.number;
            success          = cdi_cue_get_keyword(&type, &line);            
            if (!success)
                break;

            trk.form  = 0;
            trk.mode2 = 0;

            trk.pre = 0;

            if (!strcmp(type, "AUDIO")) {
                trk.sector_size = RAW_SECTOR_SIZE;
                trk.attr        = AUDIO_TRACK;
            } else if (!strcmp(type, "MODE1/2048")) {
                trk.sector_size = COOKED_SECTOR_SIZE;
                trk.attr        = DATA_TRACK;
            } else if (!strcmp(type, "MODE1/2352")) {
                trk.sector_size = RAW_SECTOR_SIZE;
                trk.attr        = DATA_TRACK;
            } else if (!strcmp(type, "MODE1/2448")) {
                trk.sector_size = 2448;
                trk.attr        = DATA_TRACK;
            } else if (!strcmp(type, "MODE2/2048")) {
                trk.form        = 1;
                trk.sector_size = COOKED_SECTOR_SIZE;
                trk.attr        = DATA_TRACK;
                trk.mode2       = 1;
            } else if (!strcmp(type, "MODE2/2324")) {
                trk.form        = 2;
                trk.sector_size = 2324;
                trk.attr        = DATA_TRACK;
                trk.mode2       = 1;
            } else if (!strcmp(type, "MODE2/2328")) {
                trk.form        = 2;
                trk.sector_size = 2328;
                trk.attr        = DATA_TRACK;
                trk.mode2       = 1;
            } else if (!strcmp(type, "MODE2/2336")) {
                trk.sector_size = 2336;
                trk.attr        = DATA_TRACK;
                trk.mode2       = 1;
            } else if (!strcmp(type, "MODE2/2352")) {
                /* Assume this is XA Mode 2 Form 1. */
                trk.form        = 1;
                trk.sector_size = RAW_SECTOR_SIZE;
                trk.attr        = DATA_TRACK;
                trk.mode2       = 1;
            } else if (!strcmp(type, "MODE2/2448")) {
                /* Assume this is XA Mode 2 Form 1. */
                trk.form        = 1;
                trk.sector_size = 2448;
                trk.attr        = DATA_TRACK;
                trk.mode2       = 1;
            } else if (!strcmp(type, "CDG/2448")) {
                trk.sector_size = 2448;
                trk.attr        = DATA_TRACK;
                trk.mode2       = 1;
            } else if (!strcmp(type, "CDI/2336")) {
                trk.sector_size = 2336;
                trk.attr        = DATA_TRACK;
                trk.mode2       = 1;
            } else if (!strcmp(type, "CDI/2352")) {                
                trk.sector_size = RAW_SECTOR_SIZE;
                trk.attr        = DATA_TRACK;
                trk.mode2       = 1;
            } else
                success = 0;

            can_add_track = 1;
        } else if (!strcmp(command, "INDEX")) {
            index   = cdi_cue_get_number(&line);
            success = cdi_cue_get_frame(&frame, &line);

            switch (index) {
                case 0:
                    prestart = frame;
                    break;

                case 1:
                    trk.start = frame;
                    break;

                default:
                    /* Ignore other indices. */
                    break;
            }
        } else if (!strcmp(command, "FILE")) {
            putchar('1');
            if (can_add_track) {
                success = cdi_add_track(cdi, &trk, &shift, prestart, &total_pregap, cur_pregap);
            } else {
                success = 1;
            }
            if (!success) {
                putchar('x');
                break;
            }
            can_add_track = 0;

            putchar('2');
            memset(ansi, 0, MAX_FILENAME_LENGTH * sizeof(char));
            memset(filename, 0, MAX_FILENAME_LENGTH * sizeof(char));

            putchar('3');
            success = cdi_cue_get_buffer(ansi, &line, 0);
            if (!success)
                break;
            putchar('4');
            success = cdi_cue_get_keyword(&type, &line);
            if (!success)
                break;

            putchar('5');
            trk.file = NULL;
            error    = 1;

            putchar('6');
            if (!strcmp(type, "BINARY")) {
                memset(temp, 0, MAX_FILENAME_LENGTH * sizeof(char));
//KM//                path_append_filename(filename, pathname, ansi);
                strncpy(filename,ansi,MAX_FILENAME_LENGTH);
                trk.file = track_file_init(filename, &error);
                if (trk.file) {
                    error = 0;
                }
            }
            putchar('7');
            if (error) {
/* #ifdef ENABLE_CDROM_IMAGE_BACKEND_LOG */
                cdrom_image_backend_log("CUE: cannot open fille '%s' in cue sheet!\n",
                                        filename);
/* #endif */
                if (trk.file != NULL) {
                    trk.file->close(trk.file);
                    trk.file = NULL;
                }
                success = 0;
            }
        } else if (!strcmp(command, "PREGAP"))
            success = cdi_cue_get_frame(&cur_pregap, &line);
        else if (!strcmp(command, "FLAGS"))
            success = cdi_cue_get_flags(&trk, &line);
        else if (!strcmp(command, "CATALOG") || !strcmp(command, "CDTEXTFILE") || !strcmp(command, "ISRC") || !strcmp(command, "PERFORMER") || !strcmp(command, "POSTGAP") || !strcmp(command, "REM") || !strcmp(command, "SONGWRITER") || !strcmp(command, "TITLE") || !strcmp(command, "")) {
            /* Ignored commands. */
            success = 1;
        } else {
/* #ifdef ENABLE_CDROM_IMAGE_BACKEND_LOG */
            cdrom_image_backend_log("CUE: unsupported command '%s' in cue sheet!\n", command);
/* #endif */
            success = 0;
        }

        putchar('z');
        if (!success)
            break;
    }

    f_close(&fp);
    if (!success)
        return 0;

    /* Add last track. */
    if (!cdi_add_track(cdi, &trk, &shift, prestart, &total_pregap, cur_pregap))
        return 0;

    /* Add lead out track. */
    trk.number++;
    trk.track_number = 0xAA;
    trk.attr         = 0x16; /* Was 0x00 but I believe 0x16 is appropriate. */
    trk.start        = 0;
    trk.length       = 0;
    trk.file         = NULL;
    if (!cdi_add_track(cdi, &trk, &shift, 0, &total_pregap, 0))
        return 0;

    return 1;
}

int
cdi_has_data_track(cd_img_t *cdi)
{
    if ((cdi == NULL) || (cdi->tracks == NULL))
        return 0;

    /* Data track has attribute 0x14. */
    for (int i = 0; i < cdi->tracks_num; i++) {
        if (cdi->tracks[i].attr == DATA_TRACK)
            return 1;
    }

    return 0;
}

int
cdi_has_audio_track(cd_img_t *cdi)
{
    if ((cdi == NULL) || (cdi->tracks == NULL))
        return 0;

    /* Audio track has attribute 0x14. */
    for (int i = 0; i < cdi->tracks_num; i++) {
        if (cdi->tracks[i].attr == AUDIO_TRACK)
            return 1;
    }

    return 0;
}
