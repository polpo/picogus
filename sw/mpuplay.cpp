#include <stdio.h>

#ifdef USE_ALARM
#include "pico_pic.h"
#endif

#include "tusb.h"

#include "mpu401/export.h"

void play_mpu() {
    puts("starting core 1 MPU");

    // Init TinyUSB for joystick support
    tuh_init(BOARD_TUH_RHPORT);

#ifdef USE_ALARM
    // Init PIC on this core so it handles timers
    PIC_Init();
    puts("pic inited on core 1");
#endif

    for (;;) {
        send_midi_byte();				// see if we need to send a byte	
        // Service TinyUSB events
        tuh_task();
    }
}
