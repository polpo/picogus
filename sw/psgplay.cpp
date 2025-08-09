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

#include "square/square.h"

#include "include/cmd_buffers.h"
#include "audio/volctrl.h"

#if SOUND_TANDY
extern tandy_buffer_t tandy_buffer;
#endif
#if SOUND_CMS
extern cms_buffer_t cms_buffer;
#endif

extern uint LED_PIN;

#ifdef USB_STACK
#include "tusb.h"
#endif
#if defined(USB_MOUSE) || defined(SOUND_MPU)
#include "system/pico_pic.h"
#endif

#ifdef USB_MOUSE
#include "mouse/8250uart.h"
#include "mouse/sermouse.h"
#endif

#include <string.h>

#if PICO_ON_DEVICE
#include "pico/binary_info.h"
bi_decl(bi_3pins_with_names(PICO_AUDIO_I2S_DATA_PIN, "I2S DIN", PICO_AUDIO_I2S_CLOCK_PIN_BASE, "I2S BCK", PICO_AUDIO_I2S_CLOCK_PIN_BASE+1, "I2S LRCK"));
#endif

#ifdef SOUND_MPU
#include "system/flash_settings.h"
extern Settings settings;
#include "mpu401/export.h"
#endif

#define SAMPLES_PER_BUFFER 8

struct audio_buffer_pool *init_audio() {

    static audio_format_t audio_format = {
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

    ok = audio_i2s_connect_extra(producer_pool, false, 0, 0, NULL);
    assert(ok);
    audio_i2s_set_enabled(true);
    set_volume(CMD_PSGVOL);
    return producer_pool;
}

void play_psg() {
    puts("starting core 1 psg");
    // flash_safe_execute_core_init();

#if defined(USB_MOUSE) || defined(SOUND_MPU)
    // Init PIC on this core so it handles timers
    PIC_Init();
    puts("pic inited on core 1");
#endif
#ifdef USB_STACK
    // Init TinyUSB for joystick support
    tuh_init(BOARD_TUH_RHPORT);
#endif

#ifdef SOUND_MPU
    MPU401_Init(settings.MPU.delaySysex, settings.MPU.fakeAllNotesOff);
#endif

#if SOUND_TANDY
    tandysound_t tandysound;
#endif
#if SOUND_CMS
    cms_t cms;
#endif

    struct audio_buffer_pool *ap = init_audio();
    int32_t buf[SAMPLES_PER_BUFFER * 2];
    for (;;) {
        bool notfirst = false;
#if SOUND_TANDY
        while (tandy_buffer.tail != tandy_buffer.head) {
            if (!notfirst) {
                gpio_xor_mask(LED_PIN);
                notfirst = true;
            }
            tandysound.write_register(0, tandy_buffer.cmds[tandy_buffer.tail]);
            ++tandy_buffer.tail;
        }
#endif // SOUND_TANDY
#if SOUND_CMS
        while (cms_buffer.tail != cms_buffer.head) {
            if (!notfirst) {
                gpio_xor_mask(LED_PIN);
                notfirst = true;
            }
            auto cmd = cms_buffer.cmds[cms_buffer.tail];
            if (cmd.addr & 1) {
                cms.write_addr(cmd.addr, cmd.data);
            } else {
                cms.write_data(cmd.addr, cmd.data);
            }
            ++cms_buffer.tail;
        }
#endif

        memset(buf, 0, sizeof(buf));
        struct audio_buffer *buffer = take_audio_buffer(ap, true);
        int16_t *samples = (int16_t *) buffer->buffer->bytes;
      
#if SOUND_TANDY
        tandysound.generator().generate_frames(buf, SAMPLES_PER_BUFFER);
#endif
#if SOUND_CMS
        cms.generator(0).generate_frames(buf, SAMPLES_PER_BUFFER);
        cms.generator(1).generate_frames(buf, SAMPLES_PER_BUFFER);
#endif
        for (int i = 0; i < SAMPLES_PER_BUFFER; ++i) {          
            samples[i << 1] = scale_sample(buf[i << 1], psg_volume, 0);
            samples[(i << 1) + 1] = scale_sample(buf[(i << 1) + 1], psg_volume, 0);
        }
        buffer->sample_count = SAMPLES_PER_BUFFER;

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
    }
}
