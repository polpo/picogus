#include <stdio.h>

#ifdef USE_ALARM
#include "pico_pic.h"
#endif

#include "mpu401/export.h"

void play_mpu() {
    puts("starting core 1");

#ifdef USE_ALARM
    // Init PIC on this core so it handles timers
    PIC_Init();
#endif
    puts("pic inited");

    for (;;) {
        send_midi_byte();				// see if we need to send a byte	
    }
}
