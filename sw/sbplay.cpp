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

#include "opl.h"
extern "C" void OPL_Pico_simple(int16_t *buffer, uint32_t nsamples);
extern "C" void OPL_Pico_PortWrite(opl_port_t, unsigned int);

#ifdef SOUND_SB
extern int16_t sbdsp_sample();
#endif
#if defined(SOUND_SB) || defined(USB_MOUSE) || defined(SOUND_MPU)
#include "pico_pic.h"
#endif

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
Minimum expected sample rate from DSP should be 8000hz?
Maximum number of DSP to process at once should be 64.
49716 / 8000 = 6.2145 * 64 = 397
*/
#define SAMPLES_PER_BUFFER 512

struct audio_buffer_pool *init_audio() {

    static audio_format_t audio_format = {
            .sample_freq = 49716,
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

    for (;;) {
        bool notfirst = false;
        while (opl_buffer.tail != opl_buffer.head) {
            if (!notfirst) {
#ifndef PICOW
                gpio_xor_mask(LED_PIN);
#endif
                notfirst = true;
            }
            auto cmd = opl_buffer.cmds[opl_buffer.tail];
            OPL_Pico_PortWrite((opl_port_t)cmd.addr, cmd.data);
            // putchar('.');
            ++opl_buffer.tail;
        }
        struct audio_buffer *buffer = take_audio_buffer(ap, true);
        int16_t *samples = (int16_t *) buffer->buffer->bytes;
        int16_t opl_sample;
        OPL_Pico_simple(&opl_sample, 1);
#ifdef SOUND_SB
        samples[0] = samples[1] = clamp16((int32_t)sbdsp_sample() + (int32_t)opl_sample);
#else
        samples[0] = samples[1] = opl_sample;
#endif
        buffer->sample_count=1;
        // putchar((unsigned char)buffer->buffer->bytes[1]);
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
        send_midi_bytes(1);
#endif
    }
}
