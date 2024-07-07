// By Jeroen Taverne
// New features and mods by smymm

#include "pico/stdlib.h"
#include "hardware/structs/watchdog.h"
#include <hardware/flash.h>
#include "flash_firmware.h"
#include "../flash_settings.h"
#include <stdio.h>

static uint32_t sStart = 0;
static const uint32_t offset[NR_OF_FIRMWARES] = {FLASH_FIRMWARE1, FLASH_FIRMWARE2, FLASH_FIRMWARE3, FLASH_FIRMWARE4, FLASH_FIRMWARE5, FLASH_FIRMWARE6, FLASH_FIRMWARE7};

uint8_t read_permMode(void)
{
    Settings settings;
    loadSettings(&settings);
    uint8_t pModeByte = settings.startupMode;

    if (pModeByte >= 1 && pModeByte <= NR_OF_FIRMWARES)
        return (pModeByte - 1);
    else
        return 0;   // No valid value, boot 1st fw.
}


int main(void)
{
    stdio_init_all();
    printf("picogus bootloader\n");
    stdio_flush();
    uint8_t firmware_nr = (uint8_t) (0x000000FF & watchdog_hw->scratch[3]);

    if (firmware_nr > NR_OF_FIRMWARES) {
        firmware_nr = 0;
    }

    if (firmware_nr == 0)   // Try to boot from permanent storage if no change requested / cold boot / bad value
    {
        sStart = offset[read_permMode()] + XIP_BASE;
    } else {    // Mode change requested by pgusinit
    	sStart = offset[firmware_nr-1] + XIP_BASE;
    }

    // Jump to application
    asm volatile
    (
         "mov r0, %[start]\n"
         "ldr r1, =%[vtable]\n"
         "str r0, [r1]\n"
         "ldmia r0, {r0, r1}\n"
         "msr msp, r0\n"
         "bx r1\n"
         :
         : [start] "r" (sStart + 0x100), [vtable] "X" (PPB_BASE + M0PLUS_VTOR_OFFSET)
         :
    );

    return 0;
}
