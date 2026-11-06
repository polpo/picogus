/*
 *  Copyright (C) 2022-2024  Ian Scott
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "pico_reflash.h"

#include <hardware/flash.h>
#include "../include/pg_debug.h"
#include "hardware/clocks.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"

#if RP2_CLOCK_SPEED / PICO_FLASH_SPI_CLKDIV > 133000
#error "PICO_FLASH_SPI_CLKDIV must be at least 4 when running at 370MHz"
#endif

static union {
    uint8_t buf[512];
    struct UF2_Block {
        // 32 byte header
        uint32_t magicStart0;
        uint32_t magicStart1;
        uint32_t flags;
        uint32_t targetAddr;
        uint32_t payloadSize;
        uint32_t blockNo;
        uint32_t numBlocks;
        uint32_t fileSize; // or familyID;
        uint8_t data[476];
        uint32_t magicEnd;
    } uf2;
} uf2_buf;

static uint32_t pico_firmware_curByte = 0;
static uint32_t pico_firmware_curBlock = 0;
static uint32_t pico_firmware_numBlocks = 0;
static uint32_t pico_firmware_payloadSize = 0;
static uint32_t pico_firmware_totalSize = 0;
// Flash below this address has been erased for the incoming firmware
static uint32_t pico_firmware_erasedEnd = 0;

// Set by core 0 when a full 512 byte block is in uf2_buf, cleared by core 1 when it picks it up
static volatile bool pico_firmware_blockReady = false;
// Set by core 0 if a byte arrives while core 1 is still processing a block
static volatile bool pico_firmware_overrun = false;

static volatile pico_firmware_status_t pico_firmware_status = PICO_FIRMWARE_IDLE;

static void pico_firmware_reset(pico_firmware_status_t status)
{
    pico_firmware_curByte = 0;
    pico_firmware_curBlock = 0;
    pico_firmware_numBlocks = 0;
    pico_firmware_payloadSize = 0;
    pico_firmware_totalSize = 0;
    pico_firmware_erasedEnd = 0;
    pico_firmware_blockReady = false;
    pico_firmware_overrun = false;
    pico_firmware_status = status;
}

// Erase flash up to (at least) end. This is used a sector at a time as blocks arrive, instead of all up front. This keeps
// each wait seen by pgusinit short. Use 64KB block erases where possible since they are much faster per byte.
static void pico_firmware_erase_to(uint32_t end)
{
    while (pico_firmware_erasedEnd < end) {
        uint32_t len = FLASH_SECTOR_SIZE;
        if ((pico_firmware_erasedEnd % FLASH_BLOCK_SIZE) == 0 &&
            pico_firmware_erasedEnd + FLASH_BLOCK_SIZE <= pico_firmware_totalSize) {
            len = FLASH_BLOCK_SIZE;
        }
        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(pico_firmware_erasedEnd, len);
        restore_interrupts(ints);
        pico_firmware_erasedEnd += len;
    }
}

static void pico_firmware_process_block(void)
{
    if (uf2_buf.uf2.magicStart0 != 0x0A324655 || uf2_buf.uf2.magicStart1 != 0x9E5D5157 || uf2_buf.uf2.magicEnd != 0x0AB16F30) {
        // Invalid UF2 file
        ERR_PUTS("Invalid UF2 data!");
        pico_firmware_reset(PICO_FIRMWARE_ERROR);
        return;
    }
    if (pico_firmware_curBlock == 0) {
        DBG_PUTS("Starting firmware write...");
        pico_firmware_numBlocks = uf2_buf.uf2.numBlocks;
        DBG_PRINTF("numBlocks: %u\n", pico_firmware_numBlocks);
        pico_firmware_payloadSize = uf2_buf.uf2.payloadSize;
        uint32_t totalSize = pico_firmware_numBlocks * pico_firmware_payloadSize;
        pico_firmware_totalSize = (totalSize + FLASH_SECTOR_SIZE - 1) & ~(FLASH_SECTOR_SIZE - 1);
    }
    uint32_t curAddress = pico_firmware_curBlock * pico_firmware_payloadSize;
    // Point of no return... erasing the flash! (first block now, the rest as blocks arrive)
    pico_firmware_erase_to(curAddress + pico_firmware_payloadSize);
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(curAddress, uf2_buf.uf2.data, pico_firmware_payloadSize);
    restore_interrupts(ints);
    DBG_PRINTF("curBlock: %u\n", pico_firmware_curBlock);
    ++pico_firmware_curBlock;
    if (pico_firmware_overrun) {
        // Host wrote data while the block was being processed, so the next block is corrupt
        ERR_PUTS("Firmware data overrun!");
        pico_firmware_reset(PICO_FIRMWARE_ERROR);
    } else if (pico_firmware_curBlock == pico_firmware_numBlocks) {
        // Final block has been written
        DBG_PUTS("Final block written.");
        pico_firmware_status = PICO_FIRMWARE_DONE;
    } else {
        pico_firmware_status = PICO_FIRMWARE_WRITING;
    }
}


// Called from the ISA handler on core 0, so it never blocks by putting the byte in the block
// buffer and core 1 does the slow flash work once a full block has arrived.
void pico_firmware_write(uint8_t data)
{
    if (pico_firmware_status == PICO_FIRMWARE_BUSY) {
        // Core 1 still owns uf2_buf; drop the byte and fail the block.
        // In practice this shouldn't happen because pgusinit won't send a byte if it sees BUSY
        pico_firmware_overrun = true;
        return;
    }
    if (pico_firmware_curByte == 0 && pico_firmware_curBlock == 0) {
        // Writing first byte
        pico_firmware_status = PICO_FIRMWARE_WRITING;
    }
    uf2_buf.buf[pico_firmware_curByte++] = data;
    if (pico_firmware_curByte == 512) {
        pico_firmware_curByte = 0;
        // Set BUSY immediately so the status read right after the last byte is never a stale WRITING
        pico_firmware_status = PICO_FIRMWARE_BUSY;
        __dmb();
        pico_firmware_blockReady = true;
    }
}

void firmware_loop() {
    DBG_PUTS("starting core 1");

    for (;;) {
        if (!pico_firmware_blockReady) {
            continue;
        }
        pico_firmware_blockReady = false;
        __dmb();
        pico_firmware_process_block();
    }
}

static void pico_firmware_reboot()
{
    DBG_PUTS("Rebooting!");
    // Stop second core
    multicore_reset_core1();
    // Go back to stock speed
    set_sys_clock_khz(125000, true);
    // Reboot the Pico!
    // Undocumented method to reboot the Pico without messing around with the watchdog. Source:
    // https://forums.raspberrypi.com/viewtopic.php?p=1928868&sid=09bfd964ebd49cc6349581ced3b4b9b9#p1928868
    #define AIRCR_Register (*((volatile uint32_t*)(PPB_BASE + 0x0ED0C)))
    AIRCR_Register = 0x5FA0004;
}

void pico_firmware_start()
{
    if (pico_firmware_status == PICO_FIRMWARE_DONE) {
        pico_firmware_reboot();
        return;
    }
    pico_firmware_reset(PICO_FIRMWARE_IDLE);
    // Stop second core
    multicore_reset_core1();
    multicore_launch_core1(&firmware_loop);
}

pico_firmware_status_t pico_firmware_getStatus(void)
{
    return pico_firmware_status;
}
