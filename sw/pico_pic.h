#pragma once

#include <stdint.h>

#include "pico/time.h"
#include "hardware/gpio.h"

#include "dosbox-x-compat.h"

#define IRQ_PIN 19 // TODO don't spread around pin definitions like this

typedef void (* PIC_EventHandler)(Bitu val);

struct PIC_TimerEvent {
    PIC_EventHandler handler;
    Bitu value;
    alarm_id_t alarm_id;
};

__force_inline void PIC_ActivateIRQ(Bitu irq) {
    // puts("activate irq");
    gpio_put(IRQ_PIN, 1); 
}

__force_inline void PIC_DeActivateIRQ(Bitu irq) {
    gpio_put(IRQ_PIN, 0); 
}

void PIC_AddEvent(PIC_EventHandler handler, uint32_t delay, Bitu val=0);
void PIC_RemoveEvents(PIC_EventHandler handler);
