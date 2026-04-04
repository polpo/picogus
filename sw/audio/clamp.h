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
#pragma once

#if defined(INTERP_CLAMP) || defined(INTERP_VOLCTRL)
#include "hardware/interp.h"
#endif

// Configure interp1 for clamp mode — the only interpolator that supports it on RP2040.
// Input is shifted right by 'shift', masked to bits 0..mask_msb,
// sign-extended, and clamped to [-32768, 32767].
static void clamp_setup(uint8_t shift, uint8_t mask_msb) {
#if defined(INTERP_CLAMP) || defined(INTERP_VOLCTRL)
    interp_config cfg;
    cfg = interp_default_config();
    interp_config_set_clamp(&cfg, true);
    interp_config_set_shift(&cfg, shift);
    interp_config_set_mask(&cfg, 0, mask_msb);
    interp_config_set_signed(&cfg, true);
    interp_set_config(interp1, 0, &cfg);
    interp1->base[0] = -32768;
    interp1->base[1] = 32767;
#endif
}

#if defined(INTERP_CLAMP) || defined(INTERP_VOLCTRL)
static int16_t __force_inline clamp16_interp(int32_t d) {
    interp1->accum[0] = d;
    return interp1->peek[0];
}
#endif

static int16_t __force_inline clamp16(int32_t d) {
#ifdef INTERP_CLAMP
    return clamp16_interp(d);
#else
    // optimized C clamp16: single branch check
    if (__builtin_expect((uint32_t)(d + 32768) >> 16, 0)) {
        return d < 0 ? (int16_t)-32768 : (int16_t)32767;
    }
    return (int16_t)d;
#endif // INTERP_CLAMP
}
