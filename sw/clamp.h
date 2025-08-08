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

#ifdef INTERP_CLAMP
#include "hardware/interp.h"
#endif

static void clamp_setup(void) {
#ifdef INTERP_CLAMP
    interp_config cfg;
    // Clamp setup
    cfg = interp_default_config();
    interp_config_set_clamp(&cfg, true);
    interp_config_set_shift(&cfg, 14);
    // set mask according to new position of sign bit..
    interp_config_set_mask(&cfg, 0, 17);
    // ...so that the shifted value is correctly sign extended
    interp_config_set_signed(&cfg, true);
    interp_set_config(interp1, 0, &cfg);
    interp1->base[0] = -32768;
    interp1->base[1] = 32767;
#endif
}


static int16_t __force_inline clamp16(int32_t d) {
#ifdef INTERP_CLAMP
    interp1->accum[0] = d;
    return interp1->peek[0];
#else
    return d < -32768 ? -32768 : (d > 32767 ? 32767 : d);
#endif // INTERP_CLAMP
}
