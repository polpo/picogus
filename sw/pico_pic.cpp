#include "pico_pic.h"

#include "pico/time.h"

#include <cstdio>

uint32_t timer_0 = 0, timer_1 = 1;

// A fixed pool of only 3 events for now. gus-x only has 3 different timer events - two timers and one DMA
PIC_TimerEvent timerEvents[3];

int64_t PIC_HandleEvent(alarm_id_t id, void *user_data) {
    PIC_TimerEvent* event = (PIC_TimerEvent *)user_data;
    // printf("event fired: %x %x\n", event->handler, event->value);
    (event->handler)(event->value);
    event->handler = nullptr;
    event->value = 0;
    event->alarm_id = 0;
    gpio_put(PICO_DEFAULT_LED_PIN, 0);
    return 0;
}

void PIC_AddEvent(PIC_EventHandler handler, uint32_t delay, Bitu val) {
    // printf("add event: %x %x %d\n", handler, val, delay);
    timerEvents[val].handler = handler;
    timerEvents[val].value = val;
    timerEvents[val].alarm_id = add_alarm_in_us(delay, PIC_HandleEvent, &timerEvents[val], true);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
}

void PIC_RemoveEvents(PIC_EventHandler handler) {
    for (int i = 0; i < 3; ++i) {
        if (timerEvents[i].handler == handler) {
            timerEvents[i].handler = nullptr;
            timerEvents[i].value = 0;
            if (timerEvents[i].alarm_id) {
                cancel_alarm(timerEvents[i].alarm_id);
                timerEvents[i].alarm_id = 0;
            }
        }
    }
}
