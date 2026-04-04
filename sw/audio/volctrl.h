/*
 *  Copyright (C) 2025  Daniel Arnold
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

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "../common/picogus.h"
#include "system/flash_settings.h"
#include "clamp.h"

struct volctrl_t {
    // 0 - left, 1 - right
    int32_t opl[2];             // OPL3   (aka MIDI)
    int32_t sb_pcm[2];          // SB PCM (aka Voice)
    int32_t cd_audio[2];        // CD audio
    int32_t sb_master[2];       // SB Master volume

    int32_t gus;
    int32_t psg;
};
extern struct volctrl_t volume;

#define VOLCTRL_FRACT_BITS 12       // leave enough headroom for mixing

extern int32_t set_volume_scale (uint8_t percent);
// Optional callback invoked when a volume changes, so dependents can update
// precomputed values (e.g. GUS prescaled channel volumes).
typedef void (*volctrl_callback_t)(void);
extern volctrl_callback_t volctrl_gus_callback;
static inline int32_t scale_sample (int32_t sample, const int32_t scale, const bool clamp) {
#ifdef INTERP_VOLCTRL
    if (clamp) {
        // interp1 does >>VOLCTRL_FRACT_BITS and clamp to int16_t in one shot.
        // Only interp1 supports clamp mode on RP2040.
        return clamp16_interp(sample * scale);
    }
#endif
    sample = (sample * scale) >> VOLCTRL_FRACT_BITS;
#ifndef INTERP_VOLCTRL
    if (clamp)
        sample = clamp16(sample);
#endif
    return sample;
}

void set_volume(uint16_t mode);

#ifdef __cplusplus
}
#endif
