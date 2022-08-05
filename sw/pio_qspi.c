#include "pio_qspi.h"
#include <stdio.h>

void __time_critical_func(pio_spi_write_read_blocking)(const pio_spi_inst_t *spi,
                                                       const uint8_t *src, const size_t src_len,
                                                       uint8_t *dst, const size_t dst_len) {
    size_t tx_remain = src_len, rx_remain = dst_len;

    // Put bytes to write in X
    pio_sm_put_blocking(spi->pio, spi->sm, src_len * 8);
    // Put bytes to read in Y
    pio_sm_put_blocking(spi->pio, spi->sm, dst_len * 8);

    io_rw_8 *txfifo = (io_rw_8 *) &spi->pio->txf[spi->sm];
    while (tx_remain) {
        if (!pio_sm_is_tx_fifo_full(spi->pio, spi->sm)) {
            *txfifo = *src++;
            --tx_remain;
        }
    }

    io_rw_8 *rxfifo = (io_rw_8 *) &spi->pio->rxf[spi->sm];
    while (rx_remain) {
        if (!pio_sm_is_rx_fifo_empty(spi->pio, spi->sm)) {
            *dst++ = *rxfifo;
            --rx_remain;
        }
    }
}

void __time_critical_func(pio_qspi_write_read_blocking)(const pio_spi_inst_t *spi, const uint32_t cmd,
                                                        const uint8_t *src, const size_t src_len,
                                                        uint8_t *dst, const size_t dst_len) {

    size_t tx_remain = src_len, rx_remain = dst_len;

    // Put bytes to write in X
    pio_sm_put(spi->pio, spi->sm, src_len);
    // Put bytes to read in Y
    pio_sm_put(spi->pio, spi->sm, dst_len);
    // Push command
    pio_sm_put(spi->pio, spi->sm, cmd);

    io_rw_8 *txfifo = (io_rw_8 *) &spi->pio->txf[spi->sm];
    while (tx_remain) {
        if (!pio_sm_is_tx_fifo_full(spi->pio, spi->sm)) {
            *txfifo = *src++;
            --tx_remain;
        }
    }

    io_rw_8 *rxfifo = (io_rw_8 *) &spi->pio->rxf[spi->sm];
    while (rx_remain) {
        if (!pio_sm_is_rx_fifo_empty(spi->pio, spi->sm)) {
            *dst++ = *rxfifo;
            --rx_remain;
        }
    }
}

