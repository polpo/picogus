/* Host stand-in for the pico-sdk header that sw/include/pg_debug.h includes. */
#pragma once
#include <stdbool.h>

typedef struct uart_inst uart_inst_t;
#define uart0 ((uart_inst_t *)0)
#define PICO_DEFAULT_UART_BAUD_RATE 115200

static inline bool uart_is_enabled(uart_inst_t *uart) { (void)uart; return true; }
static inline void uart_init(uart_inst_t *uart, unsigned baud) { (void)uart; (void)baud; }
static inline void uart_puts(uart_inst_t *uart, const char *s) { (void)uart; (void)s; }
