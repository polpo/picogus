/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <math.h>
#include <string.h>

#if PICO_ON_DEVICE

#include "hardware/clocks.h"
#include "hardware/structs/clocks.h"

#endif

#include "pico/stdlib.h"

#include "pico/audio_i2s.h"

#include "cRSID/libcRSID.h"
cRSID_C64instance C64;
// cRSID_SIDinstance* SID;
extern uint32_t last_sid_tick;
#include "pico/critical_section.h"
extern critical_section_t sid_crit;

#include "pico/time.h"
alarm_pool_t* alarm_pool;

#include "hardware/interp.h"

#if PICO_ON_DEVICE
#include "pico/binary_info.h"
bi_decl(bi_3pins_with_names(PICO_AUDIO_I2S_DATA_PIN, "I2S DIN", PICO_AUDIO_I2S_CLOCK_PIN_BASE, "I2S BCK", PICO_AUDIO_I2S_CLOCK_PIN_BASE+1, "I2S LRCK"));
#endif

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
            .pio_sm = 1,
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

static int16_t __force_inline clamp16(int32_t d) {
    // interp1->accum[0] = d;
    // return interp1->peek[0];
    if (d > 32767) { return 32767; }
    if (d < -32768) { return -32768; }
    return d;
}

int64_t handle(alarm_id_t id, void *user_data) {
    cRSID_emulateADSRs(&C64.SID, 7);
    return -7;
}

void play_sid() {
    puts("starting core 1 SID");

    alarm_pool = alarm_pool_create(2, PICO_TIME_DEFAULT_ALARM_POOL_MAX_TIMERS);
    irq_set_priority(TIMER_IRQ_2, PICO_HIGHEST_IRQ_PRIORITY);
    alarm_pool_add_alarm_in_us(alarm_pool, 7, handle, 0, true);
    /*
    // Clamp setup
    interp_config cfg = interp_default_config();
    interp_config_set_clamp(&cfg, true);
    interp_config_set_shift(&cfg, 14);
    // set mask according to new position of sign bit..
    interp_config_set_mask(&cfg, 0, 17);
    // ...so that the shifted value is correctly sign extended
    interp_config_set_signed(&cfg, true);
    interp_set_config(interp1, 0, &cfg);
    interp1->base[0] = -32768;
    interp1->base[1] = 32767;
    */

    C64.SampleRate = 44100;
    C64.CPUfrequency = 985248;
    C64.SampleClockRatio = (C64.CPUfrequency<<4)/C64.SampleRate; //shifting (multiplication) enhances SampleClockRatio precision
    C64.Attenuation = 26;
    cRSID_createSIDchip(&C64, &C64.SID, 8580);

    // uint32_t instruction_pool = (44100 << 4)

    struct audio_buffer_pool *ap = init_audio();
    for (;;) {
        struct audio_buffer *buffer = take_audio_buffer(ap, true);
        int16_t *samples = (int16_t *) buffer->buffer->bytes;
        // cRSID_emulateADSRs(&C64.SID, 22);
        // samples[0] = samples[1] = clamp16(cRSID_emulateWaves(&C64.SID));
        // cRSID_emulateADSRs(&C64.SID, 2);
        for(uint32_t i = 0; i < SAMPLES_PER_BUFFER; ++i) {
            // cRSID_emulateADSRs(&C64.SID, SAMPLES_PER_BUFFER * C64.SampleClockRatio);
            // cRSID_emulateADSRs(&C64.SID, 7);
            // critical_section_enter_blocking(&sid_crit);
            // uint32_t cur_tick = time_us_32();
            // cRSID_emulateADSRs(&C64.SID, cur_tick - last_sid_tick);
            // last_sid_tick = cur_tick;
            // critical_section_exit(&sid_crit);
            samples[i << 1] = samples[(i << 1) + 1] = clamp16(cRSID_emulateWaves(&C64.SID));
            // samples[i << 1] = samples[(i << 1) + 1] = cRSID_emulateWaves(&C64.SID);
            // printf("%d ", samples[i << 1]);
        }
        // printf("%u ", cms_audio_elapsed);
        // buffer->sample_count = 1; //buffer->max_sample_count;
        buffer->sample_count = buffer->max_sample_count;
        // buffer->sample_count = 4;

        give_audio_buffer(ap, buffer);
    }
}
