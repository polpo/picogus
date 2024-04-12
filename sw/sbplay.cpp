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

#ifdef OPL_NUKED
#include "Nuked-OPL3/opl3.h"
extern opl3_chip nuked_chip;
#else
#include "opl.h"
extern "C" void OPL_Pico_simple(int16_t *buffer, uint32_t nsamples);
extern "C" void OPL_Pico_PortWrite(opl_port_t, unsigned int);

#include "cmd_buffers.h"
extern cms_buffer_t opl_buffer;
#endif

extern int16_t sbdsp_sample();

#include "clamp.h"

#ifdef USB_JOYSTICK
#include "tusb.h"
#endif

extern uint LED_PIN;

#ifdef USE_ALARM
#include "pico_pic.h"
#endif

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
    uint32_t start, end;

#ifdef USE_ALARM
    // Init PIC on this core so it handles timers
    PIC_Init();
#endif

#ifdef USB_JOYSTICK
    // Init TinyUSB for joystick support
    puts("tinyusb init");
    tuh_init(BOARD_TUH_RHPORT);
#endif

    clamp_setup();

#ifdef OPL_NUKED
    OPL3_Reset(&nuked_chip, 49716);
#endif

    struct audio_buffer_pool *ap = init_audio();

    puts("starting audio");
    for (;;) {
#ifndef OPL_NUKED
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
#endif
        struct audio_buffer *buffer = take_audio_buffer(ap, true);
        int16_t *samples = (int16_t *) buffer->buffer->bytes;
#ifdef OPL_NUKED
        int16_t opl_samples[2] = {0};
        OPL3_Generate(&nuked_chip, opl_samples);
        samples[0] = clamp16((int32_t)sbdsp_sample() + (int32_t)opl_samples[0]);
        samples[1] = clamp16((int32_t)sbdsp_sample() + (int32_t)opl_samples[1]);
#else
        int16_t opl_sample;
        OPL_Pico_simple(&opl_sample, 1);
        samples[0] = samples[1] = clamp16((int32_t)sbdsp_sample() + (int32_t)opl_sample);
#endif
        buffer->sample_count=1;
        // putchar((unsigned char)buffer->buffer->bytes[1]);
        give_audio_buffer(ap, buffer);
#ifdef USB_JOYSTICK
        // Service TinyUSB events
        tuh_task();
#endif
    }
}
