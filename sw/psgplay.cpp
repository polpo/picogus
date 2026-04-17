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

#if PICO_ON_DEVICE
#include "hardware/clocks.h"
#include "hardware/structs/clocks.h"
#include "hardware/pwm.h"
#include "hardware/pio.h"
#endif

#include "pico/stdlib.h"
#include "audio/audio_i2s_minimal.h"
#include "audio/volctrl.h"
#include "audio/clamp.h"

#include "square/square.h"

#include "include/cmd_buffers.h"

#if SOUND_TANDY
extern tandy_buffer_t tandy_buffer;
#endif
#if SOUND_CMS
extern cms_buffer_t cms_buffer;
#endif

extern uint LED_PIN;

#ifdef USB_STACK
#include "tusb.h"
#endif
#if defined(USB_MOUSE) || defined(SOUND_MPU)
#include "system/pico_pic.h"
#endif

#ifdef USB_MOUSE
#include "mouse/8250uart.h"
#include "mouse/sermouse.h"
#endif

#if PICO_ON_DEVICE
#include "pico/binary_info.h"
bi_decl(bi_3pins_with_names(PICO_AUDIO_I2S_DATA_PIN, "I2S DIN", PICO_AUDIO_I2S_CLOCK_PIN_BASE, "I2S BCK", PICO_AUDIO_I2S_CLOCK_PIN_BASE+1, "I2S LRCK"));
#endif

#ifdef SOUND_MPU
#include "system/flash_settings.h"
extern Settings settings;
#include "mpu401/export.h"
#endif

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

#if SOUND_TANDY
static tandysound_t tandysound;
#endif
#if SOUND_CMS
static cms_t cms;
#endif

// Setup values for audio sample clock
static constexpr uint32_t clocks_per_sample_minus_one = (RP2_CLOCK_SPEED * 1000u / 44100) - 1;
static constexpr uint pwm_slice_num = 4; // slices 0-3 are taken by USB joystick support

// PSG generation is cheap (phase accumulators + volume lookups per voice), so
// synthesize directly in the ISR — no intermediate FIFO needed.
void audio_sample_handler(void) {
    pwm_clear_irq(pwm_slice_num);

    int32_t buf[2] = {0, 0};
#if SOUND_TANDY
    tandysound.generator().generate_frames(buf, 1);
#endif
#if SOUND_CMS
    cms.generator(0).generate_frames(buf, 1);
    cms.generator(1).generate_frames(buf, 1);
#endif
    int32_t sample_l = scale_sample(clamp16(buf[0]), volume.psg, 0);
    int32_t sample_r = scale_sample(clamp16(buf[1]), volume.psg, 0);
    sample_l = clamp16(sample_l);
    sample_r = clamp16(sample_r);

    audio_pio->txf[PICO_AUDIO_I2S_SM] = ((sample_l & 0xFFFF) | (sample_r << 16));
}

void play_psg() {
    DBG_PUTS("starting core 1 psg");
    // flash_safe_execute_core_init();

#if defined(USB_MOUSE) || defined(SOUND_MPU)
    // Init PIC on this core so it handles timers
    PIC_Init();
    DBG_PUTS("pic inited on core 1");
#endif
#ifdef USB_STACK
    // Init TinyUSB for joystick support
    tuh_init(BOARD_TUH_RHPORT);
    // Pump USB events for ~500 ms so any device already connected at boot
    // is fully enumerated before the audio loop starts.
    {
        uint32_t deadline = time_us_32() + 500000u;
        while (time_us_32() < deadline) tuh_task();
    }
#endif

#ifdef SOUND_MPU
    MPU401_Init(settings.MPU.delaySysex, settings.MPU.fakeAllNotesOff);
#endif

    init_audio();
    set_volume(CMD_PSGVOL);

    // Use the PWM peripheral to trigger an IRQ at 44100Hz
    pwm_config pwm_c = pwm_get_default_config();
    pwm_config_set_wrap(&pwm_c, clocks_per_sample_minus_one);
    pwm_init(pwm_slice_num, &pwm_c, false);
    pwm_set_irq_enabled(pwm_slice_num, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, audio_sample_handler);
    irq_set_priority(PWM_IRQ_WRAP, PICO_LOWEST_IRQ_PRIORITY);
    irq_set_enabled(PWM_IRQ_WRAP, true);
    pwm_set_enabled(pwm_slice_num, true);

    for (;;) {
        bool notfirst = false;
#if SOUND_TANDY
        while (tandy_buffer.tail != tandy_buffer.head) {
            if (!notfirst) {
                gpio_xor_mask(LED_PIN);
                notfirst = true;
            }
            tandysound.write_register(0, tandy_buffer.cmds[tandy_buffer.tail]);
            ++tandy_buffer.tail;
        }
#endif // SOUND_TANDY
#if SOUND_CMS
        while (cms_buffer.tail != cms_buffer.head) {
            if (!notfirst) {
                gpio_xor_mask(LED_PIN);
                notfirst = true;
            }
            auto cmd = cms_buffer.cmds[cms_buffer.tail];
            if (cmd.addr & 1) {
                cms.write_addr(cmd.addr, cmd.data);
            } else {
                cms.write_data(cmd.addr, cmd.data);
            }
            ++cms_buffer.tail;
        }
#endif

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
    }
}
