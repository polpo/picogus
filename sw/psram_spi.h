#pragma once

#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/sync.h"
#include "stdio.h"

// SPI Defines
#define PIN_CS   1
#define PIN_SCK  2
#define PIN_MOSI 3
#define PIN_MISO 0

#include "psram_spi.pio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pio_spi_inst {
    PIO pio;
    uint sm;
    spin_lock_t* spinlock;
} pio_spi_inst_t;


static __force_inline void __time_critical_func(pio_spi_write_read_blocking)(const pio_spi_inst_t* spi,
                                                       const uint8_t* src, const size_t src_len,
                                                       uint8_t* dst, const size_t dst_len) {
    size_t tx_remain = src_len, rx_remain = dst_len;

    uint32_t irq_state = spin_lock_blocking(spi->spinlock);
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
    spin_unlock(spi->spinlock, irq_state);
}


/**
 * Basic interface to SPI PSRAMs such as Espressif ESP-PSRAM64, apmemory APS6404L, IPUS IPS6404, Lyontek LY68L6400, etc.
 * NOTE that the read/write functions abuse type punning to avoid shifts and masks to be as fast as possible!
 * I'm open to suggestions that this is really dumb or insane. This is a fixed platform (arm-none-eabi-gcc) so I'm OK with it
 */
__force_inline pio_spi_inst_t psram_init(void) {
    printf("add program\n");
    uint spi_offset = pio_add_program(pio1, &spi_fudge_program);
    printf("claim unused sm\n");
    uint spi_sm = pio_claim_unused_sm(pio1, true);
    pio_spi_inst_t spi;
    spi.pio = pio1;
    spi.sm = spi_sm;
    int spin_id = spin_lock_claim_unused(true);
    spi.spinlock = spin_lock_init(spin_id);
    printf("sm is %d\n", spi_sm);

    gpio_set_drive_strength(PIN_CS, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_drive_strength(PIN_SCK, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_drive_strength(PIN_MOSI, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_slew_rate(PIN_CS, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(PIN_SCK, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(PIN_MOSI, GPIO_SLEW_RATE_FAST);

    printf("about to init fudge\n", spi_sm);
    pio_spi_fudge_cs_init(pio1, spi_sm, spi_offset, 8 /*n_bits*/, 1 /*clkdiv*/, PIN_CS, PIN_MOSI, PIN_MISO);

    // SPI initialisation.
    printf("Inited SPI PIO... at sm %d\n", spi.sm);

    busy_wait_us(150);
    uint8_t psram_reset_en_cmd = 0x66u;
    pio_spi_write_read_blocking(&spi, &psram_reset_en_cmd, 1, nullptr, 0);
    busy_wait_us(50);
    uint8_t psram_reset_cmd = 0x99u;
    pio_spi_write_read_blocking(&spi, &psram_reset_cmd, 1, nullptr, 0);
    busy_wait_us(100);

    return spi;
};

__force_inline void psram_write8(pio_spi_inst_t* spi, uint32_t addr, uint8_t val) {
    unsigned char* addr_bytes = (unsigned char*)&addr;
    uint8_t command[5] = {
        0x02u,
        *(addr_bytes + 2),
        *(addr_bytes + 1),
        *addr_bytes,
        val
    };

    pio_spi_write_read_blocking(spi, command, sizeof(command), nullptr, 0);
};

__force_inline uint8_t psram_read8(pio_spi_inst_t* spi, uint32_t addr) {
    uint8_t val; 
    unsigned char* addr_bytes = (unsigned char*)&addr;
    uint8_t command[5] = {
        0x0bu, // fast read command
        *(addr_bytes + 2),
        *(addr_bytes + 1),
        *addr_bytes,
        0,
    };

    pio_spi_write_read_blocking(spi, command, sizeof(command), &val, 1);
    return val;
};

__force_inline void psram_write16(pio_spi_inst_t* spi, uint32_t addr, uint16_t val) {
    unsigned char* addr_bytes = (unsigned char*)&addr;
    unsigned char* val_bytes = (unsigned char*)&val;
    uint8_t command[6] = {
        0x02u,
        *(addr_bytes + 2),
        *(addr_bytes + 1),
        *addr_bytes,
        *val_bytes,
        *(val_bytes + 1)
    };

    pio_spi_write_read_blocking(spi, command, sizeof(command), nullptr, 0);
};

__force_inline uint16_t psram_read16(pio_spi_inst_t* spi, uint32_t addr) {
    uint16_t val; 
    unsigned char* addr_bytes = (unsigned char*)&addr;
    uint8_t command[5] = {
        0x0bu, // fast read command
        *(addr_bytes + 2),
        *(addr_bytes + 1),
        *addr_bytes,
        0
    };

    pio_spi_write_read_blocking(spi, command, sizeof(command), (unsigned char*)&val, 2);
    return val;
};

__force_inline void psram_write32(pio_spi_inst_t* spi, uint32_t addr, uint32_t val) {
    unsigned char* addr_bytes = (unsigned char*)&addr;
    unsigned char* val_bytes = (unsigned char*)&val;
    // Break the address into three bytes and send read command
    uint8_t command[8] = {
        0x02u, // write command
        *(addr_bytes + 2),
        *(addr_bytes + 1),
        *addr_bytes,
        *val_bytes,
        *(val_bytes + 1),
        *(val_bytes + 2),
        *(val_bytes + 3)
    };

    pio_spi_write_read_blocking(spi, command, sizeof(command), nullptr, 0);
};

__force_inline uint32_t psram_read32(pio_spi_inst_t* spi, uint32_t addr) {
    uint32_t val;
    unsigned char* addr_bytes = (unsigned char*)&addr;
    uint8_t command[5] = {
        0x0bu, // fast read command
        *(addr_bytes + 2),
        *(addr_bytes + 1),
        *addr_bytes,
        0
    };

    pio_spi_write_read_blocking(spi, command, sizeof(command), (unsigned char*)&val, 4);
    return val;
};

#ifdef __cplusplus
}
#endif
