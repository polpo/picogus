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

#include "tusb.h"
#include "mouse/8250uart.h"
#include "mouse/sermouse.h"

// #include <string.h>
#ifdef CDROM
#include "cdrom/cdrom.h"
#include "pico/audio_i2s.h"
#endif

#ifdef SOUND_MPU
#include "system/flash_settings.h"
extern Settings settings;
#include "mpu401/export.h"
#endif

#include "system/pico_pic.h"

#ifdef CDROM
#define SAMPLES_PER_BUFFER 256

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

    //ok = audio_i2s_connect(producer_pool);
    ok = audio_i2s_connect_extra(producer_pool, false, 0, 0, NULL);
    assert(ok);
    audio_i2s_set_enabled(true);
    return producer_pool;
}
#endif // CDROM

void play_usb() {
    puts("starting core 1 USB");
    // flash_safe_execute_core_init();

    // board_init();

    // Init PIC on this core so it handles timers
    PIC_Init();
    puts("pic inited on core 1");

    // init host stack on configured roothub port
    tuh_init(BOARD_TUH_RHPORT);

#ifdef SOUND_MPU
    MPU401_Init(settings.MPU.delaySysex, settings.MPU.fakeAllNotesOff);
#endif

#ifdef CDROM
    struct audio_buffer_pool *ap = init_audio();
#endif
    for (;;) {
#ifdef CDROM
        if (cdrom.cd_status == CD_STATUS_PLAYING) {
            struct audio_buffer *buffer = take_audio_buffer(ap, true);
            int16_t *samples = (int16_t *) buffer->buffer->bytes;
            buffer->sample_count = cdrom_audio_callback_simple(&cdrom, samples, SAMPLES_PER_BUFFER << 1, false) >> 1;
            if (buffer->sample_count == 0) {
                // If we got no samples back, playback stopped so output a sample of silence
                // (give_audio_buffer does not tolerate 0 samples, so we have to emit 1)
                samples[0] = samples[1] = 0;
                buffer->sample_count = 1;
            }
            give_audio_buffer(ap, buffer);
#ifdef SOUND_MPU
            send_midi_bytes(32);
        } else {
            send_midi_bytes(8);
#endif // SOUND_MPU
        }
        cdrom_tasks(&cdrom);
#else // CDROM
#ifdef SOUND_MPU
        send_midi_bytes(8);
#endif // SOUND_MPU
#endif // CDROM
        // tinyusb host task
        tuh_task();

        // mouse task
        sermouse_core1_task();

        // uart emulation task
        uartemu_core1_task();
    }
}
