/*
 *  Copyright (C) 2025  Ian Scott
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
#include <hardware/clocks.h>
#include <audio_i2s.pio.h>
#include "audio_i2s_minimal.h"

#define audio_pio __CONCAT(pio, PICO_AUDIO_I2S_PIO)
#define GPIO_FUNC_PIOx __CONCAT(GPIO_FUNC_PIO, PICO_AUDIO_I2S_PIO)

void audio_i2s_minimal_setup(const audio_i2s_config_t *config, uint32_t sample_rate) {
    uint func = GPIO_FUNC_PIOx;
    gpio_set_function(config->data_pin, func);
    gpio_set_function(config->clock_pin_base, func);
    gpio_set_function(config->clock_pin_base + 1, func);

#if PICO_PIO_USE_GPIO_BASE
    if(config->data_pin >= 32 || config->clock_pin_base + 1 >= 32) {
        pio_set_gpio_base(audio_pio, 16);
    }
#endif
    uint8_t sm = config->pio_sm;
    pio_sm_claim(audio_pio, sm);

    const struct pio_program *program =
#if PICO_AUDIO_I2S_CLOCK_PINS_SWAPPED
        &audio_i2s_swapped_program
#else
        &audio_i2s_program
#endif
        ;
    uint offset = pio_add_program(audio_pio, program);

    audio_i2s_program_init(audio_pio, sm, offset, config->data_pin, config->clock_pin_base);

    uint32_t system_clock_frequency = clock_get_hz(clk_sys);
    uint32_t divider = system_clock_frequency * 4 / sample_rate; // avoid arithmetic overflow
    pio_sm_set_clkdiv_int_frac(audio_pio, sm, divider >> 8u, divider & 0xffu);
    pio_sm_set_enabled(audio_pio, sm, true);
}
