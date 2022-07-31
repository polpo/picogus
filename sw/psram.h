#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "stdio.h"

// SPI Defines
// We are going to use SPI 0, and allocate it to the following GPIO pins
// Pins can be changed, see the GPIO function select table in the datasheet for information on GPIO assignments
#define SPI_PORT spi0
#define PIN_MISO 4
#define PIN_CS   5
#define PIN_SCK  2
#define PIN_MOSI 3

/**
 * Super basic interface to SPI PSRAMs such as Espressif ESP-PSRAM64, apmemory APS6404L, IPUS IPS6404, Lyontek LY68L6400, etc.
 * NOTE that this abuses type punning to avoid shifts and masks to be as fast as possible!
 */
class Psram {
    public:
        static void init(void) {
            uint8_t reset_en_cmd = 0x66;
            uint8_t reset_cmd = 0x99;

            // SPI initialisation.
            // Let's go nuts and use SPI at 133MHz as that's the max speed of the AP Memory APS6404L!
            uint32_t baudrate = spi_init(SPI_PORT, 1000*1000*133);
            hw_write_masked(&spi_get_hw(spi_default)->cr0, (1 - 1) << SPI_SSPCR0_SCR_LSB, SPI_SSPCR0_SCR_BITS);
            printf("Inited SPI at baud rate %d\n", baudrate);
            gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
            gpio_set_function(PIN_CS,   GPIO_FUNC_SIO);
            gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
            gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
            
            gpio_init(PIN_CS);
            gpio_set_dir(PIN_CS, GPIO_OUT);
            gpio_put(PIN_CS, 1);

            busy_wait_us(150);
            gpio_put(PIN_CS, 0);
            spi_write_blocking(SPI_PORT, &reset_en_cmd, 1);
            gpio_put(PIN_CS, 1);
            busy_wait_us(50);
            gpio_put(PIN_CS, 0);
            spi_write_blocking(SPI_PORT, &reset_cmd, 1);
            gpio_put(PIN_CS, 1);
            busy_wait_us(100);

        };

        inline static void write8(uint32_t addr, uint8_t val) {
            unsigned char* addr_bytes = (unsigned char*)&addr;
            uint8_t command[5] = {
                0x2, // write
                *(addr_bytes + 2),
                *(addr_bytes + 1),
                *addr_bytes,
                val
            };

            // Select RAM chip
            gpio_put(PIN_CS, 0);
            spi_write_blocking(SPI_PORT, command, sizeof(command));
            // Deselect
            gpio_put(PIN_CS, 1);
        };

        inline static uint8_t read8(uint32_t addr) {
            uint8_t val; 
            unsigned char* addr_bytes = (unsigned char*)&addr;
            uint8_t command[4] = {
                0x3, // read
                *(addr_bytes + 2),
                *(addr_bytes + 1),
                *addr_bytes
            };

            // Select RAM chip
            gpio_put(PIN_CS, 0);
            spi_write_blocking(SPI_PORT, command, sizeof(command));
            spi_read_blocking(SPI_PORT, 0, &val, 1);
            // Deselect
            gpio_put(PIN_CS, 1);
            return val;
        };

        inline static void write16(uint32_t addr, uint16_t val) {
            unsigned char* addr_bytes = (unsigned char*)&addr;
            unsigned char* val_bytes = (unsigned char*)&val;
            uint8_t command[6] = {
                0x2, // write
                *(addr_bytes + 2),
                *(addr_bytes + 1),
                *addr_bytes,
                *val_bytes,
                *(val_bytes + 1)
            };

            // Select RAM chip
            gpio_put(PIN_CS, 0);
            spi_write_blocking(SPI_PORT, command, sizeof(command));
            // Deselect
            gpio_put(PIN_CS, 1);
        };

        inline static uint16_t read16(uint32_t addr) {
            uint16_t val; 
            unsigned char* addr_bytes = (unsigned char*)&addr;
            uint8_t command[4] = {
                0x3, // read
                *(addr_bytes + 2),
                *(addr_bytes + 1),
                *addr_bytes
            };

            // Select RAM chip
            gpio_put(PIN_CS, 0);
            spi_write_blocking(SPI_PORT, command, sizeof(command));
            spi_read_blocking(SPI_PORT, 0, (unsigned char*)&val, 2);
            // Deselect
            gpio_put(PIN_CS, 1);
            return val;
        };

        inline static void write32(uint32_t addr, uint32_t val) {
            unsigned char* addr_bytes = (unsigned char*)&addr;
            unsigned char* val_bytes = (unsigned char*)&val;
            // Break the address into three bytes and send read command
            uint8_t command[8] = {
                0x2, // write
                *(addr_bytes + 2),
                *(addr_bytes + 1),
                *addr_bytes,
                *val_bytes,
                *(val_bytes + 1),
                *(val_bytes + 2),
                *(val_bytes + 3)
            };

            // Select RAM chip
            gpio_put(PIN_CS, 0);
            spi_write_blocking(SPI_PORT, command, 5);
            // Deselect
            gpio_put(PIN_CS, 1);
        };

        inline static uint32_t read32(uint32_t addr) {
            uint32_t val;
            unsigned char* addr_bytes = (unsigned char*)&addr;
            uint8_t command[4] = {
                0x3, // read
                *(addr_bytes + 2),
                *(addr_bytes + 1),
                *addr_bytes
            };

            // Select RAM chip
            gpio_put(PIN_CS, 0);
            spi_write_blocking(SPI_PORT, command, 4);
            spi_read_blocking(SPI_PORT, 0, (unsigned char*)&val, 4);
            // Deselect
            gpio_put(PIN_CS, 1);
            return val;
        };
};

