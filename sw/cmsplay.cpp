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
#include "pico/flash.h"

#ifdef USB_STACK
#include "tusb.h"
#endif
#if (defined(USB_MOUSE) || defined(SOUND_MPU)) && defined(USE_ALARM)
#include "pico_pic.h"
#endif

#ifdef USB_MOUSE
#include "mouse/8250uart.h"
#include "mouse/sermouse.h"
#endif

#ifdef MAME_CMS
#include "saa1099/saa1099.h"
saa1099_device *saa0, *saa1;
#else
#include "square/square.h"
#endif
#include "cmd_buffers.h"
extern cms_buffer_t cms_buffer;

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

#define SAMPLES_PER_BUFFER 4

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

void play_cms() {
    puts("starting core 1 CMS");
    flash_safe_execute_core_init();

#if (defined(USB_MOUSE) || defined(SOUND_MPU)) && defined(USE_ALARM)
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
#ifdef MAME_CMS
    puts("Creating SAA1099 1");
    saa0 = new saa1099_device(7159090);
    puts("Creating SAA1099 2");
    saa1 = new saa1099_device(7159090);
    // saa0->device_start();
    // saa1->device_start();
    int16_t buf0[SAMPLES_PER_BUFFER * 2];
    int16_t buf1[SAMPLES_PER_BUFFER * 2];
#else
    cms_t cms;
#ifdef SQUARE_FLOAT_OUTPUT
    float buf[SAMPLES_PER_BUFFER * 2];
#else
    int32_t buf[SAMPLES_PER_BUFFER * 2];
#endif
#endif // MAME_CMS
    struct audio_buffer_pool *ap = init_audio();
    for (;;) {
        // putchar('.');
        bool notfirst = false;
        while (cms_buffer.tail != cms_buffer.head) {
            if (!notfirst) {
                gpio_xor_mask(LED_PIN);
                notfirst = true;
            }
            auto cmd = cms_buffer.cmds[cms_buffer.tail];
#ifdef MAME_CMS
            // printf("%x ", cmd.addr & 0x3);
            switch (cmd.addr & 0x3) {
            case 0x0:
                saa0->data_w(cmd.data);
                break;
            case 0x1:
                saa0->control_w(cmd.data);
                break;
            case 0x2:
                saa1->data_w(cmd.data);
                break;
            case 0x3:
                saa1->control_w(cmd.data);
                break;
            }
#else // MAME_CMS
            if (cmd.addr & 1) {
                cms.write_addr(cmd.addr, cmd.data);
            } else {
                cms.write_data(cmd.addr, cmd.data);
            }
#endif // MAME_CMS
            ++cms_buffer.tail;
        }
        struct audio_buffer *buffer = take_audio_buffer(ap, true);
        int16_t *samples = (int16_t *) buffer->buffer->bytes;

#ifdef MAME_CMS    
        saa0->sound_stream_update(buf0, SAMPLES_PER_BUFFER);
        saa1->sound_stream_update(buf1, SAMPLES_PER_BUFFER);
#else
        memset(buf, 0, sizeof(buf));
        // uint32_t cms_audio_begin = time_us_32();
#ifdef SQUARE_FLOAT_OUTPUT
        cms.generator(0).generate_frames(buf, SAMPLES_PER_BUFFER, 1.0f);
        cms.generator(1).generate_frames(buf, SAMPLES_PER_BUFFER, 1.0f);
#else
        cms.generator(0).generate_frames(buf, SAMPLES_PER_BUFFER);
        cms.generator(1).generate_frames(buf, SAMPLES_PER_BUFFER);
#endif // SQUARE_FLOAT_OUTPUT
#endif // MAME_CMS
        for(uint32_t i = 0; i < SAMPLES_PER_BUFFER; ++i) {
#ifdef MAME_CMS
            samples[i << 1] = ((int32_t)buf0[i << 1] + (int32_t)buf1[i << 1]) >> 1;
            samples[(i << 1) + 1] = ((int32_t)buf0[(i << 1) + 1] + (int32_t)buf1[(i << 1) + 1]) >> 1;
#else
#ifdef SQUARE_FLOAT_OUTPUT
            samples[i << 1] = (int32_t)(buf[i << 1] * 32768.0f) >> 1;
            samples[(i << 1) + 1] = (int32_t)(buf[(i << 1) + 1] * 32768.0f) >> 1;
#else
            samples[i << 1] = buf[i << 1] >> 1;
            samples[(i << 1) + 1] = buf[(i << 1) + 1] >> 1;
#endif //SQUARE_FLOAT_OUTPUT
#endif // MAME_CMS
        }
        // uint32_t cms_audio_elapsed = time_us_32() - cms_audio_begin;
        // printf("%u ", cms_audio_elapsed);
        buffer->sample_count = buffer->max_sample_count;

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
        send_midi_bytes(4);
#endif
    }
}
