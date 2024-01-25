/*
    8250/16450 UART emulation module, adapted for PicoGUS

    Copyright (c) 2024 Artem Vasilev - wbcbz7

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

#pragma once

#include <stdint.h>
#include "pico/critical_section.h"

#ifdef __cplusplus
extern "C" {
#endif

// inter-core communication FIFO
enum {
    UARTEMU_MSG_FIFO_SIZE           = 16,
    
    // messages (type in [30:24], data in [23:0])
    UARTEMU_MSG_TYPE_MASK           = ((1 << 7) - 1) << 24,
    UARTEMU_MSG_PARM_MASK           = ((1 << 24) - 1),

    // TX (core0->core1)
    UARTEMU_MSG_IRQ_TX_SENT         = (0x01 << 24),
    UARTEMU_MSG_IRQ_TX_SENT_LOOP    = (0x02 << 24),
    UARTEMU_MSG_MODEM_CTRL          = (0x03 << 24),
    UARTEMU_MSG_RX_BUF_EMPTY        = (0x04 << 24),

    // RX (core1->core0)
    UARTEMU_MSG_NEW_RX_INTERVAL     = (0x41 << 24),
};

// inter-core message pipe FIFO
struct uartemu_msg_fifo_t {
    struct {
        uint8_t readpos;
        uint8_t writepos;
    } tx, rx;
    uint32_t txdata[UARTEMU_MSG_FIFO_SIZE];
    uint32_t rxdata[UARTEMU_MSG_FIFO_SIZE];
};

// data buffer descriptor
struct uartemu_databuf_t {
    uint16_t        read_cursor;
    uint16_t        length;
    const uint8_t  *data;
};

// UART callback signatures
typedef void (* uartemu_tx_sent_cb_t)(void *userptr, uint8_t data);
typedef void (* uartemu_modem_ctrl_cb_t)(void *userptr, uint8_t mcr);


// UART emulation structures
struct uart_state_t {
    // emulated resources
    uint16_t iobase, iobase_active;

    // divisor latch
    union {
        struct {
            uint8_t l, h;
        };
        uint16_t w;
    } divisor;

    union {
        uint8_t modemctrl[2];
        struct {
            // modem control lines (damn you checkit!)
            uint8_t dtr     : 1;
            uint8_t rts     : 1;
            uint8_t out1    : 1;
            uint8_t out2    : 1;   // aka interrupt enable

            // loopback control lines
            uint8_t ldtr    : 1;
            uint8_t lrts    : 1;
            uint8_t lout1   : 1;
            uint8_t lout2   : 1;

            // deltas
            uint8_t dcts    : 1;
            uint8_t ddsr    : 1;
            uint8_t dri     : 1;
            uint8_t ddcd    : 1;

            // input
            uint8_t cts     : 1;
            uint8_t dsr     : 1;
            uint8_t ri      : 1;
            uint8_t dcd     : 1;
        };
    };

    // is initialized?
    uint8_t initialized : 1;

    // loopback flag
    uint8_t loopback : 1;

    // interrupt active flag
    uint8_t irq_active : 1;

    // RX/TX 1-byte buffes
    uint8_t rx_buffer;
    uint8_t tx_buffer;

    // 8250 shadow registers
    uint8_t ier;
    uint8_t lcr;
    uint8_t lsr;
    uint8_t scratchpad;

    // ----------------------
    // internal stuff

    // critial section
    critical_section_t crit;

    // inter-core FIFO pipe
    struct uartemu_msg_fifo_t msg;

    // current RX data buffer;
    struct uartemu_databuf_t *rxdata;

    // user callbacks
    uartemu_tx_sent_cb_t    user_tx_sent_cb;
    uartemu_modem_ctrl_cb_t user_modem_ctrl_cb;
    void                    *userptr;

    // delay between bytes sent in us
    int         rx_irq_delay;

    // loopback buffer
    uint8_t     loopback_buffer;

    // interrupt queue
    uint8_t     int_queue;
};

// register flags
enum {
    UARTEMU_IER_RX_DATA_AVAILABLE = (1 << 0),
    UARTEMU_IER_TX_EMPTY          = (1 << 1),
    UARTEMU_IER_RX_LINE_STATUS    = (1 << 2),
    UARTEMU_IER_MODEM_STATUS      = (1 << 3),
};

enum {
    UARTEMU_LCR_DLAB                = (1 << 7),
};

enum {
    UARTEMU_MCR_DTR         = (1 << 0),
    UARTEMU_MCR_RTS         = (1 << 1),
    UARTEMU_MCR_OUT1        = (1 << 2),
    UARTEMU_MCR_OUT2        = (1 << 3), // aka interrupt enable
    UARTEMU_MCR_LOOPBACK    = (1 << 4),
};

enum {
    UARTEMU_LSR_DATA_READY          = (1 << 0),
    UARTEMU_LSR_OVERRUN_ERROR       = (1 << 1),
    UARTEMU_LSR_PARITY_ERROR        = (1 << 2),
    UARTEMU_LSR_FRAMING_ERROR       = (1 << 3),
    UARTEMU_LSR_BREAK_INTERRUPT     = (1 << 4),
    UARTEMU_LSR_TX_REG_EMPTY        = (1 << 5),
    UARTEMU_LSR_TX_EMPTY            = (1 << 6),
};

enum {
    UARTEMU_MSR_DELTA_CTS           = (1 << 0),
    UARTEMU_MSR_DELTA_DSR           = (1 << 1),
    UARTEMU_MSR_TRAILING_RI         = (1 << 2),
    UARTEMU_MSR_DELTA_DCD           = (1 << 3),

    UARTEMU_MSR_CTS                 = (1 << 4),
    UARTEMU_MSR_DSR                 = (1 << 5),
    UARTEMU_MSR_RI                  = (1 << 6),
    UARTEMU_MSR_DCD                 = (1 << 7),
};

enum {
    UARTEMU_INT_QUEUE_MODEM_STATUS      = (1 << 0),
    UARTEMU_INT_QUEUE_TX_EMPTY          = (1 << 1),
    UARTEMU_INT_QUEUE_RX_DATA_AVAILABLE = (1 << 2),
    UARTEMU_INT_QUEUE_RX_LINE_STATUS    = (1 << 3),
};

// --------------------------
// core0 side
uint32_t uartemu_init(int iobase);
uint32_t uartemu_done();

// I/O port traps
void    uartemu_write(uint32_t port, uint32_t data);
uint8_t uartemu_read (uint32_t port);

// -------------------------
// core1 side
void     uartemu_core1_task();

// HACK - may need different approach
void     uartemu_set_dsr(int dsr);

// -------------------------
// these can be called from each core
uint32_t uartemu_set_callbacks(void* userptr, uartemu_tx_sent_cb_t user_tx_sent_cb, uartemu_modem_ctrl_cb_t user_modem_ctrl_cb);
uint8_t  uartemu_get_modem_ctrl();
void     uartemu_set_rxdata_buf(struct uartemu_databuf_t *buf, uint32_t delay_us);
void     uartemu_set_rx_delay(uint32_t delay_us);

#ifdef __cplusplus
}
#endif
