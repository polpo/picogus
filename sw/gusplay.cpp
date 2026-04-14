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
// #include "stdio_async_uart.h"
#include <math.h>

#if PICO_ON_DEVICE
#include "hardware/clocks.h"
#include "hardware/structs/clocks.h"
#include "hardware/pwm.h"
#include "hardware/pio.h"
#endif

#include "pico/stdlib.h"
#include "audio/audio_i2s_minimal.h"

#include "system/pico_pic.h"

#ifdef USB_STACK
#include "tusb.h"
#endif

#ifdef PSRAM
#include "psram_spi.h"
extern psram_spi_inst_t psram_spi;
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

#include "isa/isa_dma.h"
extern irq_handler_t GUS_DMA_isr_pt;
extern dma_inst_t dma_config;

#include "gus/gus-x.h"

#define DMA_PIO_SM 2

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

static constexpr uint pwm_slice_num = 4; // slices 0-3 are taken by USB joystick support
// SYS_CLK / GUS_CLOCK (19.7568MHz) reduced by GCD of 3200: 115625 / 6174
static constexpr uint32_t CLK_RATIO_NUM = RP2_CLOCK_SPEED * 1000u / 3200; // 115625
static constexpr uint32_t CLK_RATIO_DEN = 19756800u / 3200;               // 6174
static uint8_t current_gus_channels = 14;

void audio_sample_handler(void) {
    pwm_clear_irq(pwm_slice_num);

    uint32_t sample = GUS_sample_stereo();
    audio_pio->txf[PICO_AUDIO_I2S_SM] = sample;
}

// Clocks per GUS sample = round(SYS_CLK * 32 * channels / GUS_CLOCK)
static inline uint32_t gus_clocks_per_sample(uint8_t channels) {
    return (CLK_RATIO_NUM * 32u * channels + CLK_RATIO_DEN / 2) / CLK_RATIO_DEN;
}

// Update PWM IRQ rate and I2S PIO clock divider for new GUS channel count
// GUS sample rate = GUS_CLOCK / (32 * channels)
static void update_gus_timing(uint8_t channels) {
    current_gus_channels = channels;

    uint32_t cps = gus_clocks_per_sample(channels);
    pwm_set_wrap(pwm_slice_num, cps - 1);
    pwm_hw->slice[pwm_slice_num].ctr = 0; // Reset counter to take effect immediately

    // I2S PIO divider (16.8 fixed point) = 4x clocks per sample
    uint32_t divider = cps * 4;
    pio_sm_set_clkdiv_int_frac(audio_pio, PICO_AUDIO_I2S_SM, divider >> 8u, divider & 0xffu);

    DBG_PRINTF("GUS channels: %u\n", channels);
}

// void __xip_cache("my_sub_section") (play_gus)(void) {
void play_gus() {
    DBG_PUTS("starting core 1");
    // flash_safe_execute_core_init();
    uint32_t start, end;

    // Init PIC on this core so it handles timers
    PIC_Init();

    // Init ISA DMA on this core so it handles the ISR
    DBG_PUTS("Initing ISA DMA PIO...");
    dma_config = DMA_init(pio0, DMA_PIO_SM, GUS_DMA_isr_pt);

#ifdef PSRAM_CORE1
#ifdef PSRAM
    DBG_PUTS("Initing PSRAM...");
    psram_spi = psram_spi_init_clkdiv(pio1, -1, 1.6);
#if TEST_PSRAM
    DBG_PUTS("Writing PSRAM...");
    uint8_t deadbeef[8] = {0xd, 0xe, 0xa, 0xd, 0xb, 0xe, 0xe, 0xf};
    for (uint32_t addr = 0; addr < (1024 * 1024); ++addr) {
        psram_write8_async(&psram_spi, addr, (addr & 0xFF));
    }
    DBG_PUTS("Reading PSRAM...");
    uint32_t psram_begin = time_us_32();
    for (uint32_t addr = 0; addr < (1024 * 1024); ++addr) {
        uint8_t result = psram_read8(&psram_spi, addr);
        if (static_cast<uint8_t>((addr & 0xFF)) != result) {
            DBG_PRINTF("\nPSRAM failure at address %x (%x != %x)\n", addr, addr & 0xFF, result);
            return;
        }
    }
    uint32_t psram_elapsed = time_us_32() - psram_begin;
    float psram_speed = 1000000.0 * 1024.0 * 1024 / psram_elapsed;
    DBG_PRINTF("8 bit: PSRAM read 1MB in %d us, %d B/s (target 705600 B/s)\n", psram_elapsed, (uint32_t)psram_speed);
#endif
#endif
#endif
    GUS_Setup();

#ifdef USB_STACK
    // Init TinyUSB for joystick support
    tuh_init(BOARD_TUH_RHPORT);
#endif

#ifdef SOUND_MPU
    MPU401_Init(settings.MPU.delaySysex, settings.MPU.fakeAllNotesOff);
#endif

    init_audio();

    DBG_PRINTF("GUS IRQ-driven audio started\n");

    // Use the PWM peripheral to trigger an IRQ at the GUS sample rate
    pwm_config pwm_c = pwm_get_default_config();
    pwm_config_set_wrap(&pwm_c, gus_clocks_per_sample(current_gus_channels) - 1);
    pwm_init(pwm_slice_num, &pwm_c, false);
    pwm_set_irq_enabled(pwm_slice_num, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, audio_sample_handler);
    irq_set_priority(PWM_IRQ_WRAP, PICO_LOWEST_IRQ_PRIORITY);
    irq_set_enabled(PWM_IRQ_WRAP, true);
    pwm_set_enabled(pwm_slice_num, true);

    for (;;) {
        // Check if GUS channel count changed
        uint8_t channels = GUS_timingChannels();
        if (channels != current_gus_channels) {
            update_gus_timing(channels);
        }
#ifdef USB_STACK
        // Service TinyUSB events
        tuh_task();
#endif
#ifdef SOUND_MPU
        send_midi_bytes(8);
#endif
    }
}
