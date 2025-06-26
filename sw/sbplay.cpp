/*
 *  Copyright (C) 2022-2024  Ian Scott
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <stdio.h>
#include <math.h>

#if PICO_ON_DEVICE

#include "hardware/clocks.h"
#include "hardware/structs/clocks.h"

#endif

#include "pico/stdlib.h"
#include "pico/audio_i2s.h"
#include "volctrl.h"

#include "opl.h"
extern "C" void OPL_Pico_simple(int16_t *buffer, uint32_t nsamples);
extern "C" void OPL_Pico_WriteRegister(unsigned int reg_num, unsigned int value);

#include "audio_fifo.h"
#if SOUND_SB
#if SB_BUFFERLESS
extern int16_t sbdsp_sample();
#else // SB_BUFFERLESS
extern uint16_t sbdsp_sample_rate();
extern uint16_t sbdsp_muted();
extern audio_fifo_t* sbdsp_fifo_peek();
#endif // SB_BUFFERLESS
#endif // SOUND_SB
#if defined(SOUND_SB) || defined(USB_MOUSE) || defined(SOUND_MPU)
#include "pico_pic.h"
#endif

#if CDROM
#include "cdrom/cdrom.h"
// extern cdrom_t cdrom;
#endif // CDROM

#include "clamp.h"

#include "cmd_buffers.h"
extern cms_buffer_t opl_buffer;

#ifdef USB_STACK
#include "tusb.h"
#endif
#ifdef USB_MOUSE
#include "mouse/8250uart.h"
#include "mouse/sermouse.h"
#endif

#ifdef SOUND_MPU
#include "flash_settings.h"
extern Settings settings;
#include "mpu401/export.h"
#endif

extern uint LED_PIN;


#if PICO_ON_DEVICE
#include "pico/binary_info.h"
bi_decl(bi_3pins_with_names(PICO_AUDIO_I2S_DATA_PIN, "I2S DIN", PICO_AUDIO_I2S_CLOCK_PIN_BASE, "I2S BCK", PICO_AUDIO_I2S_CLOCK_PIN_BASE+1, "I2S LRCK"));
#endif


/*
Minimum expected aample rate from DSP should be 8000hz?
Maximum number of DSP to process at once should be 64.
49716 / 8000 = 6.2145 * 64 = 397
*/
#define SAMPLES_PER_BUFFER 256

struct audio_buffer_pool *init_audio() {

    static audio_format_t audio_format = {
            //.sample_freq = 49716,
            .sample_freq = 44100,
            .format = AUDIO_BUFFER_FORMAT_PCM_S16,
            .channel_count = 2,
    };

    static struct audio_buffer_format producer_format = {
            .format = &audio_format,
            .sample_stride = 4
    };

    struct audio_buffer_pool *producer_pool = audio_new_producer_pool(&producer_format, 2,
                                                                      SAMPLES_PER_BUFFER); // todo correct size
    bool __unused ok;
    const struct audio_format *output_format;
    struct audio_i2s_config config = {
            .data_pin = PICO_AUDIO_I2S_DATA_PIN,
            .clock_pin_base = PICO_AUDIO_I2S_CLOCK_PIN_BASE,
            .dma_channel = 6,
            .pio_sm = 3,
    };

    output_format = audio_i2s_setup(&audio_format, &config);
    if (!output_format) {
        panic("PicoAudio: Unable to open audio device.\n");
    }

    //ok = audio_i2s_connect(producer_pool);
    ok = audio_i2s_connect_extra(producer_pool, false, 0, 0, NULL);
    assert(ok);
    audio_i2s_set_enabled(true);

    return producer_pool;
}

/* Fixed-point format: Q16.16 (16 bits integer, 16 bits fractional) */
static constexpr uint32_t FRAC_BITS = 16;
static constexpr uint32_t FRAC_MASK = (1u << FRAC_BITS) - 1;

static inline uint32_t fixed_ratio(uint16_t a, uint16_t b) {
    return ((uint32_t)a << FRAC_BITS) / b;
}

/**
 * Linear interpolation in fixed-point
 * v0 and v1 are sample values, frac is the fractional position
 */
static inline int16_t lerp_fixed(int16_t v0, int16_t v1, uint32_t frac) {
    return v0 + (int16_t)(((int32_t)(v1 - v0) * frac) >> FRAC_BITS);
}

void play_adlib() {
    puts("starting core 1");
    // flash_safe_execute_core_init();
    uint32_t start, end;

#if defined(SOUND_SB) || defined(USB_MOUSE) || defined(SOUND_MPU)
    // Init PIC on this core so it handles timers
    PIC_Init();
#endif

#ifdef USB_STACK
    // Init TinyUSB for joystick support
    tuh_init(BOARD_TUH_RHPORT);
#endif

    clamp_setup();

#ifdef SOUND_MPU
    MPU401_Init(settings.MPU.delaySysex, settings.MPU.fakeAllNotesOff);
#endif

    struct audio_buffer_pool *ap = init_audio();

#if SOUND_SB && !SB_BUFFERLESS
    // uint8_t sb_samples[512] = {128};
    audio_fifo_t* sb_fifo = sbdsp_fifo_peek();
#endif
#ifdef CDROM
    int16_t cd_samples[SAMPLES_PER_BUFFER * 2];
    bool has_cd_samples;
#endif

#ifdef SOUND_SB
    // int16_t sb_sample = 0;
    uint32_t sb_ratio = 0;
    uint32_t sb_pos = 0;
    uint32_t sb_index_old = 0xffffffff;
    uint32_t sb_index = 0;
    uint32_t sb_frac = 0;
    // uint32_t sb_sample_idx = 0x0;
    uint32_t sb_left = 0;
#endif

    uint32_t cd_left = 0;
    uint32_t cd_index = 0;

    constexpr uint32_t OPL_SAMPLE_COUNT = 8;
    constexpr uint32_t OPL_BUFFER_BITS = (OPL_SAMPLE_COUNT << 1) - 1;
    int16_t opl_samples[OPL_SAMPLE_COUNT << 1] = {0};
    uint32_t opl_ratio = fixed_ratio(49716, 44100);
    printf("opl_ratio: %x ", opl_ratio);
    uint32_t opl_pos = 0;
    uint32_t opl_index = 0;
    uint32_t opl_sample_idx = 0x0;

    // int16_t cd_samples[1024] = {0};

    int32_t accum[2] = {0};
    for (;;) {
#if CDROM
        has_cd_samples = cdrom_audio_callback_simple(&cdrom, cd_samples, SAMPLES_PER_BUFFER << 1, true);
#endif
        struct audio_buffer *buffer = take_audio_buffer(ap, true);
        int16_t *samples = (int16_t *) buffer->buffer->bytes;
        // Do mixing with lerp
        //
#if !SB_BUFFERLESS
        for (int i = 0; i < SAMPLES_PER_BUFFER; ++i) {
#endif // !SB_BUFFERLESS
            accum[0] = accum[1] = 0;
#if CDROM
            if (has_cd_samples) {
                accum[0] += cd_samples[i << 1];
                accum[1] += cd_samples[(i << 1) + 1];
            }
#endif // CDROM
            uint32_t opl_index = (opl_pos >> FRAC_BITS);
            // uint32_t opl_index = i;
            uint32_t opl_frac = opl_pos & FRAC_MASK;
            opl_pos += opl_ratio;
#if !SB_BUFFERLESS // don't support OPL in bufferless for now
            if (
                ((opl_index & OPL_SAMPLE_COUNT) && opl_sample_idx != 0x0) ||
                (!(opl_index & OPL_SAMPLE_COUNT) && opl_sample_idx != OPL_SAMPLE_COUNT)
            ) {
                opl_sample_idx = (opl_index + OPL_SAMPLE_COUNT) & OPL_SAMPLE_COUNT;
                // bool notfirst = false;
                while (opl_buffer.tail != opl_buffer.head) {
                    // if (!notfirst) {
#ifndef PICOW
                    //     gpio_xor_mask(LED_PIN);
#endif
                    //     notfirst = true;
                    // }
                    auto cmd = opl_buffer.cmds[opl_buffer.tail];
                    OPL_Pico_WriteRegister(cmd.addr, cmd.data);
                    ++opl_buffer.tail;
                }
                OPL_Pico_simple(opl_samples + opl_sample_idx, OPL_SAMPLE_COUNT);
            }
            int32_t opl_sample = lerp_fixed(
                opl_samples[opl_index & OPL_BUFFER_BITS],
                opl_samples[(opl_index + 1) & OPL_BUFFER_BITS],
                opl_frac);            
           
            opl_sample = scale_sample(opl_sample << 2, opl_volume, 1);

            accum[0] += opl_sample;
            accum[1] += opl_sample;

#if SOUND_SB
            if (!sb_left) {
                // putchar('t');
                uint32_t num_samples = 256;
                sb_left = fifo_take_samples(sb_fifo, num_samples);
            }
            if (sb_left) {
                sb_index = (sb_pos >> FRAC_BITS);
                if (sb_index_old != sb_index) {
                    sb_left--;
                }
                sb_index_old = sb_index;
                sb_frac = sb_pos & FRAC_MASK;
                //sb_ratio = fixed_ratio(sbdsp_sample_rate(), 44100);

                static uint16_t last_sb_rate = 0;
                uint16_t current_rate = sbdsp_sample_rate();
                if (current_rate != last_sb_rate)
                {
                    sb_ratio = fixed_ratio(current_rate, 44100);
                    last_sb_rate = current_rate;
                }

                sb_pos += sb_ratio;
                // putchar('p');
                if (!sbdsp_muted()) {
                    //int16_t sb_sample = sb_fifo->buffer[sb_index & AUDIO_FIFO_BITS];

                    int16_t *fifo_buf = sb_fifo->buffer;
                    int16_t sb_sample = fifo_buf[sb_index & AUDIO_FIFO_BITS];
                    
                    sb_sample = scale_sample((int32_t)sb_sample >> 1, sb_volume, 1);

                    accum[0] += sb_sample;
                    accum[1] += sb_sample;
                }
            }
#endif // SOUND_SB
            samples[i << 1] = clamp16(accum[0]);
            samples[(i << 1) + 1] = clamp16(accum[1]);
        }
        buffer->sample_count = SAMPLES_PER_BUFFER;
#else // !SB_BUFFERLESS
        samples[0] = clamp16(accum[0]);
        samples[1] = clamp16(accum[1]);
        buffer->sample_count = 1;
#endif // !SB_BUFFERLESS
        give_audio_buffer(ap, buffer);
#ifdef USB_STACK
        // Service TinyUSB events
        tuh_task();
#endif
#ifdef USB_MOUSE
        // mouse task
        sermouse_core1_task();

        // uart emulation task
        uartemu_core1_task();
#endif
#ifdef SOUND_MPU
        send_midi_bytes(8);
#endif
#ifdef CDROM
        cdrom_tasks(&cdrom);
#endif
    }
}
