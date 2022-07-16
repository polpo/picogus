#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/spi.h"
#include "hardware/pio.h"
#include "hardware/irq.h"

#include "io.pio.h"

// SPI Defines
// We are going to use SPI 0, and allocate it to the following GPIO pins
// Pins can be changed, see the GPIO function select table in the datasheet for information on GPIO assignments
#define SPI_PORT spi0
#define PIN_MISO 4
#define PIN_CS   5
#define PIN_SCK  2
#define PIN_MOSI 3

#define AD0_PIN  6
#define D0_PIN  18
#define BUSOE_PIN  28

void play_sine(void);

static uint ior_read_sm;
static uint ior_write_sm;

static void isr(void) {
    uint32_t ior_read = pio_sm_get(pio0, ior_read_sm); //>> 16;
    /* printf("IOR: %x\n", ior_read); */
    uint16_t port = (ior_read & 0x3FF);
    uint32_t value;
    switch (port - 0x40) {
        case 0x206:
        case 0x208:
        case 0x20a:
        case 0x302:
        case 0x303:
        case 0x304:
        case 0x305:
        case 0x307:
            /*
            gpio_init(18);
            gpio_set_dir(18, GPIO_OUT);
            gpio_put(18, 0);
            gpio_init(19);
            gpio_set_dir(19, GPIO_OUT);
            gpio_put(19, 0);
            gpio_init(20);
            gpio_set_dir(20, GPIO_OUT);
            gpio_put(20, 0);
            gpio_init(21);
            gpio_set_dir(21, GPIO_OUT);
            gpio_put(21, 0);
            */
            /* value = pThis->gus->ReadFromPort(port + GUS_PORT_BASE, io_width_t::byte); */
            /* value = (ior_read >> 12); */ 
            /* printf("value: %x\n", value); */
            value = 0x0;
            pio_sm_put_blocking(pio0, ior_write_sm, value);
            printf("IOR: port: %x, value: %x\n", port, value);
            break;
        case 0x202:
            value = 0xffffffff;
            pio_sm_put_blocking(pio0, ior_write_sm, value);
            printf("IOR: port: %x, value: %x\n", port, value);
            break;
    }
    pio_interrupt_clear(pio0, 0);
    /* irq_clear(PIO0_IRQ_0); */
}

int main()
{
    // Overclock!
    set_sys_clock_khz(266000, true);

    /*
    // Init GPIOs
    for (int i = 0; i <= 27; i++) {
        gpio_init(i);
        gpio_set_dir(i, GPIO_IN);
        gpio_set_pulls(i, false, false);
    }
    */
    stdio_init_all();

    // SPI initialisation. We'll go nuts and use SPI at 133MHz.
    /*
    spi_init(SPI_PORT, 1000*1000*133);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_CS,   GPIO_FUNC_SIO);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    
    // Chip select is active-low, so we'll initialise it to a driven-high state
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);
    */

    puts("PiGUS-nano booting!");

    /*
    puts("Enabling IRQ");
    const int irq = PIO0_IRQ_0;
    irq_set_enabled(irq, false);
    irq_set_priority(irq, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_exclusive_handler(irq, isr);
    irq_set_enabled(irq, true);
    */

    puts("Starting PIO...");
    PIO pio = pio0;
    uint iow_offset = pio_add_program(pio, &iow_program);
    uint iow_sm = pio_claim_unused_sm(pio, true);
    iow_program_init(pio, iow_sm, iow_offset);
   
    /*
    uint ior_read_offset = pio_add_program(pio, &ior_read_program);
    ior_read_sm = pio_claim_unused_sm(pio, true);
    ior_read_program_init(pio, ior_read_sm, ior_read_offset);
    uint ior_write_offset = pio_add_program(pio, &ior_write_program);
    ior_write_sm = pio_claim_unused_sm(pio, true);
    ior_write_program_init(pio, ior_write_sm, ior_write_offset);
    */

    uint32_t iow_read;
    uint32_t ior_read;
    uint16_t port;
    uint32_t value;
    
    multicore_launch_core1(&play_sine);

    puts("Enabling bus transceivers...");
    gpio_init(BUSOE_PIN);
    gpio_set_dir(BUSOE_PIN, GPIO_OUT);
    gpio_put(BUSOE_PIN, 1);

    for (;;) {
        if (!pio_sm_is_rx_fifo_empty(pio, iow_sm)) {
            /* printf("iowr "); */
            iow_read = pio_sm_get_blocking(pio, iow_sm); //>> 16;
            /* printf("IOW: %x\n", iow_read); */
            port = (iow_read >> 8) & 0x3FF;
            switch (port - 0x40) {
            case 0x200:
            case 0x208:
            case 0x209:
            case 0x20b:
            case 0x302:
            case 0x303:
            case 0x304:
            case 0x305:
            case 0x307:
                value = iow_read & 0xFF;
                printf("IOW: port: %x value: %x\n", port, value);
                break;
            }
        }
        /*
        //printf("IOR: %x\n", ior_read);
        if (!pio_sm_is_rx_fifo_empty(pio, ior_read_sm)) {
            ior_read = pio_sm_get_blocking(pio0, ior_read_sm); //>> 16;
            port = (ior_read & 0x3FF);
            switch (port - 0x40) {
            case 0x206:
            case 0x208:
            case 0x20a:
            case 0x302:
            case 0x303:
            case 0x304:
            case 0x305:
            case 0x307:
                // value = pThis->gus->ReadFromPort(port + GUS_PORT_BASE, io_width_t::byte);
                // value = (ior_read >> 12); 
                // printf("value: %x\n", value);
                value = 0x0;
                pio_sm_put(pio0, ior_write_sm, port);
                printf("IOR: port: %x, value: %x\n", port, value);
                break;
            case 0x202:
                value = 0xff;
                pio_sm_put(pio0, ior_write_sm, value);
                printf("IOR: port: %x, value: %x\n", port, value);
                break;
            }
        }
    */
    }
    return 0;
}
