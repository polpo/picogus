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

#include <stdio.h>

#include "system/flash_settings.h"
extern Settings settings;

#include "system/pico_pic.h"

#ifdef USB_STACK
#include "tusb.h"
#endif

#include "mpu401/export.h"

void play_mpu() {
    puts("starting core 1 MPU");
    // flash_safe_execute_core_init();

#ifdef USB_STACK
    // Init TinyUSB for joystick support
    tuh_init(BOARD_TUH_RHPORT);
#endif

    // Init PIC on this core so it handles timers
    PIC_Init();
    puts("pic inited on core 1");
    MPU401_Init(settings.MPU.delaySysex, settings.MPU.fakeAllNotesOff);

    for (;;) {
        send_midi_bytes(8);
#ifdef USB_STACK
        // Service TinyUSB events
        tuh_task();
#endif
    }
}
