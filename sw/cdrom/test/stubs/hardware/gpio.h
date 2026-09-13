/* Host stand-in for the pico-sdk header that sw/include/pg_debug.h includes. */
#pragma once

#define PICO_DEFAULT_UART_TX_PIN 0
#define GPIO_FUNC_UART 2

static inline void gpio_set_function(unsigned gpio, unsigned fn) { (void)gpio; (void)fn; }
