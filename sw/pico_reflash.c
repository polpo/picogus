#include "pico_reflash.h"

#include <stdio.h>
#include <hardware/flash.h>
#include "hardware/clocks.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

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

static volatile pico_firmware_status_t pico_firmware_status = PICO_FIRMWARE_IDLE;

static void pico_firmware_reset(pico_firmware_status_t status)
{
    pico_firmware_curByte = 0;
    pico_firmware_curBlock = 0;
    pico_firmware_numBlocks = 0;
    pico_firmware_payloadSize = 0;
    pico_firmware_status = status;
}

static void pico_firmware_process_block(void)
{
    if (uf2_buf.uf2.magicStart0 != 0x0A324655 || uf2_buf.uf2.magicStart1 != 0x9E5D5157 || uf2_buf.uf2.magicEnd != 0x0AB16F30) {
        // Invalid UF2 file
        puts("Invalid UF2 data!");
        pico_firmware_reset(PICO_FIRMWARE_ERROR);
        return;
    }
    pico_firmware_status = PICO_FIRMWARE_BUSY;
    if (pico_firmware_curBlock == 0) {
        puts("Starting firmware write...");
        pico_firmware_numBlocks = uf2_buf.uf2.numBlocks;
        printf("numBlocks: %u\n", pico_firmware_numBlocks);
        pico_firmware_payloadSize = uf2_buf.uf2.payloadSize;
        uint32_t totalSize = pico_firmware_numBlocks * pico_firmware_payloadSize;
        // Point of no return... erasing the flash!
        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(0, (totalSize / 4096) * 4096 + 4096);
        restore_interrupts(ints);
    }
    uint32_t curAddress = pico_firmware_curBlock * pico_firmware_payloadSize;
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(curAddress, uf2_buf.uf2.data, pico_firmware_payloadSize);
    restore_interrupts(ints);
    printf("curBlock: %u\n", pico_firmware_curBlock);
    ++pico_firmware_curBlock;
    if (pico_firmware_curBlock == pico_firmware_numBlocks) {
        // Final block has been written
        puts("Final block written.");
        pico_firmware_status = PICO_FIRMWARE_DONE;
    } else {
        pico_firmware_status = PICO_FIRMWARE_WRITING;
    }
}


void pico_firmware_write(uint8_t data)
{
    multicore_fifo_push_blocking(data);
}

void firmware_loop() {
    puts("starting core 1");

    for (;;) {
        if (!multicore_fifo_rvalid()) {
            continue;
        }
        uint8_t data = (uint8_t)multicore_fifo_pop_blocking();
        if (pico_firmware_curByte == 0 && pico_firmware_curBlock == 0) {
            // Writing first byte
            pico_firmware_status = PICO_FIRMWARE_WRITING;
        }
        uf2_buf.buf[pico_firmware_curByte++] = data;
        if (pico_firmware_curByte == 512) {
            pico_firmware_process_block();
            pico_firmware_curByte = 0;
        }
    }
}

static void pico_firmware_reboot()
{
    puts("Rebooting!");
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
    // Clock down the RP2040 so the flash at its default 1/2 clock divider is within spec (<=133MHz)
    set_sys_clock_khz(240000, true);
    multicore_launch_core1(&firmware_loop);
}

pico_firmware_status_t pico_firmware_getStatus(void)
{
    return pico_firmware_status;
}
