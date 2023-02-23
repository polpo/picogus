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

#ifdef OPL_YMFM
#include "ymfm_opl.h"
extern ymfm::ymf262* myOPL;
#include "pico/critical_section.h"
extern critical_section_t opl_crit;
#else
#include "opl.h"
extern "C" void OPL_Pico_Mix_callback(audio_buffer_t *);
#endif

#if PICO_ON_DEVICE
#include "pico/binary_info.h"
bi_decl(bi_3pins_with_names(PICO_AUDIO_I2S_DATA_PIN, "I2S DIN", PICO_AUDIO_I2S_CLOCK_PIN_BASE, "I2S BCK", PICO_AUDIO_I2S_CLOCK_PIN_BASE+1, "I2S LRCK"));
#endif

#define SAMPLES_PER_BUFFER 64

struct audio_buffer_pool *init_audio() {

    static audio_format_t audio_format = {
            .sample_freq = 49716 / 3,
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
            .pio_sm = 1,
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
    uint32_t start, end;

    struct audio_buffer_pool *ap = init_audio();
#ifdef OPL_YMFM
    ymfm::ymf262::output_data output;
#endif
    for (;;) {
        struct audio_buffer *buffer = take_audio_buffer(ap, true);
#ifdef OPL_YMFM
        int16_t *samples = (int16_t *) buffer->buffer->bytes;
        uint32_t audio_begin = time_us_32();
        for (int i = 0; i < buffer->max_sample_count; ++i) {
            critical_section_enter_blocking(&opl_crit);
            myOPL->generate(&output);
            critical_section_exit(&opl_crit);
            samples[i << 1] = output.data[0];
            samples[(i << 1) + 1] = output.data[1];
        }
        uint32_t audio_elapsed = time_us_32() - audio_begin;
        // if (audio_elapsed > 1280) {
        //     printf("took too long: %u %u\n", audio_elapsed, buffer->max_sample_count);
        // }
        printf("%u ", audio_elapsed);
        buffer->sample_count = buffer->max_sample_count;
        // putchar('=');
#else
        OPL_Pico_Mix_callback(buffer);
#endif
        // putchar((unsigned char)buffer->buffer->bytes[1]);
        give_audio_buffer(ap, buffer);
    }
}
