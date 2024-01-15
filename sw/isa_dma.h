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

dma_inst_t DMA_init(PIO pio, uint sm, irq_handler_t dma_isr);

// __force_inline size_t DMA_Write(dma_inst_t* dma, uint32_t dmaaddr, bool invert_msb, bool is_16bit, uint32_t delay, bool* dma_active) {
__force_inline extern void DMA_Start_Write(dma_inst_t* dma) {
    pio_sm_put_blocking(dma->pio, dma->sm, 0xffffffffu);  // Write 1s to kick off DMA process. note that these 1s are used to set TC flag in PIO!
}

// __force_inline uint32_t DMA_Complete_Write(dma_inst_t* dma, uint32_t dmaaddr, bool invert_msb) {
__force_inline extern uint32_t DMA_Complete_Write(dma_inst_t* dma) {
    // putchar('.');
    uint32_t dma_data = pio_sm_get(dma->pio, dma->sm);
    return dma_data;
}

__force_inline extern void DMA_Cancel_Write(dma_inst_t* dma) {
    pio_sm_exec(dma->pio, dma->sm, pio_encode_jmp(dma->offset));
}

#ifdef __cplusplus
} // extern "C"
#endif
