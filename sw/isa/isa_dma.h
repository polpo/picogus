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
dma_inst_t DMA_multi_init(PIO pio, uint sm, irq_handler_t dma_isr);

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
    if (pio_sm_get_pc(dma->pio, dma->sm) != dma->offset+1) {
        pio_sm_exec(dma->pio, dma->sm, pio_encode_jmp(dma->offset));
    }
}

// xfer_count: number of DMA transfers for each push. DRQ is held high for all transfers
__force_inline extern void DMA_Multi_Start_Write(dma_inst_t* dma, uint32_t xfer_count) {
    pio_sm_put_blocking(dma->pio, dma->sm, xfer_count - 1);
}

// push_thresh: set number of bits for the DMA pio to autopush
__force_inline extern void DMA_Multi_Set_Push_Threshold(dma_inst_t* dma, uint32_t push_thresh) {
    hw_write_masked(&dma->pio->sm[dma->sm].shiftctrl,
                    push_thresh << PIO_SM0_SHIFTCTRL_PUSH_THRESH_LSB,
                    PIO_SM0_SHIFTCTRL_PUSH_THRESH_BITS);
}

// Nanosecond-accurate DMA interval dithering.
// Alternates between floor and ceil microsecond intervals so the average
// converges on the exact nanosecond-precision sample period.
typedef struct dma_dither {
    uint32_t avg;
    uint32_t target;
    uint32_t rate;
    uint32_t trim;
} dma_dither_t;

// target_ns: exact period in nanoseconds (e.g. 1000000000 / sample_rate)
// interval_us: floor'd microsecond period (target_ns / 1000)
__force_inline extern void DMA_Dither_Init(dma_dither_t* d, uint32_t target_ns, uint32_t interval_us) {
    d->target = target_ns;
    d->rate = interval_us * 1000;
    d->avg = target_ns;
    d->trim = 0;
}

// Advance the dither filter and return the interval to use (base_interval or base_interval+1).
__force_inline extern uint32_t DMA_Dither_Step(dma_dither_t* d, uint32_t base_interval) {
    d->avg = (d->rate + d->trim + 31 * d->avg) >> 5;  // α=1/32 EMA
    d->trim = (d->avg < d->target) ? 1000 : 0;
    return base_interval + (d->trim ? 1 : 0);
}

#ifdef __cplusplus
} // extern "C"
#endif
