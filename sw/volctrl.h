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
#include "flash_settings.h"

extern int32_t opl_volume;
extern int32_t sb_volume;
extern int32_t cd_audio_volume;
extern int32_t gus_volume;
extern int32_t psg_volume;


extern int32_t set_volume_scale (uint8_t percent);
extern int32_t scale_sample (int32_t sample, int32_t scale, int clamp);

void set_volume(uint16_t mode);

#ifdef __cplusplus
}
#endif
