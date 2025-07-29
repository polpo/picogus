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
#pragma once

#include <pico/audio_i2s.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Alternative setup for pico_extras' audio_i2s library that does not set up DMA.
 * This allows for a minimal set up audio to be sent to the PIO sample by sample,
 * enabling a simpler interface with a sample clock IRQ, other buffering arrangement, etc.
 */
void audio_i2s_minimal_setup(const audio_i2s_config_t *config, uint32_t sample_rate);

#ifdef __cplusplus
} // extern "C"
#endif
