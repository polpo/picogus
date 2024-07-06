#include "pico_pic.h"

#include "pico/time.h"

#include <stdio.h>

// A fixed pool of multiple events.
// gus-x has 3 different timer events - two timers and one DMA
// 8250uart has 3
// mpu401 has 3
// SB has 1
PIC_TimerEvent timerEvents[PIC_MAX_TIMERS];

alarm_pool_t* alarm_pool;

int64_t clear_irq(alarm_id_t id, void *user_data) {
    gpio_put(IRQ_PIN, 0); 
    return 0;
}

int64_t PIC_HandleEvent(alarm_id_t id, void *user_data) {
    PIC_TimerEvent* event = (PIC_TimerEvent *)user_data;
#ifdef USE_ALARM
    if (id != event->alarm_id) {
        return 0;
    }
#endif
    uint32_t ret = (event->handler)(event->value);
    // printf("called event handler: %x %x, ret %d\n", event->handler, event->value, ret);
#ifdef USE_ALARM
    // gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
    if (!ret) {
        event->alarm_id = 0;
        return ret;
    }
    // A negative return value re-sets the alarm from the time when it initially triggered
    return -(int64_t)ret;
    /* return (int64_t)ret; */
#else
    if (ret) {
        event->deadline = time_us_32() + ret;
    } else {
        event->active = false;
    }
    return 0;
#endif
}

void PIC_RemoveEvents(PIC_EventHandler handler) {
    // puts("removeevents");
    for (int i = 0; i < PIC_MAX_TIMERS; ++i) {
        if (timerEvents[i].handler == handler) {
#ifdef USE_ALARM
            if (timerEvents[i].alarm_id) {
                // cancel_alarm(timerEvents[i].alarm_id);
                alarm_pool_cancel_alarm(alarm_pool, timerEvents[i].alarm_id);
                timerEvents[i].alarm_id = 0;
            }
#else
            timerEvents[i].active = false;
#endif
        }
    }
}

void PIC_Init() {
#ifdef USE_ALARM
    alarm_pool = alarm_pool_create(2, PICO_TIME_DEFAULT_ALARM_POOL_MAX_TIMERS);
    irq_set_priority(TIMER_IRQ_2, PICO_HIGHEST_IRQ_PRIORITY);
    for (int i = 0; i < PIC_MAX_TIMERS; ++i) {
        timerEvents[i].alarm_id = 0;
        timerEvents[i].handler = 0;
    }
#else
    for (int i = 0; i < PIC_MAX_TIMERS; ++i) {
        timerEvents[i].active = false;
        timerEvents[i].deadline = UINT32_MAX;
        timerEvents[i].handler = 0;
    }
#endif
}
