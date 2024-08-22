/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
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

void play_ne2000() {
    //flash_safe_execute_core_init();
    puts("starting core 1 ne2000");
    set_sys_clock_khz(240000, true);//temporary hack because of board definition flash divider
    PG_EnableWifi();
    PG_Wifi_Connect(settings.WiFi.ssid, settings.WiFi.password);
    set_sys_clock_khz(366000, true);

    static bool flag = false;
    while(1) {
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
        /*
        if (((time_us_32() >> 21) & 0x1) == 0x1) { 
            if (flag == false) {
                PG_Wifi_Reconnect();
                flag = true;
            }
        } else {
            flag = false;
        }
        */
    }
}
