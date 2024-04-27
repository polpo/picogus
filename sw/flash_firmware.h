#pragma once

// By Jeroen Taverne
// New features and mods by smymm

// Make sure the FLASH addresses match the .ld files!!!
// ld files for picogus firmwares/modes are based on memmap_copy_to_ram.ld as they run with set(PICO_COPY_TO_RAM 1)

#define NR_OF_FIRMWARES 6
#define FLASH_FIRMWARE1 0x00008000
#define FLASH_FIRMWARE2 0x00040000
#define FLASH_FIRMWARE3 0x00080000
#define FLASH_FIRMWARE4 0x000c0000
#define FLASH_FIRMWARE5 0x00100000
#define FLASH_FIRMWARE6 0x00140000
