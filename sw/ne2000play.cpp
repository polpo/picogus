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
#include "pico/multicore.h"

#endif

#include "pico/stdlib.h"

extern "C" {
  #include "ne2000/ne2000.h"
}


extern uint LED_PIN;

void play_ne2000() {
    puts("starting core 1 ne2000");
    set_sys_clock_khz(240000, true);//temporary hack because of board definition flash divider
    PG_EnableWifi();
    set_sys_clock_khz(366000, true);

    while(1) {   
        if (multicore_fifo_rvalid()) {
            switch(multicore_fifo_pop_blocking()) {                    
                case FIFO_NE2K_SEND:
                ne2000_initiate_send();
                break;
            default:
                break;
            }
        }                
    }
}
