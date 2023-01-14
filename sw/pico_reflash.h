#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum {
    PICO_FIRMWARE_IDLE = 0,
    PICO_FIRMWARE_WRITING = 1,
    PICO_FIRMWARE_ERROR = 0xFF
} pico_firmware_status_t;

void pico_firmware_write(uint8_t data);
void pico_firmware_stop(pico_firmware_status_t status);
pico_firmware_status_t pico_firmware_getStatus(void);

#ifdef __cplusplus
}
#endif
