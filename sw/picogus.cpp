#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "hardware/regs/vreg_and_chip_reset.h"

#ifdef PSRAM
#include "psram_spi.h"
pio_spi_inst_t psram_spi;
#endif
#include "isa_io.pio.h"

#ifndef SOUND_MPU
#include "stdio_async_uart.h"
#endif 
// UART_TX_PIN is defined in isa_io.pio.h
#define UART_RX_PIN (-1)
#define UART_ID     uart0
#define BAUD_RATE   115200

#ifdef SOUND_OPL
#include "opl.h"

void play_adlib(void);
extern "C" int OPL_Pico_Init(unsigned int);
extern "C" void OPL_Pico_PortWrite(opl_port_t, unsigned int);
extern "C" unsigned int OPL_Pico_PortRead(opl_port_t);
#endif

#ifdef SOUND_GUS
#include "gus-x.cpp"

#include "isa_dma.h"
dma_inst_t dma_config;

constexpr uint16_t GUS_PORT_TEST = GUS_PORT >> 4 | 0x10;
void play_gus(void);
#endif

#ifdef SOUND_MPU
#include "mpu401/export.h"
#endif

constexpr uint LED_PIN = PICO_DEFAULT_LED_PIN;

static uint iow_sm;
static uint ior_sm;
static uint ior_write_sm;

__force_inline void handle_iow(void) {
    bool iochrdy_on;
    uint32_t iow_read = pio_sm_get(pio0, iow_sm); //>> 16;
    // printf("%x", iow_read);
    // printf("IOW: %x\n", iow_read);
    uint16_t port = (iow_read >> 8) & 0x3FF;
#ifdef SOUND_GUS
    if ((port >> 4 | 0x10) == GUS_PORT_TEST) {
        switch (port) {
        case GUS_PORT + 0x8:
        case GUS_PORT + 0x102:
        case GUS_PORT + 0x103:
        case GUS_PORT + 0x104:
            // Fast write, don't set iochrdy by writing 0
            iochrdy_on = false;
            pio_sm_put(pio0, iow_sm, 0x0u);
            break;
        default:
            // Slow write, set iochrdy by writing non-0
            iochrdy_on = true;
            pio_sm_put(pio0, iow_sm, 0xffffffffu);
            break;
        }
        uint32_t value = iow_read & 0xFF;
        // uint32_t write_begin = time_us_32();
        __dsb();
        // printf("%x", iow_read);
        write_gus(port, value);
        // uint32_t write_elapsed = time_us_32() - write_begin;
        // if (write_elapsed > 1) {
        //     printf("long write to port %x, (sel reg %x), took %d us\n", port, gus->selected_register, write_elapsed);
        // }
        // Tell PIO that we are done
        if (iochrdy_on) {
            pio_sm_put(pio0, iow_sm, 0x0u);
        }
        __dsb();
        // printf("GUS IOW: port: %x value: %x\n", port, value);
        // gpio_xor_mask(1u << LED_PIN);
        // puts("IOW");
        // uart_print_hex_u32(port);
        // uart_print_hex_u32(value);
    } else {
        // Reset SM
        pio_sm_put(pio0, iow_sm, 0x0u);
        // gpio_xor_mask(1u << LED_PIN);
    }
#endif // SOUND_GUS
#ifdef SOUND_OPL
    switch (port) {
    case 0x388:
        pio_sm_put(pio0, iow_sm, 0x0u);
        OPL_Pico_PortWrite(OPL_REGISTER_PORT, iow_read & 0xFF);
        // Tell PIO that we are done
        // putchar(iow_read & 0xFF);
        // printf("%x", iow_read);
        break;
    case 0x389:
        pio_sm_put(pio0, iow_sm, 0xffffffffu);
        OPL_Pico_PortWrite(OPL_DATA_PORT, iow_read & 0xFF);
        __dsb();
        // Tell PIO that we are done
        // putchar(iow_read & 0xFF);
        // printf("%x", iow_read);
        pio_sm_put(pio0, iow_sm, 0x0u);
        break;
    default:
        pio_sm_put(pio0, iow_sm, 0x0u);
    }
#endif // SOUND_OPL
#ifdef SOUND_MPU
    switch (port) {
    case 0x330:
        pio_sm_put(pio0, iow_sm, 0xffffffffu);
        // printf("MPU IOW: port: %x value: %x\n", port, iow_read & 0xFF);
        MPU401_WriteData(iow_read & 0xFF);
        // Tell PIO that we are done
        pio_sm_put(pio0, iow_sm, 0x0u);
        break;
    case 0x331:
        pio_sm_put(pio0, iow_sm, 0xffffffffu);
        MPU401_WriteCommand(iow_read & 0xFF);
        // printf("MPU IOW: port: %x value: %x\n", port, iow_read & 0xFF);
        __dsb();
        // Tell PIO that we are done
        pio_sm_put(pio0, iow_sm, 0x0u);
        break;
    default:
        pio_sm_put(pio0, iow_sm, 0x0u);
    }
#endif // SOUND_MPU
}

__force_inline void handle_ior(void) {
    uint32_t ior_read = pio_sm_get(pio0, ior_sm);
    uint16_t port = ior_read & 0x3FF;
    // printf("IOR: %x\n", port);
#if defined(SOUND_GUS)
    if ((port >> 4 | 0x10) == GUS_PORT_TEST) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, ior_sm, 0xffffffffu);
        uint32_t value;
        if (port == GUS_PORT + 0x2) {
            value = 0xdd;
        } else {
            __dsb();
            value = read_gus(port);
        }
        // OR with 0x00ffff00 is required to set pindirs in the PIO
        pio_sm_put(pio0, ior_sm, 0x00ffff00u | value);
        // printf("GUS IOR: port: %x value: %x\n", port, value);
        // gpio_xor_mask(1u << LED_PIN);
    } else {
        // Reset PIO
        pio_sm_put(pio0, ior_sm, 0x0u);
    }
#elif defined(SOUND_OPL)
    if (port == 0x388) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, ior_sm, 0xffffffffu);
        uint32_t value = OPL_Pico_PortRead(OPL_REGISTER_PORT);
        // OR with 0x00ffff00 is required to set pindirs in the PIO
        pio_sm_put(pio0, ior_sm, 0x00ffff00u | value);
    } else {
        // Reset PIO
        pio_sm_put(pio0, ior_sm, 0x0u);
    }
#else
    // Reset PIO
    pio_sm_put(pio0, ior_sm, 0x0u);
#endif
#ifdef SOUND_MPU
    if (port == 0x331) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, ior_sm, 0xffffffffu);
        uint32_t value = MPU401_ReadData();
        // printf("MPU IOR: port: %x value: %x\n", port, value);
        // OR with 0x00ffff00 is required to set pindirs in the PIO
        pio_sm_put(pio0, ior_sm, 0x00ffff00u | value);
    } else {
        // Reset PIO
        pio_sm_put(pio0, ior_sm, 0x0u);
    }
#endif
}

#ifdef USE_IRQ
void iow_isr(void) {
    /* //printf("ints %x\n", pio0->ints0); */
    handle_iow();
    // pio_interrupt_clear(pio0, pio_intr_sm0_rxnempty_lsb);
    irq_clear(PIO0_IRQ_0);
}
void ior_isr(void) {
    handle_ior();
    // pio_interrupt_clear(pio0, PIO_INTR_SM0_RXNEMPTY_LSB);
    irq_clear(PIO0_IRQ_1);
}
#endif

void err_blink(void) {
    for (;;) {
        gpio_xor_mask(1u << LED_PIN);
        busy_wait_ms(100);
    }
}

#ifndef USE_ALARM
#include "pico_pic.h"
#endif

int main()
{
    // Overclock!
    // set_sys_clock_khz(200000, true);
    // Use hacked set_sys_clock_khz to keep SPI clock high - see clock_pll.h for details
    // set_sys_clock_khz(266000, true);
    set_sys_clock_khz(280000, true);
    // vreg_set_voltage(VREG_VOLTAGE_1_20);

    // stdio_init_all();
#ifndef SOUND_MPU
    stdio_async_uart_init_full(UART_ID, BAUD_RATE, UART_TX_PIN, UART_RX_PIN);
#else
    stdio_init_all();
#endif

    puts("PicoGUS booting!");

    io_rw_32 *reset_reason = (io_rw_32 *) (VREG_AND_CHIP_RESET_BASE + VREG_AND_CHIP_RESET_CHIP_RESET_OFFSET);
    if (*reset_reason & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_POR_BITS) {
        puts("I was reset due to power on reset or brownout detection.");
    } else if (*reset_reason & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_RUN_BITS) {
        puts("I was reset due to the RUN pin (either manually or due to ISA RESET signal)");
    } else if(*reset_reason & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_POR_BITS) {
        puts("I was reset due the debug port");
    }

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    // alarm_pool_init_default();
    gpio_init(IRQ_PIN);
    gpio_set_dir(IRQ_PIN, GPIO_OUT);

#ifdef SOUND_MPU
    puts("Initing MIDI UART...");
    uart_init(UART_ID, 31250);
    uart_set_translate_crlf(UART_ID, false);
    uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);
    gpio_set_drive_strength(UART_TX_PIN, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    busy_wait_ms(1000);
    MPU401_Init();
#endif

#ifdef PSRAM
    puts("Initing PSRAM...");
    psram_spi = psram_init();
#if TEST_PSRAM
    puts("Writing PSRAM...");
    uint8_t deadbeef[8] = {0xd, 0xe, 0xa, 0xd, 0xb, 0xe, 0xe, 0xf};
    for (uint32_t addr = 0; addr < (1024 * 1024); ++addr) {
        psram_write8(&psram_spi, addr, (addr & 0xFF));
    }
    puts("Reading PSRAM...");
    uint32_t psram_begin = time_us_32();
    for (uint32_t addr = 0; addr < (1024 * 1024); ++addr) {
        uint8_t result = psram_read8(&psram_spi, addr);
        if (static_cast<uint8_t>((addr & 0xFF)) != result) {
            printf("\nPSRAM failure at address %x (%x != %x)\n", addr, addr & 0xFF, result);
            err_blink();
            return 1;
        }
    }
    uint32_t psram_elapsed = time_us_32() - psram_begin;
    float psram_speed = 1000000.0 * 1024.0 * 1024 / psram_elapsed;
    printf("8 bit: PSRAM read 1MB in %d us, %d B/s (target 705600 B/s)\n", psram_elapsed, (uint32_t)psram_speed);

    psram_begin = time_us_32();
    for (uint32_t addr = 0; addr < (1024 * 1024); addr += 2) {
        uint16_t result = psram_read16(&psram_spi, addr);
        if (static_cast<uint16_t>(
                (((addr + 1) & 0xFF) << 8) |
                (addr & 0XFF)) != result
        ) {
            printf("PSRAM failure at address %x (%x != %x) ", addr, addr & 0xFF, result & 0xFF);
            err_blink();
            return 1;
        }
    }
    psram_elapsed = (time_us_32() - psram_begin);
    psram_speed = 1000000.0 * 1024 * 1024 / psram_elapsed;
    printf("16 bit: PSRAM read 1MB in %d us, %d B/s (target 1411200 B/s)\n", psram_elapsed, (uint32_t)psram_speed);

    psram_begin = time_us_32();
    for (uint32_t addr = 0; addr < (1024 * 1024); addr += 4) {
        uint32_t result = psram_read32(&psram_spi, addr);
        if (static_cast<uint32_t>(
                (((addr + 3) & 0xFF) << 24) |
                (((addr + 2) & 0xFF) << 16) |
                (((addr + 1) & 0xFF) << 8)  |
                (addr & 0XFF)) != result
        ) {
            printf("PSRAM failure at address %x (%x != %x) ", addr, addr & 0xFF, result & 0xFF);
            err_blink();
            return 1;
        }
    }
    psram_elapsed = (time_us_32() - psram_begin);
    psram_speed = 1000000.0 * 1024 * 1024 / psram_elapsed;
    printf("32 bit: PSRAM read 1MB in %d us, %d B/s (target 1411200 B/s)\n", psram_elapsed, (uint32_t)psram_speed);
#endif
#endif

#ifdef SOUND_OPL
    puts("Creating OPL");
    OPL_Pico_Init(0x388);
    multicore_launch_core1(&play_adlib);
#endif

#ifdef SOUND_GUS
    puts("Creating GUS");
    GUS_OnReset(GUS_PORT);
    multicore_launch_core1(&play_gus);
#endif


    for(int i=AD0_PIN; i<(AD0_PIN + 10); ++i) {
        gpio_disable_pulls(i);
    }
    gpio_disable_pulls(IOW_PIN);
    gpio_disable_pulls(IOR_PIN);
    //gpio_disable_pulls(IOCHRDY_PIN);
    // gpio_put(IOCHRDY_PIN, 1);
    // gpio_init(IOCHRDY_PIN);
    gpio_pull_down(IOCHRDY_PIN);
    gpio_set_dir(IOCHRDY_PIN, GPIO_OUT);
    // gpio_put(IOCHRDY_PIN, 1);

    puts("Enabling bus transceivers...");
    // waggle ADS to set BUSOE latch
    gpio_init(ADS_PIN);
    gpio_set_dir(ADS_PIN, GPIO_OUT);
    gpio_put(ADS_PIN, 1);
    busy_wait_ms(10);
    gpio_put(ADS_PIN, 0);

    puts("Starting ISA bus PIO...");
    // gpio_set_drive_strength(ADS_PIN, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(ADS_PIN, GPIO_SLEW_RATE_FAST);

    PIO pio = pio0;

    uint iow_offset = pio_add_program(pio, &iow_program);
    iow_sm = pio_claim_unused_sm(pio, true);
    printf("iow sm: %u\n", iow_sm);

    uint ior_offset = pio_add_program(pio, &ior_program);
    ior_sm = pio_claim_unused_sm(pio, true);
    printf("ior sm: %u\n", ior_sm);

    ior_program_init(pio, ior_sm, ior_offset);
    iow_program_init(pio, iow_sm, iow_offset);

#ifdef USE_IRQ
    puts("Enabling IRQ");
    // iow irq
    irq_set_enabled(PIO0_IRQ_0, false);
    pio_set_irq0_source_enabled(pio0, pis_sm0_rx_fifo_not_empty, true);
    irq_set_priority(PIO0_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_exclusive_handler(PIO0_IRQ_0, iow_isr);
    irq_set_enabled(PIO0_IRQ_0, true);
    // ior irq
    irq_set_enabled(PIO0_IRQ_1, false);
    pio_set_irq1_source_enabled(pio0, pis_sm1_rx_fifo_not_empty, true);
    irq_set_priority(PIO0_IRQ_1, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_exclusive_handler(PIO0_IRQ_1, ior_isr);
    irq_set_enabled(PIO0_IRQ_1, true);
#endif

#ifdef SOUND_GUS
    puts("Initing ISA DMA PIO...");
    dma_config = DMA_init(pio);
#endif

    gpio_xor_mask(1u << LED_PIN);

#ifndef USE_ALARM
    PIC_Init();
#endif

    for (;;) {
#ifndef USE_IRQ
        if (!pio_sm_is_rx_fifo_empty(pio, iow_sm)) {
            handle_iow();
            // gpio_xor_mask(1u << LED_PIN);
        }

        if (!pio_sm_is_rx_fifo_empty(pio, ior_sm)) {
            handle_ior();
            // gpio_xor_mask(1u << LED_PIN);
        }
#endif
#ifndef USE_ALARM
        PIC_HandleEvents();
#endif
#ifdef SOUND_MPU
        send_midi_byte();				// see if we need to send a byte	
#endif
    }
}
