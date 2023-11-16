// Board config file for PicoGUS 2.0, also compatible with the Pi Pico 

#ifndef _BOARDS_PICOGUS2_H
#define _BOARDS_PICOGUS2_H

// Allow extra time for xosc to start.
#define PICO_XOSC_STARTUP_DELAY_MULTIPLIER 64

// Slower flash to assist restarts when flashing on the fly
#define PICO_FLASH_SPI_CLKDIV 4

#include "boards/pico.h"
#endif
