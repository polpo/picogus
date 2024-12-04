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

#include <string.h>
#include <stdio.h>

#include "flash_settings.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"
#include "pico/stdlib.h"

static const Settings defaultSettings = {
    .magic = SETTINGS_MAGIC,
    .version = SETTINGS_VERSION,
    .startupMode = 1,
    .Global = {
        .waveTableVolume = 100
    },
    .Joy = {
        .basePort = 0xffff
    },
    .GUS = {
        .basePort = 0x240,
        .audioBuffer = 4,
        .dmaInterval = 0,
        .force44k = false
    },
    .SB = {
        .basePort = 0x220,
        .oplBasePort = 0x388,
        .oplSpeedSensitive = false
    },
    .MPU = {
        .basePort = 0x330,
        .delaySysex = false,
        .fakeAllNotesOff = false
    },
    .CMS = {
        .basePort = 0x220
    },
    .Tandy = {
        .basePort = 0x2c0
    },
    .Mouse = {
        .basePort = 0xffff,
        .protocol = 0,
        .reportRate = 60,
        .sensitivity = 0x100
    },
    .NE2K = {
        .basePort = 0x300,
    },
    .WiFi = {
        .ssid = {0},
        .password = {0}
    }
};


#define SETTINGS_SECTOR (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)   //  last sector of a 2M flash (adjust for other flash sizes if needed)
void loadSettings(Settings* settings)
{
    printf("copying settings to %u from %u with size %u\n", settings, XIP_BASE + SETTINGS_SECTOR, sizeof(Settings));
    memcpy(settings, (void *)(XIP_BASE + SETTINGS_SECTOR), sizeof(Settings));
    /* stdio_flush(); */

    if (settings->magic != SETTINGS_MAGIC || settings->version != SETTINGS_VERSION) {
        // Initialize settings to default
        puts("default settings");
        stdio_flush();
        memcpy(settings, &defaultSettings, sizeof(Settings));
        return;
    }
    stdio_flush();
}


void saveSettings(const Settings* settings)
{
    uint8_t data[FLASH_PAGE_SIZE] = {0};
    static_assert(sizeof(Settings) < FLASH_PAGE_SIZE, "Settings struct doesn't fit inside one flash page");
    memcpy(data, settings, sizeof(Settings));
    printf("doing settings save: ");
    // No need for flash_safe_execute or stop second core, since it will not touch flash
    // If the 2nd core will ever touch flash, reconsider this approach!
    // Clock down the RP2040 so the flash at its default 1/2 clock divider is within spec (<=133MHz)
    set_sys_clock_khz(240000, true);

    putchar('/');
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(SETTINGS_SECTOR, FLASH_SECTOR_SIZE);    // last sector
    putchar('/');
    flash_range_program(SETTINGS_SECTOR, data, FLASH_PAGE_SIZE);
    set_sys_clock_khz(RP2_CLOCK_SPEED, true);
    restore_interrupts(ints);
    printf("settings saved");
}

void getDefaultSettings(Settings* settings)
{
    memcpy(settings, &defaultSettings, sizeof(Settings));
}
