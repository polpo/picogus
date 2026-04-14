/*
 *  PicoGUS Debug Printing Framework
 *
 *  Copyright (C) 2024  Ian Scott
 *
 *  Master switch: PGDEBUG (set via CMake: -DPGDEBUG=ON)
 *    When PGDEBUG is not defined, all debug macros compile to ((void)0).
 *
 *  Module-level switches (PGDEBUG_GUS, PGDEBUG_CDROM, etc.) provide
 *  per-module granularity within debug builds.
 *
 *  ERR_PUTS outputs fatal error strings via UART directly, bypassing
 *  printf/stdio entirely. Works even with pico_set_printf_implementation(none).
 */

#pragma once

#include "hardware/uart.h"
#include "hardware/gpio.h"

/* ---- Fatal error output: always active, bypasses printf ---- */
static inline void _pg_err_puts(const char *s) {
    uart_inst_t *uart = uart0;
    if (!uart_is_enabled(uart)) {
        uart_init(uart, PICO_DEFAULT_UART_BAUD_RATE);
        gpio_set_function(PICO_DEFAULT_UART_TX_PIN, GPIO_FUNC_UART);
    }
    uart_puts(uart, s);
    uart_puts(uart, "\r\n");
}
#define ERR_PUTS(s) _pg_err_puts(s)

/* ---- Debug macros: gated on PGDEBUG ---- */
#ifdef PGDEBUG

#include <stdio.h>

#define DBG_PRINTF(fmt, ...)  printf(fmt, ##__VA_ARGS__)
#define DBG_PUTS(s)           puts(s)
#define DBG_PUTCHAR(c)        putchar(c)

#else

#define DBG_PRINTF(fmt, ...)  ((void)0)
#define DBG_PUTS(s)           ((void)0)
#define DBG_PUTCHAR(c)        ((void)0)

#endif /* PGDEBUG */
