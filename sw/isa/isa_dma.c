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

#include "isa_dma.h"

dma_inst_t DMA_init(PIO pio, uint sm, irq_handler_t dma_isr) {
    dma_inst_t dma;
    dma.offset = pio_add_program(pio, &dma_write_program);
    pio_sm_claim(pio, sm);
    dma.sm = sm;
    dma.pio = pio;
    dma_write_program_init(pio, dma.sm, dma.offset);

    if (dma_isr) {
        uint pio_irq = (pio == pio0) ? PIO0_IRQ_0 : PIO1_IRQ_0;
        irq_set_enabled(pio_irq, false);
        pio_set_irq0_source_enabled(pio, pis_sm0_rx_fifo_not_empty + dma.sm, true);
        irq_set_priority(pio_irq, PICO_HIGHEST_IRQ_PRIORITY);
        irq_set_exclusive_handler(pio_irq, dma_isr);
        irq_set_enabled(pio_irq, true);
    }

    return dma;
};
