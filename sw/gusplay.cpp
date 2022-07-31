/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include "stdio_async_uart.h"
#include <math.h>

#if PICO_ON_DEVICE
#include "hardware/clocks.h"
#include "hardware/structs/clocks.h"
#endif

#include "pico/stdlib.h"

#include "pico/audio_i2s.h"

#if PICO_ON_DEVICE
#include "pico/binary_info.h"
bi_decl(bi_3pins_with_names(PICO_AUDIO_I2S_DATA_PIN, "I2S DIN", PICO_AUDIO_I2S_CLOCK_PIN_BASE, "I2S BCK", PICO_AUDIO_I2S_CLOCK_PIN_BASE+1, "I2S LRCK"));
#endif

// #include "gus.h"
#include "gus-x.h"

#define SAMPLES_PER_BUFFER 1024

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
            .pio_sm = 0,
    };

    output_format = audio_i2s_setup(&audio_format, &config);
    if (!output_format) {
        panic("PicoAudio: Unable to open audio device.\n");
    }

    //ok = audio_i2s_connect(producer_pool);
    ok = audio_i2s_connect_extra(producer_pool, false, 1, 1024, NULL);
    assert(ok);
    audio_i2s_set_enabled(true);
    return producer_pool;
}

// extern Gus *gus;

void play_gus() {
    puts("starting core 1");
    uint32_t start, end;

    struct audio_buffer_pool *ap = init_audio();
    for (;;) {
        uint8_t active_voices = GUS_activeChannels();
        uint32_t playback_rate = GUS_basefreq();
        struct audio_buffer *buffer = take_audio_buffer(ap, true);
        /* gus->dothangs; */
        int16_t *samples = (int16_t *) buffer->buffer->bytes;

        uint32_t gus_audio_begin = time_us_32();
        // gus->AudioCallback(buffer->max_sample_count, samples);
        GUS_CallBack(buffer->max_sample_count, samples);
        uint32_t gus_audio_elapsed = time_us_32() - gus_audio_begin;
        if (/*gus->*/active_voices) {
            // printf("%d\n", gus->active_voices);
            // printf("%d us %d samples (tgt 23220)\n", gus_audio_elapsed, buffer->max_sample_count);
            uart_print_hex_u32(gus_audio_elapsed);
        }
        buffer->sample_count = buffer->max_sample_count;
        // if GUS sample rate changed
        if (/*gus->*/active_voices && (ap->format->sample_freq != /*gus->*/playback_rate)) {
            printf("changing sample rate to %d", /*gus->*/playback_rate);
            // todo hack overwriting const
            ((struct audio_format *) ap->format)->sample_freq = /*gus->*/playback_rate;
        }
        give_audio_buffer(ap, buffer);
    }
}
