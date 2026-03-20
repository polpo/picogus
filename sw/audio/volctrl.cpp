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

#include <string.h>
#include <stdio.h>

#include "system/flash_settings.h"
#include "pico/stdlib.h"
#include "volctrl.h"
extern Settings settings;

// initialize to default 1.0 scale at start
struct volctrl_t volume = {
    .opl = {0x10000, 0x10000},
    .sb_pcm = {0x10000, 0x10000},
    .cd_audio = {0x10000, 0x10000},
    .sb_master = {0x10000, 0x10000},
    .gus = 0x10000,
    .psg = 0x10000,
};
extern struct volctrl_t volume;

int32_t set_volume_scale (uint8_t percent) {
     if (percent > 100)
        percent = 100;

    uint8_t delta = 100 - settings.Volume.mainVol;
    int32_t adjusted = percent - delta;

    if (adjusted <= 0) 
        return 0;

    if (adjusted > 100)
        adjusted = 100;

    // Apply audio taper curve: use cubic function
    int32_t normalized = (adjusted * 256) / 100;  // Scale to 0-256 (using 8-bit precision)
    int32_t squared = (normalized * normalized) >> 8;  // Square and scale back
    int32_t cubed = (squared * normalized) >> 8;  // Cube and scale back
    return cubed << 8;  // Final scale to 16.16 fixed point (65536 = 256 << 8)
}


void set_volume(uint16_t mode) {

    switch (mode){
        case CMD_MAINVOL:
            volume.sb_master[0] = volume.sb_master[1] = 0x10000;    // reset SB master volume
            set_volume(CMD_OPLVOL);
            set_volume(CMD_SBVOL);
            set_volume(CMD_CDVOL);
            set_volume(CMD_GUSVOL);
            set_volume(CMD_PSGVOL);
            break;
        case CMD_OPLVOL:
            volume.opl[0] = volume.opl[1] = set_volume_scale(settings.Volume.oplVol);
            break;
        case CMD_SBVOL:
            volume.sb_pcm[0] = volume.sb_pcm[1] = set_volume_scale(settings.Volume.sbVol);
            break;
        case CMD_CDVOL:
            volume.cd_audio[0] = volume.cd_audio[1] = set_volume_scale(settings.Volume.cdVol);
            break;
        case CMD_GUSVOL:
            volume.gus = set_volume_scale(settings.Volume.gusVol);
            break;
        case CMD_PSGVOL:
            volume.psg = set_volume_scale(settings.Volume.psgVol);
            break;
    }   
}
