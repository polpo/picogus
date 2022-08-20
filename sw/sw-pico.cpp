#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
// #include "hardware/vreg.h"
#include "hardware/regs/vreg_and_chip_reset.h"


// #include "stdio_async_uart.h"
/*
#define UART_TX_PIN (0)
#define UART_RX_PIN (1)
#define UART_ID     uart0
*/
#define UART_TX_PIN (20)
#define UART_RX_PIN (21)
#define UART_ID     uart1
#define BAUD_RATE   115200

// #include "clock_pll.h"

#ifdef PSRAM
#include "psram_spi.h"
pio_spi_inst_t psram_spi;
#endif
#include "isa_io.pio.h"

#ifdef SOUND_OPL
#include "opl.h"

void play_adlib(void);
extern "C" int OPL_Pico_Init(unsigned int);
extern "C" void OPL_Pico_PortWrite(opl_port_t, unsigned int);
#endif

#ifdef SOUND_GUS
#ifdef DOSBOX_STAGING
#include "gus.h"
Gus* gus;
#else
#include "gus-x.cpp"
#endif

constexpr uint16_t GUS_PORT = 0x240u;
constexpr uint16_t GUS_PORT_TEST = GUS_PORT >> 4 | 0x10;
void play_gus(void);
#endif

constexpr uint LED_PIN = PICO_DEFAULT_LED_PIN;

static uint iow_sm;
static uint ior_sm;
static uint ior_write_sm;
uint32_t iow_read;
uint32_t ior_read;
uint16_t port;
uint32_t value;


__force_inline void handle_iow(void) {
    iow_read = pio_sm_get_blocking(pio0, iow_sm); //>> 16;
    // printf("IOW: %x\n", iow_read);
    port = (iow_read >> 8) & 0x3FF;
#ifdef SOUND_GUS
    if ((port >> 4 | 0x10) == GUS_PORT_TEST) {
        pio_sm_put(pio0, iow_sm, 0xffffffffu);
        value = iow_read & 0xFF;
        // uint32_t write_begin = time_us_32();
#ifdef DOSBOX_STAGING
        gus->WriteToPort(port, value, io_width_t::byte); // 3x4 supports 16-bit transfers but PiGUS doesn't! force byte
#else                                                         
        write_gus(port, value, 1 /* always an 8 bit write */);
#endif
        // uint32_t write_elapsed = time_us_32() - write_begin;
        // if (write_elapsed > 1) {
        //     printf("long write to port %x, (sel reg %x), took %d us\n", port, gus->selected_register, write_elapsed);
        // }
        // Tell PIO that we are done
        pio_sm_put(pio0, iow_sm, 0x0u);
        //printf("GUS IOW: port: %x value: %x\n", port, value);
        // gpio_xor_mask(1u << LED_PIN);
    } else {
        // Reset SM
        pio_sm_put(pio0, iow_sm, 0x0u);
        // gpio_xor_mask(1u << LED_PIN);
    }
#endif // SOUND_GUS
#ifdef SOUND_OPL
    switch (port) {
    case 0x388:
        pio_sm_put(pio0, iow_sm, 0xffffffffu);
        OPL_Pico_PortWrite(OPL_REGISTER_PORT, iow_read & 0xFF);
        // Tell PIO that we are done
        pio_sm_put(pio0, iow_sm, 0x0u);
        break;
    case 0x389:
        pio_sm_put(pio0, iow_sm, 0xffffffffu);
        OPL_Pico_PortWrite(OPL_DATA_PORT, iow_read & 0xFF);
        __dsb();
        // Tell PIO that we are done
        pio_sm_put(pio0, iow_sm, 0x0u);
        break;
    default:
        pio_sm_put(pio0, iow_sm, 0x0u);
    }
#endif // SOUND_OPL
}

__force_inline void handle_ior(void) {
    ior_read = pio_sm_get_blocking(pio0, ior_sm); //>> 16;
    port = ior_read & 0x3FF;
    // printf("IOR: %x\n", port);
#ifdef SOUND_GUS
    if ((port >> 4 | 0x10) == GUS_PORT_TEST) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, ior_sm, 0xffffffffu);
        if (port == 0x242) {
            value = 0xdd;
        } else {
#ifdef DOSBOX_STAGING
            value = gus->ReadFromPort(port, io_width_t::byte);
#else 
            value = read_gus(port, 1);
#endif
        }
        // OR with 0x00ffff00 is required to set pindirs in the PIO
        pio_sm_put(pio0, ior_sm, 0x00ffff00u | value);
        //printf("GUS IOR: port: %x value: %x\n", port, value);
        // gpio_xor_mask(1u << LED_PIN);
    } else {
        // Reset PIO
        pio_sm_put(pio0, ior_sm, 0x0u);
    }
#endif
}

#ifdef USE_IRQ
void isr(void) {
    /* //printf("ints %x\n", pio0->ints0); */
    if (pio0->ints0 & PIO_INTR_SM0_RXNEMPTY_LSB) {
        /* //printf("ints iow %x\n", pio0->ints0); */
        handle_iow();
        pio_interrupt_clear(pio0, PIO_INTR_SM0_RXNEMPTY_LSB);
    }
    if (pio0->ints0 & PIO_INTR_SM1_RXNEMPTY_LSB) {
        /* //printf("ints ior %x\n", pio0->ints0); */
        handle_ior();
        pio_interrupt_clear(pio0, PIO_INTR_SM1_RXNEMPTY_LSB);
    }
    /* irq_clear(PIO0_IRQ_0); */
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
    // gset_sys_clock_khz(266000, true);
    set_sys_clock_khz(270000, true);
    // vreg_set_voltage(VREG_VOLTAGE_1_20);

    stdio_init_all();
    // stdio_async_uart_init_full(UART_ID, BAUD_RATE, UART_TX_PIN, UART_RX_PIN);

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

    alarm_pool_init_default();
    gpio_init(IRQ_PIN);
    gpio_set_dir(IRQ_PIN, GPIO_OUT);

    puts("Starting ISA bus PIO...");
    PIO pio = pio0;

    uint iow_offset = pio_add_program(pio, &iow_program);
    uint iow_sm = pio_claim_unused_sm(pio, true);
    iow_program_init(pio, iow_sm, iow_offset);
   
    uint ior_offset = pio_add_program(pio, &ior_program);
    ior_sm = pio_claim_unused_sm(pio, true);
    ior_program_init(pio, ior_sm, ior_offset);
    /*
    uint ior_write_offset = pio_add_program(pio, &ior_write_program);
    ior_write_sm = pio_claim_unused_sm(pio, true);
    ior_write_program_init(pio, ior_write_sm, ior_write_offset);
    */

    // gpio_set_drive_strength(ADS_PIN, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(ADS_PIN, GPIO_SLEW_RATE_FAST);

#ifdef USE_IRQ
    puts("Enabling IRQ");
    const int irq = PIO0_IRQ_0;
    pio_set_irq0_source_mask_enabled(pio0, PIO_INTR_SM0_RXNEMPTY_LSB | PIO_INTR_SM1_RXNEMPTY_LSB | PIO_INTR_SM2_RXNEMPTY_LSB | PIO_INTR_SM3_RXNEMPTY_LSB, true);
    irq_set_enabled(irq, false);
    irq_set_priority(irq, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_exclusive_handler(irq, isr);
    irq_set_enabled(irq, true);
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
#ifdef DOSBOX_STAGING
    gus = new Gus(GUS_PORT, nullptr, nullptr);
#else
    GUS_OnReset();
#endif
    multicore_launch_core1(&play_gus);
#endif


    for(int i=AD0_PIN; i<(AD0_PIN + 10); ++i) {
        gpio_disable_pulls(i);
    }
    gpio_disable_pulls(IOW_PIN);
    gpio_disable_pulls(IOR_PIN);
    gpio_pull_up(IOCHRDY_PIN);
    gpio_set_dir(IOCHRDY_PIN, GPIO_OUT);
    /* gpio_put(IOCHRDY_PIN, 1); */

    puts("Enabling bus transceivers...");
    gpio_init(BUSOE_PIN);
    gpio_set_dir(BUSOE_PIN, GPIO_OUT);
    gpio_put(BUSOE_PIN, 1);

    gpio_xor_mask(1u << LED_PIN);

    /*
    PIC_Init();
    */

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
    }
}
