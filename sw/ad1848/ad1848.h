/*
 *  Copyright (C) 2024  Ian Scott
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

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void ad1848_init();
void ad1848_write(uint8_t port, uint8_t data);
uint8_t ad1848_read(uint8_t port);

// Returns packed stereo pair: L in low 16 bits, R in high 16 bits.
// Naturally atomic on Cortex-M0+ (32-bit aligned).
uint32_t ad1848_sample_stereo();

#ifdef __cplusplus
}
#endif
