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

// Stereo OPL resampler state.
// For ymfm backends (USE_YMFM_OPL / USE_YMF3812), OPL_Pico_stereo provides
// true stereo (or symmetric mono for ym3812) from a single generate() call.
// For emu8950 (pg-adlib), OPL_Pico_stereo doesn't exist — fall back to
// OPL_Pico_simple and duplicate to both channels.
static int32_t opl_resamp_l = 0, opl_resamp_r = 0;
static int32_t opl_resamp_buf_l[2] = {0}, opl_resamp_buf_r[2] = {0};
static uint32_t opl_resamp_phase = 0; // Q16.16 phase accumulator

static void opl_resample_tick()
{
    opl_resamp_phase += opl_ratio;
    while (opl_resamp_phase >= (1u << 16))
    {
        opl_resamp_buf_l[1] = opl_resamp_buf_l[0];
        opl_resamp_buf_r[1] = opl_resamp_buf_r[0];
#if defined(USE_YMFM_OPL) || defined(USE_DBOPL_OPL) || defined(USE_YMF3812)
        int32_t l, r;
        OPL_Pico_stereo(&l, &r, 1);
        opl_resamp_buf_l[0] = scale_sample(l, opl_volume, 1);
        opl_resamp_buf_r[0] = scale_sample(r, opl_volume, 1);
#else
        int32_t mono;
        OPL_Pico_simple(&mono, 1);
        opl_resamp_buf_l[0] = opl_resamp_buf_r[0] = scale_sample(mono, opl_volume, 1);
#endif
        opl_resamp_phase -= (1u << 16);
    }
    {
        // linear resampler - TODO adapt polyphase code from resampler/resampler.hpp
        int32_t phase_frac = opl_resamp_phase & ((1u << 16) - 1);
        opl_resamp_l = ((opl_resamp_buf_l[0] * phase_frac) + (opl_resamp_buf_l[1] * ((1 << 16) - phase_frac))) >> 16;
        opl_resamp_r = ((opl_resamp_buf_r[0] * phase_frac) + (opl_resamp_buf_r[1] * ((1 << 16) - phase_frac))) >> 16;
    }
    
}

// Setup values for audio sample clock
// 8390 clock cycles per sample (370MHz / 8390 ~= 44100Hz)
static constexpr uint32_t clocks_per_sample_minus_one = (SYS_CLK_HZ / 44100) - 1;
static constexpr uint pwm_slice_num = 4; // slices 0-3 are taken by USB joystick support

// Max OPL samples to generate per main-loop iteration.
// Keeps cdrom_tasks() running frequently so the USB drive is never starved.
// 256 samples ~= 5.8 ms at 44100 Hz — matches one USB audio buffer period.
static constexpr uint32_t OPL_FILL_PER_ITER = 256;

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
    // Read L+R sample pair directly from FIFO using read_idx before advancing.
    // Must request exactly 2 (stereo pair) or skip — never read partial pairs.
    if (cd_fifo->state != FIFO_STATE_STOPPED && cd_fifo->write_idx - cd_fifo->read_idx >= 2) {
        uint32_t idx = cd_fifo->read_idx;
        sample_l += scale_sample(cd_fifo->buffer[idx & AUDIO_FIFO_BITS],              cd_audio_volume, 0);
        sample_r += scale_sample(cd_fifo->buffer[(idx + 1) & AUDIO_FIFO_BITS], cd_audio_volume, 0);
        cd_fifo->read_idx = idx + 2;
        if (cd_fifo->write_idx == cd_fifo->read_idx) cd_fifo->state = FIFO_STATE_STOPPED;
    }
#endif

    // Read OPL stereo pair from FIFO — always consume 2 samples (L then R).
    if (opl_out_fifo.state != FIFO_STATE_STOPPED && opl_out_fifo.write_idx - opl_out_fifo.read_idx >= 2) {
        uint32_t idx = opl_out_fifo.read_idx;
        sample_l += opl_out_fifo.buffer[idx & AUDIO_FIFO_BITS];
        sample_r += opl_out_fifo.buffer[(idx + 1) & AUDIO_FIFO_BITS];
        opl_out_fifo.read_idx = idx + 2;
        if (opl_out_fifo.write_idx == opl_out_fifo.read_idx) opl_out_fifo.state = FIFO_STATE_STOPPED;
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

	// opl_resamp_phase starts at 0; no explicit init needed.

#ifdef SOUND_SB
    set_volume(CMD_SBVOL);
#endif

    printf("OPL stereo pipeline started\n");

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
        cdrom_audio_callback(&cdrom, AUDIO_FIFO_SIZE - SAMPLES_PER_SECTOR);
#endif

#if 0
#if OPL_CMD_BUFFER
        // Process any pending OPL commands
        while (opl_cmd_buffer.tail != opl_cmd_buffer.head) {
            auto cmd = opl_cmd_buffer.cmds[opl_cmd_buffer.tail];
            OPL_Pico_WriteRegister(cmd.addr, cmd.data);
            ++opl_cmd_buffer.tail;
        }
#endif
#endif

        // Generate OPL stereo pairs and add to output FIFO (interleaved L, R).
        // Need 2 free slots per output sample.
        for (uint32_t opl_i = 0;
             opl_i < OPL_FILL_PER_ITER && fifo_free_space(&opl_out_fifo) >= 2;
             opl_i++) {
            opl_resample_tick();
            fifo_add_sample(&opl_out_fifo, (int16_t)opl_resamp_l);
            fifo_add_sample(&opl_out_fifo, (int16_t)opl_resamp_r);
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
