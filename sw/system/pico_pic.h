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

#include <stdint.h>

#include "pico/time.h"
#include "hardware/gpio.h"

#include "include/dosbox-x-compat.h"

#define IRQ_PIN 21 // TODO don't spread around pin definitions like this

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t (* PIC_EventHandler)(Bitu val);

typedef struct {
    PIC_EventHandler handler;
    Bitu value;
    alarm_id_t alarm_id;
} PIC_TimerEvent;

extern alarm_pool_t* alarm_pool;

int64_t PIC_HandleEvent(alarm_id_t id, void *user_data);

static __force_inline void PIC_ActivateIRQ(void) {
    // puts("activate irq");
    gpio_put(IRQ_PIN, 1); 
    // alarm_pool_add_alarm_in_us(alarm_pool, 500, clear_irq, 0, true);
}

static __force_inline void PIC_DeActivateIRQ(void) {
    gpio_put(IRQ_PIN, 0); 
}

static __force_inline void PIC_AddEvent(PIC_TimerEvent* event, uint32_t delay, Bitu val) {
    // event->handler = handler;
    event->value = val;
    // alarm_pool_cancel_alarm(alarm_pool, event->alarm_id);
    event->alarm_id = alarm_pool_add_alarm_in_us(alarm_pool, delay, PIC_HandleEvent, event, true);
    // gpio_put(PICO_DEFAULT_LED_PIN, 1);
}

void PIC_RemoveEvent(PIC_TimerEvent* event);

void PIC_Init(void);


#ifdef __cplusplus
}
#endif
