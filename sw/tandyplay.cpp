/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <math.h>

#if PICO_ON_DEVICE

#include "hardware/clocks.h"
#include "hardware/structs/clocks.h"

#endif

#include "pico/stdlib.h"
#include "pico/audio_i2s.h"
#include "pico/flash.h"

#include "square/square.h"

#include "cmd_buffers.h"
extern tandy_buffer_t tandy_buffer;

extern uint LED_PIN;

#ifdef USB_JOYSTICK
#include "tusb.h"
#endif

#include <string.h>

#if PICO_ON_DEVICE
#include "pico/binary_info.h"
bi_decl(bi_3pins_with_names(PICO_AUDIO_I2S_DATA_PIN, "I2S DIN", PICO_AUDIO_I2S_CLOCK_PIN_BASE, "I2S BCK", PICO_AUDIO_I2S_CLOCK_PIN_BASE+1, "I2S LRCK"));
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
    return producer_pool;
}

void play_tandy() {
    puts("starting core 1 tandy");
    flash_safe_execute_core_init();
#ifdef USB_JOYSTICK
    // Init TinyUSB for joystick support
    tuh_init(BOARD_TUH_RHPORT);
#endif

    tandysound_t tandysound;
    struct audio_buffer_pool *ap = init_audio();
#ifdef SQUARE_FLOAT_OUTPUT
    float buf[SAMPLES_PER_BUFFER * 2];
#else
    int32_t buf[SAMPLES_PER_BUFFER * 2];
#endif
    for (;;) {
        bool notfirst = false;
        while (tandy_buffer.tail != tandy_buffer.head) {
            if (!notfirst) {
                gpio_xor_mask(LED_PIN);
                notfirst = true;
            }
            tandysound.write_register(0, tandy_buffer.cmds[tandy_buffer.tail]);
            ++tandy_buffer.tail;
        }
        memset(buf, 0, sizeof(buf));
        struct audio_buffer *buffer = take_audio_buffer(ap, true);
        int16_t *samples = (int16_t *) buffer->buffer->bytes;
      
#ifdef SQUARE_FLOAT_OUTPUT
        tandysound.generator().generate_frames(buf, SAMPLES_PER_BUFFER, 1.0f);
#else
        tandysound.generator().generate_frames(buf, SAMPLES_PER_BUFFER);
#endif
        for (int i = 0; i < SAMPLES_PER_BUFFER; ++i) {
#ifdef SQUARE_FLOAT_OUTPUT
            samples[i << 1] = (int32_t)(buf[i << 1] * 32768.0f) >> 1;
            samples[(i << 1) + 1] = (int32_t)(buf[(i << 1) + 1] * 32768.0f) >> 1;
#else
            samples[i << 1] = buf[i << 1] >> 1;
            samples[(i << 1) + 1] = buf[(i << 1) + 1] >> 1;
#endif
        }
        buffer->sample_count = SAMPLES_PER_BUFFER;

        give_audio_buffer(ap, buffer);
#ifdef USB_JOYSTICK
        // Service TinyUSB events
        tuh_task();
#endif
    }
}
