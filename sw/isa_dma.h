#pragma once

#include <stdint.h>

#include "hardware/pio.h"

/*
#include "psram_spi.h"
extern psram_spi_inst_t psram_spi;
*/

#include "isa_dma.pio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dma_inst {
    PIO pio;
    uint sm;
    uint offset;
    bool invertMsb;
} dma_inst_t;

dma_inst_t DMA_init(PIO pio, irq_handler_t dma_isr);

// __force_inline size_t DMA_Write(dma_inst_t* dma, uint32_t dmaaddr, bool invert_msb, bool is_16bit, uint32_t delay, bool* dma_active) {
__force_inline extern void DMA_Start_Write(dma_inst_t* dma) {
    pio_sm_put_blocking(dma->pio, dma->sm, 0xffffffffu);  // Write 1s to kick off DMA process. note that these 1s are used to set TC flag in PIO!
}

// __force_inline uint32_t DMA_Complete_Write(dma_inst_t* dma, uint32_t dmaaddr, bool invert_msb) {
__force_inline extern uint32_t DMA_Complete_Write(dma_inst_t* dma) {
    // pio_sm_put_blocking(dma->pio, dma->sm, 0xffffffffu);  // Write 1s to kick off DMA process. note that these 1s are used to set TC flag in PIO!
    // putchar('.');
    uint32_t dma_data = pio_sm_get(dma->pio, dma->sm);
    return dma_data;
    /*
    // dma_data8[read_num] = (dma_data) & 0xffu;
    // dma_data8 = (dma_data >> 1) & 0xffu;
    uint8_t dma_data8 = dma_data & 0xffu;
    // putchar('>');
    // uart_print_hex_u32(dmaaddr);
    // printf("%x\n", dma_data8);
    // uart_print_hex_u32(dma_data8);
    // psram_write8(&psram_spi, dmaaddr, invert_msb ? dma_data8 ^ 0x80 : dma_data8);
    psram_write8_async(&psram_spi, dmaaddr, invert_msb ? dma_data8 ^ 0x80 : dma_data8);
    //pio_sm_put_blocking(dma->pio, dma->sm, 0);
    // uart_print_hex_u32(dma_data);
    // Return true if TC, false otherwise
    return (dma_data & 0x100u);
    */
}

__force_inline extern void DMA_Cancel_Write(dma_inst_t* dma) {
    pio_sm_exec(dma->pio, dma->sm, pio_encode_jmp(dma->offset));
}

#ifdef __cplusplus
} // extern "C"
#endif
