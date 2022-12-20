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

// __force_inline size_t DMA_Write(dma_inst_t* dma, uint32_t dmaaddr, bool invert_msb, bool is_16bit, uint32_t delay, bool* dma_active) {
__force_inline bool DMA_Write(dma_inst_t* dma, uint32_t dmaaddr, bool invert_msb) {
    pio_sm_put_blocking(dma->pio, dma->sm, 0xffffffffu);  // Write 1s to kick off DMA process. note that these 1s are used to set TC flag in PIO!
    // putchar('.');
    uint32_t dma_data = pio_sm_get_blocking(dma->pio, dma->sm);
    // dma_data8[read_num] = (dma_data) & 0xffu;
    // dma_data8 = (dma_data >> 1) & 0xffu;
    uint8_t dma_data8 = dma_data & 0xffu;
    // putchar('>');
    // uart_print_hex_u32(dmaaddr);
    // printf("%x\n", dma_data8);
    // uart_print_hex_u32(dma_data8);
    psram_write8(&psram_spi, dmaaddr, invert_msb ? dma_data8 ^ 0x80 : dma_data8);
    // uart_print_hex_u32(dma_data);
    // Return true if TC, false otherwise
    return (dma_data & 0x100u);
}
