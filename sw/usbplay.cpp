#include <stdio.h>

#include "tusb.h"

// #include <string.h>

void play_usb() {
    puts("starting core 1 USB");

    // board_init();

    // init host stack on configured roothub port
    tuh_init(BOARD_TUH_RHPORT);

    for (;;) {
        // tinyusb host task
        tuh_task();
    }
}
