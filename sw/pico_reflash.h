#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "../common/picogus.h"

void pico_firmware_write(uint8_t data);
void pico_firmware_start();
pico_firmware_status_t pico_firmware_getStatus(void);

#ifdef __cplusplus
}
#endif
