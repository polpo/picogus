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
#include "hardware/pwm.h"
#include "hardware/pio.h"

#endif

#include "pico/stdlib.h"
#include "audio_i2s_minimal.h"

#include "opl.h"
extern "C" void OPL_Pico_simple(int16_t *buffer, uint32_t nsamples);
extern "C" void OPL_Pico_WriteRegister(unsigned int reg_num, unsigned int value);

#include "audio_fifo.h"
#if SOUND_SB
extern int16_t sbdsp_sample();
#endif // SOUND_SB
#if defined(SOUND_SB) || defined(USB_MOUSE) || defined(SOUND_MPU)
#include "pico_pic.h"
#endif

#if CDROM
#include "cdrom/cdrom.h"
#endif // CDROM

#include "clamp.h"

#include "cmd_buffers.h"
extern cms_buffer_t opl_cmd_buffer;

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

#define audio_pio __CONCAT(pio, PICO_AUDIO_I2S_PIO)

static void init_audio(void) {
    const struct audio_i2s_config config = {
            .data_pin = PICO_AUDIO_I2S_DATA_PIN,
            .clock_pin_base = PICO_AUDIO_I2S_CLOCK_PIN_BASE,
            .dma_channel = 6,
            .pio_sm = PICO_AUDIO_I2S_SM,
    };
    audio_i2s_minimal_setup(&config, 44100);
}

/* Fixed-point format: Q16.16 (16 bits integer, 16 bits fractional) */
static constexpr uint32_t FRAC_BITS = 16;
static constexpr uint32_t FRAC_MASK = (1u << FRAC_BITS) - 1;
static constexpr uint32_t fixed_ratio(uint16_t a, uint16_t b) {
    return ((uint32_t)a << FRAC_BITS) / b;
}

static constexpr uint32_t opl_ratio = fixed_ratio(49716, 44100);
static audio_fifo_t opl_out_fifo;
static constexpr uint32_t OPL_SAMPLE_COUNT = 2;
static constexpr uint32_t OPL_BUFFER_BITS = (OPL_SAMPLE_COUNT << 1) - 1;
static constexpr uint32_t OPL_OUT_SAMPLE_COUNT = 1;//(OPL_SAMPLE_COUNT * 44100 / 49716);// - 1;
static int16_t opl_buffer[OPL_SAMPLE_COUNT << 1] = {0};
uint32_t opl_frac = 0;

/**
 * Linear interpolation in fixed-point
 * v0 and v1 are sample values, frac is the fractional position
 */
static inline int16_t lerp_fixed(int16_t v0, int16_t v1, uint32_t frac) {
    return v0 + (int16_t)(((int32_t)(v1 - v0) * frac) >> FRAC_BITS);
}

// Setup values for audio sample clock
// 8390 clock cycles per sample (370MHz / 8390 ~= 44100Hz)
static constexpr uint32_t clocks_per_sample_minus_one = (SYS_CLK_HZ / 44100) - 1;
static constexpr uint pwm_slice_num = 4; // slices 0-3 are taken by USB joystick support

typedef union {
    uint32_t data32;
    int16_t data16[2];
} sample_pair;

#ifdef CDROM
static audio_fifo_t* cd_fifo;
#endif

void audio_sample_handler(void) {
    pwm_clear_irq(pwm_slice_num);

    int32_t sample_l = 0, sample_r = 0;

#ifdef SOUND_SB
    sample_l = sample_r = sbdsp_sample();
#endif

#ifdef CDROM
    static uint32_t cd_index = 0;
    const uint32_t has_cd_samples = fifo_take_samples(cd_fifo, 2);
    if (has_cd_samples) {
        sample_l += cd_fifo->buffer[(cd_index++) & AUDIO_FIFO_BITS];
        sample_r += cd_fifo->buffer[(cd_index++) & AUDIO_FIFO_BITS];
    }
#endif

    static uint32_t opl_out_index = 0;
    const uint32_t has_opl_samples = fifo_take_samples(&opl_out_fifo, 1);
    if (has_opl_samples) {
        uint32_t idx = opl_out_index & AUDIO_FIFO_BITS;
        sample_l += opl_out_fifo.buffer[idx];
        sample_r += opl_out_fifo.buffer[idx];
        ++opl_out_index;
    }

    const sample_pair clamped = {.data16 = {
        clamp16(sample_l),
        clamp16(sample_r)
    }};
    audio_pio->txf[PICO_AUDIO_I2S_SM] = clamped.data32;
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

    init_audio();

    printf("opl_ratio: %x ", opl_ratio);
    uint32_t opl_pos = 0;
    uint32_t opl_index = 0;
    uint32_t opl_buffer_idx = 0x0;

    cd_fifo = cdrom_audio_fifo_peek(&cdrom);

    // Use the PWM peripheral to trigger an IRQ at 44100Hz
    pwm_config pwm_c = pwm_get_default_config();
    pwm_config_set_wrap(&pwm_c, clocks_per_sample_minus_one);
    pwm_init(pwm_slice_num, &pwm_c, false);
    pwm_set_irq_enabled(pwm_slice_num, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, audio_sample_handler);
    irq_set_enabled(PWM_IRQ_WRAP, true);
    irq_set_priority(PWM_IRQ_WRAP, 255);
    pwm_set_enabled(pwm_slice_num, true);

    for (;;) {
#if CDROM
        cdrom_audio_callback(&cdrom, 1024);
#endif
        uint32_t render_opl_samples = MIN(fifo_free_space(&opl_out_fifo), OPL_OUT_SAMPLE_COUNT);
        if (fifo_free_space(&opl_out_fifo) >= render_opl_samples) {
            if (
                ((opl_index & OPL_SAMPLE_COUNT) && opl_buffer_idx != 0x0) ||
                (!(opl_index & OPL_SAMPLE_COUNT) && opl_buffer_idx != OPL_SAMPLE_COUNT)
            ) {
                opl_buffer_idx = (opl_index + OPL_SAMPLE_COUNT) & OPL_SAMPLE_COUNT;
                // bool notfirst = false;
                while (opl_cmd_buffer.tail != opl_cmd_buffer.head) {
                    // if (!notfirst) {
#ifndef PICOW
                    //     gpio_xor_mask(LED_PIN);
#endif
                    //     notfirst = true;
                    // }
                    auto cmd = opl_cmd_buffer.cmds[opl_cmd_buffer.tail];
                    OPL_Pico_WriteRegister(cmd.addr, cmd.data);
                    ++opl_cmd_buffer.tail;
                }
                OPL_Pico_simple(opl_buffer + opl_buffer_idx, OPL_SAMPLE_COUNT);
            }
            for (int i = 0; i < render_opl_samples; ++i) {
                opl_index = (opl_pos >> FRAC_BITS);
                fifo_add_sample(&opl_out_fifo, lerp_fixed(
                    opl_buffer[opl_index & OPL_BUFFER_BITS],
                    opl_buffer[(opl_index + 1) & OPL_BUFFER_BITS],
                    opl_pos & FRAC_MASK)
                );
                opl_pos += opl_ratio;
            }
        }
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
