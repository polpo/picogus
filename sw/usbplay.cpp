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

#include "include/pg_debug.h"

#include "tusb.h"
#include "mouse/8250uart.h"
#include "mouse/sermouse.h"

#ifdef CDROM
#include "hardware/pwm.h"
#include "hardware/pio.h"
#include "cdrom/cdrom.h"
#include "audio/audio_i2s_minimal.h"
#include "audio/audio_fifo.h"
#include "audio/volctrl.h"
#include "audio/clamp.h"
#endif

#ifdef SOUND_MPU
#include "system/flash_settings.h"
extern Settings settings;
#include "mpu401/export.h"
#endif

#include "system/pico_pic.h"

#ifdef CDROM
#define audio_pio __CONCAT(pio, PICO_AUDIO_I2S_PIO)

static void init_audio(void) {
    const audio_i2s_config_t config = {
            .data_pin = PICO_AUDIO_I2S_DATA_PIN,
            .clock_pin_base = PICO_AUDIO_I2S_CLOCK_PIN_BASE,
            .dma_channel = 6,
            .pio_sm = PICO_AUDIO_I2S_SM,
    };
    audio_i2s_minimal_setup(&config, 44100);
}

static audio_fifo_t* cd_fifo;

// Setup values for audio sample clock. (RP2_CLOCK_SPEED / 44100) clocks per sample.
static constexpr uint32_t clocks_per_sample_minus_one = (RP2_CLOCK_SPEED * 1000u / 44100) - 1;
static constexpr uint pwm_slice_num = 4; // slices 0-3 are taken by USB joystick support

void audio_sample_handler(void) {
    pwm_clear_irq(pwm_slice_num);

    int32_t sample_l = 0, sample_r = 0;
    if (cd_fifo->state != FIFO_STATE_STOPPED && cd_fifo->write_idx - cd_fifo->read_idx >= 1) {
        sample_pair cd = cd_fifo->buffer[cd_fifo->read_idx & AUDIO_FIFO_BITS];
        cd_fifo->read_idx++;
        if (cd_fifo->write_idx == cd_fifo->read_idx) cd_fifo->state = FIFO_STATE_STOPPED;
        sample_l = scale_sample(cd.data16[0], volume.cd_audio[0], 0);
        sample_r = scale_sample(cd.data16[1], volume.cd_audio[1], 0);
    }
    sample_l = clamp16(sample_l);
    sample_r = clamp16(sample_r);

    audio_pio->txf[PICO_AUDIO_I2S_SM] = ((sample_l & 0xFFFF) | (sample_r << 16));
}
#endif // CDROM

void play_usb() {
    DBG_PUTS("starting core 1 USB");

    // Init PIC on this core so it handles timers
    PIC_Init();
    DBG_PUTS("pic inited on core 1");

    // init host stack on configured roothub port
    tuh_init(BOARD_TUH_RHPORT);
    // Pump USB events for ~500 ms so any device already connected at boot
    // is fully enumerated before the audio loop starts.
    {
        uint32_t deadline = time_us_32() + 500000u;
        while (time_us_32() < deadline) tuh_task();
    }

#ifdef SOUND_MPU
    MPU401_Init(settings.MPU.delaySysex, settings.MPU.fakeAllNotesOff);
#endif

#ifdef CDROM
    init_audio();
    cd_fifo = cdrom_audio_fifo_peek(&cdrom);
    set_volume(CMD_CDVOL);

    // Use the PWM peripheral to trigger an IRQ at 44100Hz
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
#ifdef CDROM
        // Fill the CD audio FIFO from the active track; ISR drains it.
        cdrom_audio_callback(&cdrom, AUDIO_FIFO_SIZE - STEREO_SAMPLES_PER_SECTOR);
        cdrom_tasks(&cdrom);
#endif
#ifdef SOUND_MPU
        send_midi_bytes(8);
#endif
        tuh_task();
        sermouse_core1_task();
        uartemu_core1_task();
    }
}
