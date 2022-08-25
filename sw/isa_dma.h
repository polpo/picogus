#pragma once

#include <stdint.h>

#include "hardware/pio.h"

#include "psram_spi.h"
extern pio_spi_inst_t psram_spi;

#include "isa_dma.pio.h"

typedef struct dma_inst {
    PIO pio;
    uint sm;
    uint offset;
} dma_inst_t;

__force_inline dma_inst_t DMA_init(PIO pio) {
    dma_inst_t dma;
    dma.offset = pio_add_program(pio, &dma_write_program);
    dma.sm = pio_claim_unused_sm(pio, true);
    dma.pio = pio;
    dma_write_program_init(pio, dma.sm, dma.offset);

    return dma;
};

__force_inline size_t DMA_Write(dma_inst_t* dma, uint32_t dmaaddr, bool invert_msb, bool is_16bit, uint32_t delay) {
    /*
    union {
        uint32_t dma_data32;
        uint8_t dma_data8[4];
    };
    */
    uint8_t dma_data8;
    size_t read_num = 3, total_read = 0;
    uint32_t dma_data;
    uint32_t cur_addr = dmaaddr;
    // printf("DMA_Write pio %u sm %u dmaaddr %u delay %u\n", dma->pio, dma->sm, dmaaddr, delay);
    while (total_read < 65536) { // absolute max ISA DMA read count is 65536
        pio_sm_put_blocking(dma->pio, dma->sm, 0xffffffffu);  // Write 1s to kick off DMA process. note that these 1s are used to set TC flag in PIO!
        // putchar('.');
        dma_data = pio_sm_get_blocking(dma->pio, dma->sm);
        // dma_data8[read_num] = (dma_data >> 1) & 0xffu;
        // dma_data8 = (dma_data >> 1) & 0xffu;
        dma_data8 = dma_data & 0xffu;
        // putchar('/');
        ++total_read;
        /*
        if (invert_msb) {
            if (!is_16bit || (read_num == 0 || read_num == 2)) {
                dma_data8[read_num] ^= 0x80;
            }
        }
        */
        //if (pio_sm_get_pc(dma->pio, dma->sm) == dma->offset) {
        psram_write8(&psram_spi, cur_addr++, dma_data8);
        // uart_print_hex_u32(dma_data);
        if (dma_data & 0x100u) { // if TC flag is set
            // flush unwritten DMA data
            // stdio_uart_out_chars("!", 1);
            /*
            for (size_t i = read_num; i >= 0; --i) {
                psram_write8(&psram_spi, cur_addr + 3 - i, dma_data8[i]);
            }
            */
            // if read_num = 0, write 4 bytes
            // if read_num = 1, write 3 bytes
            // if read_num = 2, write 2 bytes
            // if read_num = 3, write 1 byte
            //
            /*
            for (size_t i = read_num; i < 4; ++i) {
                psram_write8(&psram_spi, cur_addr + i, dma_data8[i]);
            }
            */
            // psram_write32(&psram_spi, cur_addr, dma_data32);
            return total_read;
        }
        /*
        if (read_num == 0) {
            psram_write32(&psram_spi, cur_addr, dma_data32);
            cur_addr += 4;
            read_num = 3;
        } else {
            --read_num;
        }
        */
        // if (delay) {
        //     busy_wait_us_32(delay);
        // }
    }
    puts("Max DMA write happened");
    return total_read;
}
