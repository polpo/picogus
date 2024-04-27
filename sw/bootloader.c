// By Jeroen Taverne
// New features and mods by smymm

#include "pico/stdlib.h"
#include "hardware/structs/watchdog.h"
#include <hardware/flash.h>
#include "pico/multicore.h"
#include "flash_firmware.h"

static uint32_t sStart = 0;
static const uint32_t permOffset = (2048 * 1024) - FLASH_PAGE_SIZE;   //  last page of a 2M flash (adjust for other flash sizes if needed)
static const uint32_t offset[NR_OF_FIRMWARES] = {FLASH_FIRMWARE1, FLASH_FIRMWARE2, FLASH_FIRMWARE3, FLASH_FIRMWARE4, FLASH_FIRMWARE5, FLASH_FIRMWARE6};

uint8_t read_permMode(void)
{
    const uint8_t *pMode = (const uint8_t *) (XIP_BASE + permOffset);
    uint8_t pModeByte = *pMode;

    if (pModeByte >= 1 && pModeByte <= NR_OF_FIRMWARES)
        return (pModeByte - 1);
    else
        return 0;   // No valid value, boot 1st fw.
}

void write_permMode(uint8_t pModeFw)  // warning!! erasing last 4kb sector of the flash, and writing last page
{
    uint8_t data[FLASH_PAGE_SIZE] = {0};

    data[0] = pModeFw;

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase((permOffset + FLASH_PAGE_SIZE - FLASH_SECTOR_SIZE), FLASH_SECTOR_SIZE);    // last sector
    restore_interrupts(ints);
    sleep_ms(100);
    ints = save_and_disable_interrupts();
    flash_range_program(permOffset, data, FLASH_PAGE_SIZE);     // last page
    restore_interrupts(ints);
}

int main(void)
{
	uint8_t firmware_nr = (uint8_t) (0x000000FF & watchdog_hw->scratch[3]);
    uint8_t firmware_mode = (uint8_t) (0x000000FF & (watchdog_hw->scratch[3] >> 8));

	if (firmware_nr > NR_OF_FIRMWARES) {
		firmware_nr = 0;
	}

    if (firmware_mode == 1 && firmware_nr > 0) // Perm mode requested. Write fw num to flash.
        write_permMode(firmware_nr);

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
