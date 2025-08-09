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
#include "audio/audio_i2s_minimal.h"
#include <resampler.hpp>
#include "audio/volctrl.h"

#include "opl.h"

#include "audio/audio_fifo.h"
#if SOUND_SB
#include "sbdsp/sbdsp.h"
#endif // SOUND_SB
#if defined(SOUND_SB) || defined(USB_MOUSE) || defined(SOUND_MPU)
#include "system/pico_pic.h"
#endif

#if CDROM
#include "cdrom/cdrom.h"
#endif // CDROM

#include "audio/clamp.h"

#if OPL_CMD_BUFFER
#include "include/cmd_buffers.h"
extern cms_buffer_t opl_cmd_buffer;
#endif

#ifdef USB_STACK
#include "tusb.h"
#endif
#ifdef USB_MOUSE
#include "mouse/8250uart.h"
#include "mouse/sermouse.h"
#endif

#ifdef SOUND_MPU
#include "system/flash_settings.h"
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

static int16_t get_opl_sample()
{
    int16_t opl_current_sample;
    OPL_Pico_simple(&opl_current_sample, 1);
    opl_current_sample = scale_sample(opl_current_sample << 1, opl_volume, 1);
    return opl_current_sample;
}

static Resampler<get_opl_sample> resampler;

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
    const uint32_t has_cd_samples = fifo_take_samples_inline(cd_fifo, 2);
    if (has_cd_samples) {
        sample_l += scale_sample(cd_fifo->buffer[cd_index++], cd_audio_volume, 0);
        sample_r += scale_sample(cd_fifo->buffer[cd_index++], cd_audio_volume, 0);
        // sample_l += cd_fifo->buffer[cd_index++];
        // sample_r += cd_fifo->buffer[cd_index++];
        cd_index &= AUDIO_FIFO_BITS;
    }
#endif

    static uint32_t opl_out_index = 0;
    const uint32_t has_opl_samples = fifo_take_samples_inline(&opl_out_fifo, 1);
    if (has_opl_samples) {
        int16_t opl_sample = opl_out_fifo.buffer[opl_out_index++];
        sample_l += opl_sample;
        sample_r += opl_sample;
        opl_out_index &= AUDIO_FIFO_BITS;
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
    set_volume(CMD_OPLVOL);

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

#if SOUND_SB
    sbdsp_init();
#endif
    init_audio();

	resampler.set_ratio(49716,44100);

#ifdef SOUND_SB
    set_volume(CMD_SBVOL);
#endif

    printf("opl_ratio: %x ", opl_ratio);
    uint32_t opl_pos = 0;

#ifdef CDROM
    cd_fifo = cdrom_audio_fifo_peek(&cdrom);
#endif

    // Use the PWM peripheral to trigger an IRQ at 44100Hz
#if !AUDIO_CALLBACK_CORE0
    pwm_config pwm_c = pwm_get_default_config();
    pwm_config_set_wrap(&pwm_c, clocks_per_sample_minus_one);
    pwm_init(pwm_slice_num, &pwm_c, false);
    pwm_set_irq_enabled(pwm_slice_num, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, audio_sample_handler);
    irq_set_priority(PWM_IRQ_WRAP, PICO_LOWEST_IRQ_PRIORITY);
    irq_set_enabled(PWM_IRQ_WRAP, true);
    pwm_set_enabled(pwm_slice_num, true);
#endif

    for (;;) {
#if CDROM
        cdrom_audio_callback(&cdrom, 1024);
#endif

#if OPL_CMD_BUFFER
        // Process any pending OPL commands
        while (opl_cmd_buffer.tail != opl_cmd_buffer.head) {
            auto cmd = opl_cmd_buffer.cmds[opl_cmd_buffer.tail];
            OPL_Pico_WriteRegister(cmd.addr, cmd.data);
            ++opl_cmd_buffer.tail;
        }
#endif

        // Generate OPL samples and add to output FIFO
        while (fifo_free_space(&opl_out_fifo) > 0) {
            // Interpolate at current position
            fifo_add_sample(&opl_out_fifo, resampler.get_sample());
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
