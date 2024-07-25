#include <stdio.h>

#include "flash_settings.h"
extern Settings settings;

#include "pico/flash.h"

#ifdef USE_ALARM
#include "pico_pic.h"
#endif

#ifdef USB_STACK
#include "tusb.h"
#endif

#include "mpu401/export.h"

void play_mpu() {
    puts("starting core 1 MPU");
    flash_safe_execute_core_init();

#ifdef USB_STACK
    // Init TinyUSB for joystick support
    tuh_init(BOARD_TUH_RHPORT);
#endif

#ifdef USE_ALARM
    // Init PIC on this core so it handles timers
    PIC_Init();
    puts("pic inited on core 1");
#endif
    MPU401_Init(settings.MPU.delaySysex, settings.MPU.fakeAllNotesOff);

    for (;;) {
        send_midi_bytes(4);				// see if we need to send a byte
#ifdef USB_STACK
        // Service TinyUSB events
        tuh_task();
#endif
    }
}
