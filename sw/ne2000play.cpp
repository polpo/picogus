/*
 *  Copyright (C) 2022-2024  Ian Scott
 *  Copyright (C) 2024       Kevin Moonlight
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
#include <math.h>
#include <string.h>

#if PICO_ON_DEVICE

#include "hardware/clocks.h"
#include "hardware/structs/clocks.h"
#include "hardware/timer.h"
#include "pico/multicore.h"
#include "pico/flash.h"

#endif

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

extern "C" {
  #include "ne2000/ne2000.h"
}

#include "flash_settings.h"
extern Settings settings;

extern uint LED_PIN;


static uint32_t Wifi_Reconnect_EventHandler(Bitu val) {
    PG_Wifi_Reconnect();
    return 1000000;
}
static PIC_TimerEvent Wifi_Reconnect_Event = {
    .handler = Wifi_Reconnect_EventHandler
};

void play_ne2000() {
    //flash_safe_execute_core_init();
    puts("starting core 1 ne2000");
    PG_EnableWifi();
    PG_Wifi_Connect(settings.WiFi.ssid, settings.WiFi.password);

    while(1) {
        PIC_AddEvent(&Wifi_Reconnect_Event, 1000000, 0);
        if (multicore_fifo_rvalid()) {
            switch(multicore_fifo_pop_blocking()) {
            case FIFO_NE2K_SEND:
                ne2000_initiate_send();
                break;
            case FIFO_WIFI_STATUS:
                PG_Wifi_GetStatus();
                break;
            default:
                break;
            }
        }
        /* cyw43_arch_poll(); */
    }
}
