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
    uint sm = pio_claim_unused_sm(pio, true);
    dma.pio = pio;
    dma.sm = sm;
    dma_write_program_init(pio, sm, dma.offset);

    return dma;
};

__force_inline size_t DMA_Write(dma_inst_t* dma, uint32_t dmaaddr, bool invert_msb, bool is_16bit, uint32_t delay) {
    union {
        uint32_t dma_data32;
        uint8_t dma_data8[4];
    };
    size_t read_num = 0, total_read = 0;
    uint32_t cur_addr = dmaaddr;
    for (;;) {
        pio_sm_put_blocking(dma->pio, dma->sm, 0);  // Write dummy data to kick off DMA process
        dma_data8[read_num] = pio_sm_get_blocking(dma->pio, dma->sm) & 0xffu;
        ++total_read;
        if (invert_msb) {
            if (!is_16bit || (read_num == 0 || read_num == 2)) {
                dma_data8[read_num] ^= 0x80;
            }
        }
        if (read_num == 3) {
            psram_write32(&psram_spi, cur_addr, dma_data32);
            cur_addr += 4;
            read_num == 0;
        }
        if (pio_sm_get_pc(dma->pio, dma->sm) == dma->offset) {
            // PIO SM has reset back to beginning, so DMA has reached TC
            // flush unwritten DMA data
            for (size_t i = 0; i < read_num; ++i) {
                psram_write8(&psram_spi, cur_addr, dma_data8[i]);
            }
            return total_read;
        }
        ++read_num;
        if (delay) {
            busy_wait_us_32(delay);
        }
    }
}
