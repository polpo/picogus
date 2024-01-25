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

#include <stdio.h>
#include "8250uart.h"
#include "../pico_pic.h"

// ----------------------
// static UART state
uart_state_t uart_state;

// forward declarations
static uint32_t uartemu_event(Bitu val);

// initialize emulation
uint32_t uartemu_init(int iobase) {
    // set emulated resources
    uart_state.iobase = uart_state.iobase_active = iobase;

    // clear UART state
    uart_state.int_queue = 0;
    uart_state.divisor.w = 0;
    uart_state.ier = 0;
    uart_state.lcr = 0;
    uart_state.lsr = UARTEMU_LSR_TX_EMPTY | UARTEMU_LSR_TX_REG_EMPTY;   // indicate TX buffer empty
    uart_state.scratchpad = 0;
    uart_state.rx_buffer = uart_state.tx_buffer = 0;
    uart_state.modemctrl[0] = uart_state.modemctrl[1] = 0;
    uart_state.loopback = uart_state.irq_active = 0;

    // clear message pipes
    uart_state.msg.rx.readpos = uart_state.msg.rx.writepos = 0;
    uart_state.msg.tx.readpos = uart_state.msg.tx.writepos = 0;

    // clear data buffer
    uart_state.rxdata = 0;

    // set default interrupt delay in us
    uart_state.rx_irq_delay = 1000;  // 1ms

    // init critical section
    critical_section_init(&uart_state.crit);

    // set initialized flag
    uart_state.initialized = 1;

    return 1;
}

// deinit emulation
uint32_t uartemu_done() {
    // remove stale events
    PIC_RemoveEvents(uartemu_event);

    // deinit critical section
    critical_section_deinit(&uart_state.crit);

    uart_state.initialized = 0;
    return 1;
}

// ----------------------
// interrupt stuff

void uartemu_irq_raise(bool set_active = true) {
    if (uart_state.out2) PIC_ActivateIRQ();
    if (set_active) uart_state.irq_active = true;
}

void uartemu_irq_drop(bool clear_active = true) {
    PIC_DeActivateIRQ();
    if (clear_active) uart_state.irq_active = false;
}

void uartemu_irq_check() {
    if (uart_state.int_queue == 0) uartemu_irq_drop();
}

// ----------------------
// core1 stuff

// events values
enum {
    UARTEMU_EVENT_TX_SENT = 0,
    UARTEMU_EVENT_TX_LOOPBACK,
    UARTEMU_EVENT_RX_SENT,
    UARTEMU_EVENT_RX_MODEM_STATUS,
};

static void uartemu_rx_post(uint8_t data) {
    // store data in RX buffer
    uart_state.rx_buffer = data;

    // set data ready status
    uart_state.lsr |= UARTEMU_LSR_DATA_READY;
    if (uart_state.ier & UARTEMU_IER_RX_DATA_AVAILABLE) {
        uart_state.int_queue |= UARTEMU_INT_QUEUE_RX_DATA_AVAILABLE;
        uartemu_irq_raise();
    }
}

static void uartemu_rxdata_advance() {
    if (uart_state.rxdata != 0 && uart_state.rxdata->read_cursor < uart_state.rxdata->length) {
        uartemu_rx_post(uart_state.rxdata->data[uart_state.rxdata->read_cursor++]);
    }
}

static uint32_t uartemu_event(Bitu val) {
    // acquire lock
    critical_section_enter_blocking(&uart_state.crit);

    // check the event
    switch(val) {
        case UARTEMU_EVENT_TX_SENT:
            // TX byte sent
            // set THRE/TEMT flags (lightspeed transfers!)
            uart_state.lsr |= UARTEMU_LSR_TX_EMPTY | UARTEMU_LSR_TX_REG_EMPTY;

            // post TX empty interrupt
            if (uart_state.ier & UARTEMU_IER_TX_EMPTY) {
                uart_state.int_queue |= UARTEMU_INT_QUEUE_TX_EMPTY;
                uartemu_irq_raise();
            }

            // call callback if requested
            if (uart_state.user_tx_sent_cb) {
                uart_state.user_tx_sent_cb(uart_state.userptr, uart_state.tx_buffer);
            }
            break;

        case UARTEMU_EVENT_TX_LOOPBACK:
            // transmit loopback
            uartemu_rx_post(uart_state.loopback_buffer);
            break;
        
        case UARTEMU_EVENT_RX_SENT:
            // RX byte sent, fetch next byte from RX data buffer if available
            uartemu_rxdata_advance();
            break;

        default:
            break;
    }

    // release lock
    critical_section_exit(&uart_state.crit);

    // kill event
    return 0;
}

static void uartemu_post_event(uint32_t type, uint32_t delay_us) {
    // add event
    PIC_AddEvent(uartemu_event, delay_us, type);
}

// TX message pipe drain
static void uartemu_tx_msg_handle() {
    while (uart_state.msg.tx.readpos != uart_state.msg.tx.writepos) {
        switch (uart_state.msg.txdata[uart_state.msg.tx.readpos] & UARTEMU_MSG_TYPE_MASK) {
            case UARTEMU_MSG_IRQ_TX_SENT:
                uartemu_post_event(UARTEMU_EVENT_TX_SENT, uart_state.msg.txdata[uart_state.msg.tx.readpos] & UARTEMU_MSG_PARM_MASK);
                break;
            
            case UARTEMU_MSG_IRQ_TX_SENT_LOOP:
                uartemu_post_event(UARTEMU_EVENT_TX_LOOPBACK, uart_state.msg.txdata[uart_state.msg.tx.readpos] & UARTEMU_MSG_PARM_MASK);
                break;

            case UARTEMU_MSG_RX_BUF_EMPTY:
                uartemu_post_event(UARTEMU_EVENT_RX_SENT, uart_state.msg.txdata[uart_state.msg.tx.readpos] & UARTEMU_MSG_PARM_MASK);
                break;

            case UARTEMU_MSG_MODEM_CTRL:
                // call callback if requested
                if (uart_state.user_modem_ctrl_cb) {
                    uart_state.user_modem_ctrl_cb(uart_state.userptr, uart_state.msg.txdata[uart_state.msg.tx.readpos] & 3);
                }
                break;

            default:
                break;
        }
        uart_state.msg.tx.readpos = (uart_state.msg.tx.readpos + 1) & (UARTEMU_MSG_FIFO_SIZE - 1);
    }
}

void uartemu_set_dsr(int dsr) {
    if (uart_state.dsr != dsr) {
        uart_state.ddsr = 1;

        // signal modem status interrupt
        if (uart_state.ier & UARTEMU_IER_MODEM_STATUS) {
            uart_state.int_queue |= UARTEMU_INT_QUEUE_MODEM_STATUS;
            uartemu_irq_raise();
        }
    }
    uart_state.dsr = dsr;
}

// core1 task
void uartemu_core1_task() {
    // drain TX message pipe
    uartemu_tx_msg_handle();
}

// -------------------------------
// core0 interface

// post new message for the core1
static void uartemu_post_tx_msg(uint32_t msg) {
    uart_state.msg.txdata[uart_state.msg.tx.writepos] = msg;
    uart_state.msg.tx.writepos = (uart_state.msg.tx.writepos + 1) & (UARTEMU_MSG_FIFO_SIZE - 1);
}

// write new byte to the UART
static void uartemu_tx(uint32_t data) {
    // save data to the buffer
    uart_state.tx_buffer = data & 0xFF;

    // clear TX empty interrupt
    uart_state.int_queue &= ~UARTEMU_INT_QUEUE_TX_EMPTY;
    uartemu_irq_check();

    // clear TX empty flags
    uart_state.lsr &= ~(UARTEMU_LSR_TX_EMPTY | UARTEMU_LSR_TX_REG_EMPTY);

    // post TX sent event
    uartemu_post_tx_msg(UARTEMU_MSG_IRQ_TX_SENT | uart_state.rx_irq_delay);

    // is this loopback?
    if (uart_state.loopback) {
        // store data in loopback buffer
        uart_state.loopback_buffer = uart_state.tx_buffer;

        // post TX loopback event
        uartemu_post_tx_msg(UARTEMU_MSG_IRQ_TX_SENT_LOOP | (uart_state.rx_irq_delay * 2));
    }
}

// read new byte from UART
static uint32_t uartemu_rx() {
    uint32_t rtn = -1;

    // clear pending interrupt
    uart_state.int_queue &= ~UARTEMU_INT_QUEUE_RX_DATA_AVAILABLE;
    uartemu_irq_check();

    // clear data ready in LSR
    if (uart_state.lsr &   UARTEMU_LSR_DATA_READY) {
        uart_state.lsr &= ~UARTEMU_LSR_DATA_READY;
        
        // post RX empty message
        if (uart_state.loopback == 0) {
            uartemu_post_tx_msg(UARTEMU_MSG_RX_BUF_EMPTY | uart_state.rx_irq_delay);
        }
    }

    // feed the data
    return uart_state.rx_buffer;
}

// ------------------------------

static void uartemu_ier_write(uint32_t data) {
    uart_state.ier = data & 0x0F;

    // check if TX buffer is empty and TX empty interrupt is enabled
    // if so, raise interrupt right now
    if ((data & UARTEMU_IER_TX_EMPTY) && (uart_state.lsr & UARTEMU_LSR_TX_REG_EMPTY)) {
        uart_state.int_queue |= UARTEMU_INT_QUEUE_TX_EMPTY;
        uartemu_irq_raise();
    }
}

// ------------------------------
// updated and fixed logic from dosbox-x 

static uint8_t uartemu_mcr_read() {
    if (uart_state.loopback) {
        return
            (uart_state.ldtr  ? UARTEMU_MCR_DTR  : 0) | 
            (uart_state.lrts  ? UARTEMU_MCR_RTS  : 0) | 
            (uart_state.lout1 ? UARTEMU_MCR_OUT1 : 0) | 
            (uart_state.lout2 ? UARTEMU_MCR_OUT2 : 0) | 
            (uart_state.loopback ? UARTEMU_MCR_LOOPBACK : 0);
    } else {
        return
            (uart_state.dtr  ? UARTEMU_MCR_DTR  : 0) | 
            (uart_state.rts  ? UARTEMU_MCR_RTS  : 0) | 
            (uart_state.out1 ? UARTEMU_MCR_OUT1 : 0) | 
            (uart_state.out2 ? UARTEMU_MCR_OUT2 : 0) | 
            (uart_state.loopback ? UARTEMU_MCR_LOOPBACK : 0);
    }
}

static void uartemu_mcr_write(uint32_t data) {
    // process new MCR bits
    uint8_t new_dtr  = data & UARTEMU_MCR_DTR      ? 1 : 0;
    uint8_t new_rts  = data & UARTEMU_MCR_RTS      ? 1 : 0;
    uint8_t new_out1 = data & UARTEMU_MCR_OUT1     ? 1 : 0;
    uint8_t new_out2 = data & UARTEMU_MCR_OUT2     ? 1 : 0;
    uint8_t new_loop = data & UARTEMU_MCR_LOOPBACK ? 1 : 0;

    // process loopback
    if (new_loop) {
        uint8_t deltaflags = 0;

        // update loopback bits
        if ((uart_state.ldtr != new_dtr) && (uart_state.ddsr == 0)) {
            uart_state.ddsr = 1;
            deltaflags |= UARTEMU_MSR_DELTA_DSR;
        }
        if ((uart_state.lrts != new_rts) && (uart_state.dcts == 0)) {
            uart_state.dcts = 1;
            deltaflags |= UARTEMU_MSR_DELTA_CTS;
        }
        if ((new_out1 == 0) && (uart_state.lout1 != new_out1) && (uart_state.dri == 0)) {
            uart_state.dri = 1;
            deltaflags |= UARTEMU_MSR_TRAILING_RI;
        }
        if ((uart_state.lout2 != new_out2) && (uart_state.ddcd == 0)) {
            uart_state.ddcd = 1;
            deltaflags |= UARTEMU_MSR_DELTA_DCD;
        }

        // post modem status change
        if ((deltaflags != 0) && (uart_state.ier & UARTEMU_IER_MODEM_STATUS)) {
            uart_state.int_queue |= UARTEMU_INT_QUEUE_MODEM_STATUS;
            uartemu_irq_raise();
        }

        // save loopback info
        uart_state.ldtr = new_dtr;
        uart_state.lrts = new_rts;
        uart_state.lout1 = new_out1;
        uart_state.lout2 = new_out2;

        // and force control outputs to 0
        new_dtr = new_rts = new_out1 = new_out2 = 0;
    };

    // interrupt enable on OUT2 switch logic
    if (uart_state.out2 == 0 && new_out2 == 1 && uart_state.irq_active) {
        uartemu_irq_raise(false);
    } else
    if (uart_state.out2 == 1 && new_out2 == 0 && uart_state.irq_active) {
        uartemu_irq_drop(false);
    }

    // set new flags
    uart_state.dtr  = new_dtr;
    uart_state.rts  = new_rts;
    uart_state.out1 = new_out1;
    uart_state.out2 = new_out2;
    uart_state.loopback = new_loop;

    // at last, post modem ctrl message
    if (uart_state.loopback == 0) {
        uartemu_post_tx_msg(UARTEMU_MSG_MODEM_CTRL | (uart_state.modemctrl[0] & 0x3));
    }
}

// ----------------------------------
static uint32_t uartemu_isr_read() {
    if (uart_state.int_queue != 0) {
        // resolve interrupt priority
        if (uart_state.int_queue & UARTEMU_INT_QUEUE_RX_LINE_STATUS) return 6; else
        if (uart_state.int_queue & UARTEMU_INT_QUEUE_RX_DATA_AVAILABLE) return 4; else
        if (uart_state.int_queue & UARTEMU_INT_QUEUE_TX_EMPTY) {
            // special case - reading TX empty interrupt ID clears the interrupt
            uart_state.int_queue &= ~UARTEMU_INT_QUEUE_TX_EMPTY;
            uartemu_irq_check();
            return 2;
        } else
        if (uart_state.int_queue & UARTEMU_INT_QUEUE_MODEM_STATUS) return 0;
    } else return 1;    // no interrupts pending

    return 1;
}

static uint32_t uartemu_lsr_read() {
    uint32_t rtn = uart_state.lsr;

    // clear error bits
    uart_state.lsr &= ~(UARTEMU_LSR_OVERRUN_ERROR|UARTEMU_LSR_PARITY_ERROR|UARTEMU_LSR_FRAMING_ERROR|UARTEMU_LSR_BREAK_INTERRUPT);

    // clear interrupt
    uart_state.int_queue &= ~UARTEMU_INT_QUEUE_RX_LINE_STATUS;
    uartemu_irq_check();

    return rtn;
}

// ------------------------------

static void uartemu_msr_write(uint32_t data) {
    // update delta flags only
    uart_state.dcts = data & UARTEMU_MSR_DELTA_CTS      ? 1 : 0;
    uart_state.ddsr = data & UARTEMU_MSR_DELTA_DSR      ? 1 : 0;
    uart_state.dri  = data & UARTEMU_MSR_TRAILING_RI    ? 1 : 0;
    uart_state.ddcd = data & UARTEMU_MSR_DELTA_DCD      ? 1 : 0;
}

static uint32_t uartemu_msr_read() {
    uint32_t rtn = 0;

    if (uart_state.loopback) {
        // feed loopback values
        rtn = 
            (uart_state.lrts  ? UARTEMU_MSR_CTS  : 0) | 
            (uart_state.ldtr  ? UARTEMU_MSR_DSR  : 0) | 
            (uart_state.lout1 ? UARTEMU_MSR_RI   : 0) | 
            (uart_state.lout2 ? UARTEMU_MSR_DCD  : 0);
    } else {
        rtn = 
            (uart_state.cts  ? UARTEMU_MSR_CTS  : 0) | 
            (uart_state.dsr  ? UARTEMU_MSR_DSR  : 0) | 
            (uart_state.ri   ? UARTEMU_MSR_RI   : 0) | 
            (uart_state.dcd  ? UARTEMU_MSR_DCD  : 0);
    }
    
    // add delta flags
    rtn |= 
        (uart_state.dcts  ? UARTEMU_MSR_DELTA_CTS   : 0) | 
        (uart_state.ddsr  ? UARTEMU_MSR_DELTA_DSR   : 0) | 
        (uart_state.dri   ? UARTEMU_MSR_TRAILING_RI : 0) | 
        (uart_state.ddcd  ? UARTEMU_MSR_DELTA_DCD   : 0);

    // clear delta flags
    uart_state.dcts = uart_state.ddsr = uart_state.dri = uart_state.ddcd = 0;

    // clear interrupt
    uart_state.int_queue &= ~UARTEMU_INT_QUEUE_MODEM_STATUS;
    uartemu_irq_check();

    return rtn;
}

// RX message pipe drain
static void uartemu_rx_msg_handle() {
    while (uart_state.msg.rx.readpos != uart_state.msg.rx.writepos) {
        switch (uart_state.msg.rxdata[uart_state.msg.rx.readpos] & UARTEMU_MSG_TYPE_MASK) {
            case UARTEMU_MSG_NEW_RX_INTERVAL:
                uart_state.rx_irq_delay = uart_state.msg.rxdata[uart_state.msg.rx.readpos] & UARTEMU_MSG_PARM_MASK;
                break;
            default:
                break;
        }
        uart_state.msg.rx.readpos = (uart_state.msg.rx.readpos + 1) & (UARTEMU_MSG_FIFO_SIZE - 1);
    }
}

// UART write trap
void uartemu_write(uint32_t port, uint32_t data) {
    // acquire lock
    // TODO: must NOT lock for more than 15us!
    critical_section_enter_blocking(&uart_state.crit);

    // drain RX message pipe
    uartemu_rx_msg_handle();

    // read port
    switch (port & 7) {
        case 0:     // TX Buffer / Divisor LSB
            if (uart_state.lcr & UARTEMU_LCR_DLAB) {
                uart_state.divisor.l = data & 0xFF;
            } else {
                uartemu_tx(data);
            }
            break;

        case 1:     // Interrupt Enable / Divisor MSB
            if (uart_state.lcr & UARTEMU_LCR_DLAB) {
                uart_state.divisor.h = data & 0xFF;
            } else {
                uartemu_ier_write(data);
            }
            break;

        case 2:     // 16550+ - FIFO Control - ignore
            break;

        case 3:     // Line Control
            uart_state.lcr = data & 0xFF;
            break;

        case 4:     // Modem Control
            uartemu_mcr_write(data);
            break;

        case 5:     // Line Status - ignore, "for factory testing"
            break;

        case 6:     // Modem Status
            uartemu_msr_write(data);
            break;
        
        case 7:     // Scratchpad
            uart_state.scratchpad = data & 0xFF;
            break;

        default: break;
    }

    // release the lock
    critical_section_exit(&uart_state.crit);
}

// UART read trap
uint8_t uartemu_read (uint32_t port) {
    uint8_t rtn = 0xFF;

    // acquire lock
    // TODO: must NOT block for more than 15us!
    critical_section_enter_blocking(&uart_state.crit);

    // drain RX message pipe
    uartemu_rx_msg_handle();

    switch(port & 7) {
        case 0: // RX Buffer / Divisor LSB
            rtn = (uart_state.lcr & UARTEMU_LCR_DLAB) ? uart_state.divisor.l : uartemu_rx();
            break;

        case 1: // Interrupt Enable / Divisor MSB
            rtn = (uart_state.lcr & UARTEMU_LCR_DLAB) ? uart_state.divisor.h : uart_state.ier;
            break;

        case 2: // Interrupt Identification
            rtn = uartemu_isr_read();
            break;

        case 3: // Line Control
            rtn = uart_state.lcr;
            break;

        case 4: // Modem Control
            rtn = uartemu_mcr_read();
            break;

        case 5: // Line Status
            rtn = uartemu_lsr_read();
            break;

        case 6: // Modem Status
            rtn = uartemu_msr_read();
            break;

        case 7: // Scratchpad
            rtn = uart_state.scratchpad;
            break;

        default:
            rtn = 0xFF;
            break;
    }

    // release the lock
    critical_section_exit(&uart_state.crit);

    // at last, return the data
    return rtn;
}

// -------------------------
// these can be called from each core
uint32_t uartemu_set_callbacks(void* userptr, uartemu_tx_sent_cb_t user_tx_sent_cb, uartemu_modem_ctrl_cb_t user_modem_ctrl_cb) {
    // acquire lock
    critical_section_enter_blocking(&uart_state.crit);

    // set callbacks
    uart_state.user_tx_sent_cb = user_tx_sent_cb;
    uart_state.user_modem_ctrl_cb = user_modem_ctrl_cb;
    uart_state.userptr = userptr;

    // release lock
    critical_section_exit(&uart_state.crit);

    return 1;
}

uint8_t uartemu_get_modem_ctrl() { return uart_state.modemctrl[0] & 0xF; }

void uartemu_set_rxdata_buf(struct uartemu_databuf_t *buf, uint32_t delay_us) {
    // acquire lock
    critical_section_enter_blocking(&uart_state.crit);

    // set new buffer
    uart_state.rxdata = buf;

    if (delay_us == 0) {
        // send first byte receive interrupt right now
        uartemu_rxdata_advance();
    } else {
        // reset interrupt flags
        uart_state.int_queue &= ~UARTEMU_INT_QUEUE_RX_DATA_AVAILABLE;
        uartemu_irq_check();

        if (uart_state.lsr &   UARTEMU_LSR_DATA_READY) {
            uart_state.lsr &= ~UARTEMU_LSR_DATA_READY;
        }

        // schedule first byte receive after [delay_us] microseconds
        uartemu_post_tx_msg(UARTEMU_MSG_RX_BUF_EMPTY | delay_us);
    }

    // release lock
    critical_section_exit(&uart_state.crit);
};

void uartemu_set_rx_delay(uint32_t delay_us) {
    uart_state.rx_irq_delay = delay_us;
};

