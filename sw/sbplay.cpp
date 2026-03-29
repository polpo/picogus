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
#ifdef SOUND_WSS
#include "ad1848/ad1848.h"
#else
#include "sbdsp/sbdsp.h"
#endif
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

#if (defined(SOUND_MPU) || defined(SOUND_SB))
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

// OPL FIFO — smaller than CD audio FIFO since producer and consumer are on
// the same core. 256 stereo pairs ~= 5.8ms at 44100Hz.
#define OPL_FIFO_SIZE 256
#define OPL_FIFO_BITS (OPL_FIFO_SIZE - 1)
static struct {
    sample_pair buffer[OPL_FIFO_SIZE];
    volatile uint32_t write_idx;
    volatile uint32_t read_idx;
    volatile fifo_state_t state;
} opl_out_fifo;

static inline uint32_t opl_fifo_free_space() {
    return OPL_FIFO_SIZE - (opl_out_fifo.write_idx - opl_out_fifo.read_idx);
}

static inline void opl_fifo_add_sample(sample_pair sample) {
    opl_out_fifo.buffer[opl_out_fifo.write_idx & OPL_FIFO_BITS] = sample;
    opl_out_fifo.write_idx++;
}

// OPL stereo sample callback — returns a clamped stereo pair.
// Volume is applied in the ISR after FIFO read, not here.
static sample_pair get_opl_stereo_sample()
{
#if defined(USE_YMFM_OPL) || defined(USE_DBOPL_OPL) || defined(USE_YMF3812)
    int32_t l, r;
    OPL_Pico_stereo(&l, &r, 1);
    return (sample_pair){.data16 = {clamp16(l), clamp16(r)}};
#else
    int32_t mono;
    OPL_Pico_simple(&mono, 1);
    int16_t s = clamp16(mono);
    return (sample_pair){.data16 = {s, s}};
#endif
}

#ifdef USE_LINEAR_RESAMPLER
static constexpr uint32_t FRAC_BITS = 16;
static constexpr uint32_t fixed_ratio(uint16_t a, uint16_t b) {
    return ((uint32_t)a << FRAC_BITS) / b;
}
static constexpr uint32_t opl_ratio = fixed_ratio(49716, 44100);

static int32_t opl_resamp_buf_l[2] = {0}, opl_resamp_buf_r[2] = {0};
static uint32_t opl_resamp_phase = 0;

static sample_pair opl_resample_tick()
{
    opl_resamp_phase += opl_ratio;
    while (opl_resamp_phase >= (1u << 16))
    {
        opl_resamp_buf_l[1] = opl_resamp_buf_l[0];
        opl_resamp_buf_r[1] = opl_resamp_buf_r[0];
        sample_pair in = get_opl_stereo_sample();
        opl_resamp_buf_l[0] = in.data16[0];
        opl_resamp_buf_r[0] = in.data16[1];
        opl_resamp_phase -= (1u << 16);
    }
    int32_t phase_frac = opl_resamp_phase & ((1u << 16) - 1);
    sample_pair out;
    out.data16[0] = ((opl_resamp_buf_l[0] * phase_frac) + (opl_resamp_buf_l[1] * ((1 << 16) - phase_frac))) >> 16;
    out.data16[1] = ((opl_resamp_buf_r[0] * phase_frac) + (opl_resamp_buf_r[1] * ((1 << 16) - phase_frac))) >> 16;
    return out;
}
#else
static StereoResampler<get_opl_stereo_sample> opl_resampler;
#endif

// Setup values for audio sample clock
// 8390 clock cycles per sample (370MHz / 8390 ~= 44100Hz)
static constexpr uint32_t clocks_per_sample_minus_one = (SYS_CLK_HZ / 44100) - 1;
static constexpr uint pwm_slice_num = 4; // slices 0-3 are taken by USB joystick support

// Max OPL samples to generate per main-loop iteration.
// Keeps cdrom_tasks() running frequently so the USB drive is never starved.
// 256 samples ~= 5.8 ms at 44100 Hz — matches one USB audio buffer period.
static constexpr uint32_t OPL_FILL_PER_ITER = 256;


#ifdef CDROM
static audio_fifo_t* cd_fifo;
#endif

void audio_sample_handler(void) {
    pwm_clear_irq(pwm_slice_num);

    int32_t sample_l = 0, sample_r = 0;

#ifdef SOUND_SB
    {
#ifdef SOUND_WSS
        uint32_t card_stereo = ad1848_sample_stereo();
#else
        uint32_t card_stereo = sbdsp_sample_stereo();
#endif
        sample_l = scale_sample((int16_t)(card_stereo & 0xFFFF), volume.sb_pcm[0], 0);
        sample_r = scale_sample((int16_t)(card_stereo >> 16),    volume.sb_pcm[1], 0);
    }
#endif

#ifdef CDROM
    if (cd_fifo->state != FIFO_STATE_STOPPED && cd_fifo->write_idx - cd_fifo->read_idx >= 1) {
        sample_pair cd = cd_fifo->buffer[cd_fifo->read_idx & AUDIO_FIFO_BITS];
        cd_fifo->read_idx++;
        if (cd_fifo->write_idx == cd_fifo->read_idx) cd_fifo->state = FIFO_STATE_STOPPED;
        sample_l += scale_sample(cd.data16[0], volume.cd_audio[0], 0);
        sample_r += scale_sample(cd.data16[1], volume.cd_audio[1], 0);
    }
#endif

    if (opl_out_fifo.state != FIFO_STATE_STOPPED && opl_out_fifo.write_idx - opl_out_fifo.read_idx >= 1) {
        sample_pair opl = opl_out_fifo.buffer[opl_out_fifo.read_idx & OPL_FIFO_BITS];
        opl_out_fifo.read_idx++;
        if (opl_out_fifo.write_idx == opl_out_fifo.read_idx) opl_out_fifo.state = FIFO_STATE_STOPPED;
        sample_l += scale_sample(opl.data16[0], volume.opl[0], 0);
        sample_r += scale_sample(opl.data16[1], volume.opl[1], 0);
    }

#ifdef SOUND_SB
    // apply SB master volume and clamp the output
    sample_l = scale_sample(sample_l, volume.sb_master[0], 1);
    sample_r = scale_sample(sample_r, volume.sb_master[1], 1);
#else
    sample_l = clamp16(sample_l);
    sample_r = clamp16(sample_r);
#endif

    audio_pio->txf[PICO_AUDIO_I2S_SM] = ((sample_l & 0xFFFF) | (sample_r << 16));
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
#ifdef SOUND_WSS
    ad1848_init();
#else
    sbdsp_init();
    sbdsp_set_type(settings.SB16.sbType);
    sbdsp_set_irq(settings.SB16.irq);
    sbdsp_set_dma(settings.SB16.dma);
    sbdsp_set_options(settings.SB16.options);
#endif
#endif
    init_audio();

#ifdef USE_LINEAR_RESAMPLER
	// opl_resamp_phase starts at 0; no explicit init needed.
#else
	opl_resampler.set_ratio(49716, 44100);
#endif

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
        cdrom_audio_callback(&cdrom, AUDIO_FIFO_SIZE - STEREO_SAMPLES_PER_SECTOR);
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

        // Process DSP commands
        sbdsp_process();

        // Generate OPL stereo pairs and add to output FIFO.
        for (uint32_t opl_i = 0;
             opl_i < OPL_FILL_PER_ITER && opl_fifo_free_space() >= 1;
             opl_i++) {
#ifdef USE_LINEAR_RESAMPLER
            opl_fifo_add_sample(opl_resample_tick());
#else
            opl_fifo_add_sample(opl_resampler.get_sample());
#endif
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
