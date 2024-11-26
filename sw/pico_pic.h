#pragma once

#include <stdint.h>

#include "pico/time.h"
#include "hardware/gpio.h"

#ifdef DOSBOX_STAGING
#include "dosboxcompat.h"
#else
#include "dosbox-x-compat.h"
#endif

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

// void PIC_AddEvent(PIC_EventHandler handler, uint32_t delay, Bitu val=0);

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
