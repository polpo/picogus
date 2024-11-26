#include "pico_pic.h"

#include "pico/time.h"

#include <stdio.h>

// A fixed pool of multiple events.
// gus-x has 3 different timer events - two timers and one DMA
// 8250uart has 3
// mpu401 has 3
// SB has 1

alarm_pool_t* alarm_pool;

int64_t PIC_HandleEvent(alarm_id_t id, void *user_data) {
    PIC_TimerEvent* event = (PIC_TimerEvent *)user_data;
    if (id != event->alarm_id) {
        return 0;
    }
    uint32_t ret = (event->handler)(event->value);
    // printf("called event handler: %x %x, ret %d\n", event->handler, event->value, ret);
    // gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
    if (!ret) {
        event->alarm_id = 0;
        return ret;
    }
    // A negative return value re-sets the alarm from the time when it initially triggered
    return -(int64_t)ret;
    /* return (int64_t)ret; */
}

void PIC_RemoveEvent(PIC_TimerEvent* event) {
    // puts("removeevents");
    if (event->alarm_id) {
        alarm_pool_cancel_alarm(alarm_pool, event->alarm_id);
        event->alarm_id = 0;
    }
}

void PIC_Init() {
    alarm_pool = alarm_pool_create(2, PICO_TIME_DEFAULT_ALARM_POOL_MAX_TIMERS);
    irq_set_priority(TIMER_IRQ_2, PICO_HIGHEST_IRQ_PRIORITY);
}
