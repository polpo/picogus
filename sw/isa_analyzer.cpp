/*
 *  Copyright (C) 2025 Ian Scott
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

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <pico/stdlib.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <hardware/structs/ioqspi.h>
#include <hardware/structs/qmi.h>
#include <hardware/structs/xip.h>
#include <hardware/sync.h>
#include <hardware/structs/pads_bank0.h>

#include "isa_analyzer.pio.h"

// PSRAM configuration (Pimoroni Pico Plus 2 onboard PSRAM)
#define PSRAM_CS_PIN         47
#define PSRAM_BASE_UNCACHED  0x15000000
#define PSRAM_SIZE           (8 * 1024 * 1024)

// PIO state machine assignments (both on pio0)
#define IOW_SM 0
#define IOR_SM 1

// RLE compression constants
#define RLE_MAGIC       0xFF000000
#define RLE_COUNT_MASK  0x00FFFFFF
#define RLE_MAX_COUNT   0x00FFFFFF  // 16,777,215

// Binary output markers
constexpr uint32_t BINARY_MARKER_MAGIC = 0x1DE1DE1D;
constexpr uint32_t BINARY_MARKER_START = 0x42494E53;  // "BINS"
constexpr uint32_t BINARY_MARKER_END   = 0x42494E45;  // "BINE"

static uint32_t *data_buffer;
static size_t buffer_size = 0;           // Total buffer capacity in 32-bit words
volatile static size_t write_idx = 0;    // Next write position (monotonically increasing)
volatile static bool buffer_wrapped = false;

// RLE state
static uint32_t last_event = 0;
static uint32_t repeat_count = 0;

// Port filter bitmap: 1024 bits = 128 bytes, one bit per 10-bit address
static uint8_t port_bitmap[128];

// ---- FIFO polling ----

constexpr uint32_t iow_rxempty = 1u << (PIO_FSTAT_RXEMPTY_LSB + IOW_SM);
__force_inline bool iow_has_data() {
    return !(pio0->fstat & iow_rxempty);
}

constexpr uint32_t ior_rxempty = 1u << (PIO_FSTAT_RXEMPTY_LSB + IOR_SM);
__force_inline bool ior_has_data() {
    return !(pio0->fstat & ior_rxempty);
}

// ---- Ring buffer & RLE (from ide_analyzer.cpp) ----

__force_inline void ring_write(uint32_t value) {
    data_buffer[write_idx % buffer_size] = value;
    write_idx++;
    if (write_idx >= buffer_size && !buffer_wrapped) {
        buffer_wrapped = true;
    }
}

__force_inline void flush_rle(void) {
    if (repeat_count == 0) return;

    // Write value first (value-first encoding for ring buffer safety)
    ring_write(last_event);
    // Then write trailer with count if repeated
    if (repeat_count > 1) {
        ring_write(RLE_MAGIC | (repeat_count - 1));
    }
    repeat_count = 0;
}

__force_inline void record_event(uint32_t event) {
    if (repeat_count == 0) {
        last_event = event;
        repeat_count = 1;
    } else if (event == last_event && repeat_count < RLE_MAX_COUNT) {
        repeat_count++;
    } else {
        flush_rle();
        last_event = event;
        repeat_count = 1;
    }
}

// ---- Port filter ----

// Add all ports matching (addr & mask) == (value & mask)
void add_port_filter(uint16_t mask, uint16_t value) {
    for (int addr = 0; addr < 1024; addr++) {
        if ((addr & mask) == (value & mask))
            port_bitmap[addr >> 3] |= 1u << (addr & 7);
    }
}

__force_inline bool port_match(uint16_t addr) {
    return port_bitmap[addr >> 3] & (1u << (addr & 7));
}

// ---- PSRAM init (from ide_analyzer.cpp) ----
// Based on eightycc's PSRAM gist with timing adapted for 370MHz
// Only works on boards with QMI PSRAM (e.g. Pimoroni Pico Plus 2)

static size_t psram_init(void) {
    gpio_set_function(PSRAM_CS_PIN, GPIO_FUNC_XIP_CS1);

    uint32_t save = save_and_disable_interrupts();

    // Initialize QMI in direct mode with slow clock for ID read
    qmi_hw->direct_csr = 30 << QMI_DIRECT_CSR_CLKDIV_LSB |
                         QMI_DIRECT_CSR_EN_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);

    // Exit QPI mode if stuck (0xF5 command)
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                        QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB |
                        0xF5;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

    // Read PSRAM ID (0x9F command) - 12 bytes total
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    uint8_t kgd = 0, eid = 0;
    for (size_t i = 0; i < 12; i++) {
        qmi_hw->direct_tx = (i == 0) ? 0x9F : 0xFF;
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);
        uint8_t rx = qmi_hw->direct_rx;
        if (i == 5) kgd = rx;
        if (i == 6) eid = rx;
    }
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

    // Verify KGD (Known Good Die) = 0x5D for AP Memory PSRAM
    if (kgd != 0x5D) {
        qmi_hw->direct_csr = 0;
        restore_interrupts(save);
        return 0;
    }

    // Send initialization commands: RESETEN, RESET, QPI Enable, Wrap toggle
    uint8_t cmds[] = {0x66, 0x99, 0x35, 0xC0};
    for (int i = 0; i < 4; i++) {
        qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS | cmds[i];
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        for (int j = 0; j < 20; j++) asm("nop");
        (void)qmi_hw->direct_rx;
    }

    qmi_hw->direct_csr = 0;  // Disable direct mode

    // Configure QMI timing for 370MHz with CLKDIV=4 (92.5MHz PSRAM clock)
    qmi_hw->m[1].timing =
        QMI_M0_TIMING_PAGEBREAK_VALUE_1024 << QMI_M0_TIMING_PAGEBREAK_LSB |
        3 << QMI_M0_TIMING_SELECT_HOLD_LSB |
        1 << QMI_M0_TIMING_COOLDOWN_LSB |
        2 << QMI_M0_TIMING_RXDELAY_LSB |
        11 << QMI_M0_TIMING_MAX_SELECT_LSB |   // 8us / (10.81ns x 64) = 11
        5 << QMI_M0_TIMING_MIN_DESELECT_LSB |  // 50ns / 10.81ns = 5
        4 << QMI_M0_TIMING_CLKDIV_LSB;         // 370MHz / 4 = 92.5MHz

    // Read format: Quad mode, 6 dummy cycles (24 bits)
    qmi_hw->m[1].rfmt =
        QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB |
        QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_RFMT_ADDR_WIDTH_LSB |
        QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB |
        QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_RFMT_DUMMY_WIDTH_LSB |
        QMI_M0_RFMT_DUMMY_LEN_VALUE_24 << QMI_M0_RFMT_DUMMY_LEN_LSB |
        QMI_M0_RFMT_DATA_WIDTH_VALUE_Q << QMI_M0_RFMT_DATA_WIDTH_LSB |
        QMI_M0_RFMT_PREFIX_LEN_VALUE_8 << QMI_M0_RFMT_PREFIX_LEN_LSB;

    qmi_hw->m[1].rcmd = 0xEB << QMI_M0_RCMD_PREFIX_LSB;  // Quad Read

    // Write format: Quad mode, no dummy
    qmi_hw->m[1].wfmt =
        QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB |
        QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_WFMT_ADDR_WIDTH_LSB |
        QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB |
        QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_WFMT_DUMMY_WIDTH_LSB |
        QMI_M0_WFMT_DATA_WIDTH_VALUE_Q << QMI_M0_WFMT_DATA_WIDTH_LSB |
        QMI_M0_WFMT_PREFIX_LEN_VALUE_8 << QMI_M0_WFMT_PREFIX_LEN_LSB;

    qmi_hw->m[1].wcmd = 0x38 << QMI_M0_WCMD_PREFIX_LSB;  // Quad Write

    // Enable writable access to M1 (PSRAM)
    xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS;

    __dmb();

    restore_interrupts(save);

    // Determine size from EID
    size_t psram_size = 1024 * 1024;  // Base 1MB
    uint8_t size_id = eid >> 5;
    if (eid == 0x26 || size_id == 2) psram_size *= 8;  // 8MB
    else if (size_id == 0) psram_size *= 2;            // 2MB
    else if (size_id == 1) psram_size *= 4;            // 4MB

    // Verify with write/read test
    volatile uint32_t *psram = (volatile uint32_t *)PSRAM_BASE_UNCACHED;
    psram[0] = 0x12345678;
    if (psram[0] != 0x12345678) return 0;

    return psram_size;
}

void *malloc_max(size_t *size_out) {
    size_t low = 0;
    size_t high = 512 * 1024;  // 512KB for Pico 2

    while (low < high) {
        size_t mid = low + (high - low + 1) / 2;
        void *p = malloc(mid);
        if (p) {
            free(p);
            low = mid;
        } else {
            high = mid - 1;
        }
    }

    void *buffer = NULL;
    if (low > 0) {
        buffer = malloc(low);
    }

    *size_out = buffer ? low : 0;
    return buffer;
}

int main() {
    stdio_init_all();
    // System clock is already 370MHz from SDK auto-config (PLL_SYS_* defines)

    puts("ISA bus analyzer starting up");
    stdio_flush();

    // Initialize PSRAM (must be AFTER clock setup since timing depends on clock speed)
    printf("Initializing PSRAM... ");
    stdio_flush();
    size_t psram_size = psram_init();

    if (psram_size == 0) {
        puts("not found. Falling back to SRAM.");
        stdio_flush();
        data_buffer = (uint32_t*)malloc_max(&buffer_size);
        if (!data_buffer) {
            puts("Couldn't allocate SRAM either");
            return 1;
        }
        printf("Using SRAM buffer: %u bytes\n", buffer_size);
        stdio_flush();
        buffer_size >>= 2;  // Convert bytes to 32-bit words
    } else {
        printf("OK! Detected %u MB\n", psram_size / (1024 * 1024));
        stdio_flush();
        data_buffer = (uint32_t *)PSRAM_BASE_UNCACHED;
        buffer_size = psram_size / sizeof(uint32_t);
        printf("Using PSRAM buffer: %u entries (%u MB)\n",
               buffer_size, psram_size / (1024 * 1024));
        stdio_flush();
    }

    // Disable flash CS for BOOTSEL button detection
    printf("Disabling flash CS (enabling BOOTSEL button)... ");
    stdio_flush();
    hw_write_masked(&ioqspi_hw->io[1].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    puts("done.");
    stdio_flush();

    // Configure bus input pins: enable input, disable isolation (RP2350 requirement),
    // and disable pulls. Without this, PIO reads all zeros.
    const int bus_pins[] = {
        IOW_PIN, IOR_PIN,                           // control signals
        AD0_PIN, AD0_PIN+1, AD0_PIN+2, AD0_PIN+3,  // AD0-AD9
        AD0_PIN+4, AD0_PIN+5, AD0_PIN+6, AD0_PIN+7,
        AD0_PIN+8, AD0_PIN+9,
        DACK_PIN, TC_PIN,                           // DMA signals
    };
    for (int pin : bus_pins) {
        gpio_set_input_enabled(pin, true);
        hw_clear_bits(&pads_bank0_hw->io[pin], PADS_BANK0_GPIO0_ISO_BITS);
        gpio_disable_pulls(pin);
    }

    // Drive IOCHRDY LOW = "ready" on ISA bus (no wait states inserted)
    gpio_init(IOCHRDY_PIN);
    gpio_set_dir(IOCHRDY_PIN, GPIO_OUT);
    gpio_put(IOCHRDY_PIN, 0);

    // Waggle ADS to set BUSOE latch (enable bus transceivers)
    puts("Enabling bus transceivers...");
    gpio_init(ADS_PIN);
    gpio_set_dir(ADS_PIN, GPIO_OUT);
    gpio_put(ADS_PIN, 1);
    busy_wait_ms(10);
    gpio_put(ADS_PIN, 0);

    gpio_set_slew_rate(ADS_PIN, GPIO_SLEW_RATE_FAST);

    // Configure PIO
    puts("Starting ISA bus PIO...");
    const uint iow_offset = pio_add_program(pio0, &iow_capture_program);
    pio_sm_claim(pio0, IOW_SM);

    const uint ior_offset = pio_add_program(pio0, &ior_capture_program);
    pio_sm_claim(pio0, IOR_SM);

    iow_capture_program_init(pio0, IOW_SM, iow_offset);
    ior_capture_program_init(pio0, IOR_SM, ior_offset);

    // Port filter
    // To filter specific port ranges, clear bitmap and use add_port_filter():
    memset(port_bitmap, 0, sizeof(port_bitmap));
    add_port_filter(0x3F0, 0x220);  // SB 0x220-0x22F
    add_port_filter(0x3FC, 0x388);  // OPL 0x388-0x38B
    // add_port_filter(0x2F0, 0x240);  // GUS 0x240-0x24F + 0x340-0x34F
    // add_port_filter(0x3F8, 0x530);  // WSS 0x530-0x537
   
    // Otherwise to capture all, fill with 0xFF
    //  memset(port_bitmap, 0xFF, sizeof(port_bitmap));

    printf("Buffer: %u entries (%u KB)\n", buffer_size, (buffer_size * 4) / 1024);

    // Print active port filter ranges
    {
        int count = 0;
        for (int i = 0; i < 1024; i++)
            if (port_bitmap[i >> 3] & (1u << (i & 7))) count++;

        if (count == 1024) {
            puts("Filter: all ports");
        } else if (count == 0) {
            puts("Filter: DMA only (no I/O ports)");
        } else {
            printf("Filter: %d ports:", count);
            int range_start = -1;
            for (int i = 0; i <= 1024; i++) {
                bool set = (i < 1024) && (port_bitmap[i >> 3] & (1u << (i & 7)));
                if (set && range_start < 0) {
                    range_start = i;
                } else if (!set && range_start >= 0) {
                    printf(" 0x%03X-0x%03X", range_start, i - 1);
                    range_start = -1;
                }
            }
            puts("");
        }
    }
    puts("Ready! Waiting for ISA bus activity (press BOOTSEL to stop capture)...");
    stdio_flush();

    // ---- Capture loop ----
    for (;;) {
        if (iow_has_data()) {
            uint32_t raw = pio_sm_get(pio0, IOW_SM);
            uint16_t addr = (raw >> 22) & 0x3FF;
            uint8_t data = (raw >> 14) & 0xFF;
            bool is_dma = (raw >> 13) & 1;

            //printf("w %x ", addr);

            if (!is_dma && !port_match(addr))
                goto check_ior;

            // Stored event format:
            //   Bit 31:    Direction (0=IOW, 1=IOR)
            //   Bit 30:    DMA flag
            //   Bits 17-8: 10-bit I/O address
            //   Bits 7-0:  8-bit data
            uint32_t event = (addr << 8) | data;
            if (is_dma) event |= (1u << 30);
            record_event(event);
        }
check_ior:
        if (ior_has_data()) {
            uint32_t raw = pio_sm_get(pio0, IOR_SM);
            uint16_t addr = (raw >> 22) & 0x3FF;
            uint8_t data = (raw >> 14) & 0xFF;
            bool is_dma = (raw >> 13) & 1;

            if (!is_dma && !port_match(addr))
                goto check_bootsel;

            uint32_t event = (1u << 31) | (addr << 8) | data;
            if (is_dma) event |= (1u << 30);
            record_event(event);
        }
check_bootsel:
        if (!(sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS))
            break;
    }

    // ---- Dump captured data ----

    flush_rle();

    // Calculate valid data range
    size_t entries_captured;
    size_t start_idx;

    if (buffer_wrapped) {
        entries_captured = buffer_size;
        start_idx = write_idx % buffer_size;

        // Skip orphaned RLE trailers at the start
        while (entries_captured > 0 && (data_buffer[start_idx] & 0xFF000000) == RLE_MAGIC) {
            start_idx = (start_idx + 1) % buffer_size;
            entries_captured--;
        }
    } else {
        entries_captured = write_idx;
        start_idx = 0;
    }

    printf("\nCapture complete. %u buffer entries.\n", entries_captured);
    stdio_flush();

    // Write binary markers and data
    uint32_t start_marker[2] = {BINARY_MARKER_MAGIC, BINARY_MARKER_START};
    stdio_put_string((char*)start_marker, sizeof(start_marker), false, false);
    stdio_flush();

    if (buffer_wrapped) {
        size_t first_chunk = buffer_size - start_idx;
        stdio_put_string((char*)&data_buffer[start_idx],
                         first_chunk * sizeof(uint32_t), false, false);
        stdio_flush();
        if (start_idx > 0) {
            stdio_put_string((char*)data_buffer,
                             start_idx * sizeof(uint32_t), false, false);
            stdio_flush();
        }
    } else {
        stdio_put_string((char*)data_buffer, write_idx * sizeof(uint32_t), false, false);
        stdio_flush();
    }

    uint32_t end_marker[2] = {BINARY_MARKER_MAGIC, BINARY_MARKER_END};
    stdio_put_string((char*)end_marker, sizeof(end_marker), false, false);
    stdio_flush();

    // Count events for statistics
    size_t total_events = 0;
    for (size_t j = 0; j < entries_captured; ++j) {
        size_t i = buffer_wrapped ? ((start_idx + j) % buffer_size) : j;
        uint32_t word = data_buffer[i];
        if ((word & 0xFF000000) == RLE_MAGIC) {
            total_events += (word & RLE_COUNT_MASK);
        } else {
            total_events++;
        }
    }

    printf("\n--- END --- (%u events from %u buffer entries, %.1f%% compression)\n",
           total_events, entries_captured,
           100.0 * (1.0 - (float)entries_captured / (float)total_events));
    stdio_flush();
}
