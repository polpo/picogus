/*
    Serial Mouse emulation module, adapted for PicoGUS

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
#include "sermouse.h"
#include "8250uart.h"

#include "tusb.h"

#define SERMOUSE_CLAMP(a, l, h) (((a) > (l)) ? (((a) < (h)) ? (a) : (h)) : (l))

// ----------------------
// static mouse state
static sermouse_state_t mouse_state;

// forward declarations
void sermouse_rts_callback(void *userptr, uint8_t data);

// --------------------------
// id strings

// Microsoft Mouse (2 buttons, no wheel)
static uint8_t sermouse_id_microsoft[] = { 'M' };

// Logitech (modified Microsoft, 3 buttons, no wheel)
static uint8_t sermouse_id_logitech[] = {
    'M', '3',           // classic Microsoft Mouse ID + 3-button support flag
};

// IntelliMouse (even more modified Microsoft, 3 buttons + wheel)
static uint8_t sermouse_id_intellimouse[] = {
    // more complex one, as we have to include PnP string for reliable detect
    'M', 'Z',           // classic Microsoft Mouse ID + wheel support flag
    0x40, 0, 0, 0,      // dummy packet for old mouse drivers

    // here goes PnP data
    0x08, 0x01, 0x24, 0x2D, 0x33, 0x28, 0x10, 0x10,
    0x10, 0x11, 0x3C, 0x10, 0x10, 0x10, 0x14, 0x10,
    0x12, 0x10, 0x10, 0x3C, 0x2D, 0x2F, 0x35, 0x33,
    0x25, 0x3C, 0x30, 0x2E, 0x30, 0x10, 0x26, 0x10,
    0x21, 0x3C, 0x2D, 0x29, 0x23, 0x32, 0x2F, 0x33,
    0x2F, 0x26, 0x34, 0x00, 0x29, 0x2E, 0x34, 0x25,
    0x2C, 0x2C, 0x29, 0x2D, 0x2F, 0x35, 0x33, 0x25,
    0x00, 0x0D, 0x00, 0x33, 0x25, 0x32, 0x29, 0x21,
    0x2C, 0x00, 0x36, 0x25, 0x32, 0x33, 0x29, 0x2F,
    0x2E, 0x15, 0x16, 0x09,
};

// Mouse Systems (there is no actual ID string, so we'll send empty packet as one)
static uint8_t sermouse_id_mousesystems[] = { 0x87, 0, 0, 0, 0 };

// protocol ID list
static struct {
    const uint8_t *data;
    uint16_t       length;
    uint16_t       max_bytes_per_packet;
} mouse_protocol_info[SERMOUSE_PROTOCOL_LAST] = {
    {sermouse_id_microsoft,     sizeof(sermouse_id_microsoft),    3},
    {sermouse_id_logitech,      sizeof(sermouse_id_logitech),     4},
    {sermouse_id_intellimouse,  sizeof(sermouse_id_intellimouse), 4},
    {sermouse_id_mousesystems,  sizeof(sermouse_id_mousesystems), 5},
};

// push ID string to the UART buffer
static void sermouse_send_id() {
    mouse_state.idbuf.data   = mouse_protocol_info[mouse_state.protocol].data;
    mouse_state.idbuf.length = mouse_protocol_info[mouse_state.protocol].length;
    mouse_state.idbuf.read_cursor = 0;

    // add 1ms delay for better detection
    uartemu_set_rxdata_buf(&mouse_state.idbuf, 1000);
}

// --------------------------

// inititalization
uint32_t sermouse_init(uint8_t protocol, uint8_t report_rate_hz, int16_t sensitivity) {
    // clear mouse state
    mouse_state.state = SERMOUSE_STATE_RESET;
    mouse_state.next_state = SERMOUSE_STATE_NULL;
    mouse_state.modem_control = 0;
    mouse_state.current_buf  = 0;
    mouse_state.buttons_prev = 0;
    mouse_state.last_pkt_timestamp_us = time_us_32();
    mouse_state.report_rate_hz = 0;
    mouse_state.report_interval_us = 0;
    mouse_state.rx_interval_us = 0;

    // set internal values
    sermouse_set_protocol(protocol);
    sermouse_set_report_rate_hz(report_rate_hz);
    sermouse_set_sensitivity(sensitivity);

    // set initialized flag
    mouse_state.initialized = 1;

    return 1;
}

// attach UART
uint32_t sermouse_attach_uart() {
    uartemu_set_callbacks(0, 0, sermouse_rts_callback);
    return 1;
}

// deinitialization
uint32_t sermouse_done() {
    mouse_state.initialized = 0;

    return 1;
}

// -----------------------
// TinyUSB HID Boot Protocol mouse report callback
// TODO: most mice do not support wheel in Boot Protocol report (it is optional there)
// TODO: some mice erroneously send Report ID in Boot Protocol reports
void sermouse_process_report(hid_mouse_report_t const * report) {
    if ((mouse_state.initialized == 0) || (mouse_state.state != SERMOUSE_STATE_RUN)) return;

    // get current packet index
    uint8_t current_packet_idx = mouse_state.current_buf;
    struct sermouse_packet_t *pkt = mouse_state.pkt + current_packet_idx;

    // accumulate motion data
    pkt->x += report->x;
    pkt->y += report->y;
    pkt->z += report->wheel;

    // replace button data
    mouse_state.buttons = report->buttons;
}

// -------------------------
// protocol handlers

static void sermouse_format_microsoft(struct sermouse_packet_t *pkt, uint8_t buttons, uint8_t buttons_prev) {  
    // 1st byte - 0x40 + L/R buttons + X/Y bits [7:6]
    pkt->data[0] = 0x40 |
        (buttons & MOUSE_BUTTON_LEFT  ? 0x20 : 0) |
        (buttons & MOUSE_BUTTON_RIGHT ? 0x10 : 0) |
        (((uint8_t)(pkt->y & 0xC0)) >> 4) | (((uint8_t)(pkt->x & 0xC0)) >> 6);
    // 2nd byte - X bits [5:0]
    pkt->data[1] = (uint8_t)(pkt->x & 0x3F);
    // 3rd byte - Y bits [5:0]
    pkt->data[2] = (uint8_t)(pkt->y & 0x3F);
    pkt->databuf.length = 3;
}

static void sermouse_format_logitech(struct sermouse_packet_t *pkt, uint8_t buttons, uint8_t buttons_prev) {
    // first 3 bytes are same as in Microsoft format
    sermouse_format_microsoft(pkt, buttons, buttons_prev);

    // if middle button is pressed or changed its state, append 4th byte
    if ((buttons | buttons_prev) & MOUSE_BUTTON_MIDDLE) {
        // 4th byte - M button
        pkt->data[3] = (buttons & MOUSE_BUTTON_MIDDLE) ? 0x20 : 0;
        pkt->databuf.length = 4;
    }
}

static void sermouse_format_intellimouse(struct sermouse_packet_t *pkt, uint8_t buttons, uint8_t buttons_prev) {
    // first 3 bytes are same as in Microsoft format
    sermouse_format_microsoft(pkt, buttons, buttons_prev);

    // 4th byte: M button + wheel (inverted?)
    pkt->data[3] = ((uint8_t)(-pkt->z & 0xF)) | ((buttons & MOUSE_BUTTON_MIDDLE) ? 0x10 : 0);
    pkt->databuf.length = 4;
}

static void sermouse_format_mousesystems(struct sermouse_packet_t *pkt, uint8_t buttons, uint8_t buttons_prev) {
    // 1st byte - 0x80 + inverted L/M/R buttons
    pkt->data[0] = 0x80 |
        (buttons & MOUSE_BUTTON_LEFT   ? 0 : 4) |
        (buttons & MOUSE_BUTTON_MIDDLE ? 0 : 2) |
        (buttons & MOUSE_BUTTON_RIGHT  ? 0 : 1);

    // 2nd byte - X bits [7:0]
    pkt->data[1] = (uint8_t)(pkt->x & 0xFF);

    // 3rd byte - Y bits [7:0], inverted axis
    pkt->data[2] = (uint8_t)(-pkt->y & 0xFF);

    // 4th and 5th bytes are zeroed (for real Mouse Systems, they are X/Y delta since bytes 2/3 were sent)
    pkt->data[3] = pkt->data[4] = 0;
    pkt->databuf.length = 5;
}

// send new mouse report
static void sermouse_send_report() {
    // get current packet index
    uint8_t current_packet_idx = mouse_state.current_buf;
    struct sermouse_packet_t *pkt = mouse_state.pkt + current_packet_idx;

    // if there are no state changes, do not sent the packet
    if (pkt->x == 0 && pkt->y == 0 && pkt->z == 0 && mouse_state.buttons == mouse_state.buttons_prev)
        return;

    // flip packet index for next reports
    mouse_state.current_buf ^= 1;

    // init data buffer
    pkt->databuf.data = pkt->data;
    pkt->databuf.read_cursor = 0;

    // rescale and clamp X/Y axis values according by sensitivity
    pkt->x = (pkt->x * mouse_state.sensitivity) >> 8;
    pkt->y = (pkt->y * mouse_state.sensitivity) >> 8;
    pkt->x = SERMOUSE_CLAMP(pkt->x, -127, 127);
    pkt->y = SERMOUSE_CLAMP(pkt->y, -127, 127);

    // meanwhile, format current report
    switch(mouse_state.protocol) {
        case SERMOUSE_PROTOCOL_MICROSOFT:
        default:
            sermouse_format_microsoft(pkt, mouse_state.buttons, mouse_state.buttons_prev);
            break;
        case SERMOUSE_PROTOCOL_LOGITECH:
            sermouse_format_logitech(pkt, mouse_state.buttons, mouse_state.buttons_prev);
            break;
        case SERMOUSE_PROTOCOL_INTELLIMOUSE:
            sermouse_format_intellimouse(pkt, mouse_state.buttons, mouse_state.buttons_prev);
            break;
        case SERMOUSE_PROTOCOL_MOUSESYSTEMS:
            sermouse_format_mousesystems(pkt, mouse_state.buttons, mouse_state.buttons_prev);
            break;
    }

    // send it to the UART emulation
    uartemu_set_rxdata_buf(&pkt->databuf, 0);

    // clear motion data
    pkt->x = pkt->y = pkt->z = 0;

    // and update buttons status
    mouse_state.buttons_prev = mouse_state.buttons;
}

// recalculate RX interval based on packet interval and maximum packet size in bytes
static void sermouse_recalc_rx_interval() {
    if ((mouse_state.report_interval_us > 0) && (mouse_state.max_bytes_per_packet > 0)) {
        mouse_state.rx_interval_us = mouse_state.report_interval_us / mouse_state.max_bytes_per_packet;
        // notify UART state
        uartemu_set_rx_delay(mouse_state.rx_interval_us);
    }
}

// -----------------------------
// setters/getters
void sermouse_set_protocol(uint8_t protocol) {
    if (protocol < SERMOUSE_PROTOCOL_LAST) {
        // set new protocol and put mouse on reset
        mouse_state.protocol   = protocol;
        mouse_state.next_state = SERMOUSE_STATE_RESET; 
        mouse_state.max_bytes_per_packet = mouse_protocol_info[mouse_state.protocol].max_bytes_per_packet;

        // since bytes per packet may differ between protocols, we have to recalculate rx interval
        sermouse_recalc_rx_interval();
    }
}

uint8_t sermouse_get_protocol() {
    return mouse_state.protocol;
}

void sermouse_set_report_rate_hz(uint8_t rate_hz) {
    mouse_state.report_rate_hz     = SERMOUSE_CLAMP(rate_hz, SERMOUSE_REPORTRATE_MIN, SERMOUSE_REPORTRATE_MAX);
    mouse_state.report_interval_us = (1000000 / rate_hz);
    sermouse_recalc_rx_interval();
}

uint8_t sermouse_get_report_rate_hz() {
    return mouse_state.report_rate_hz;
}

void sermouse_set_sensitivity(int16_t sensitivity) {
    mouse_state.sensitivity = sensitivity;
}

int16_t sermouse_get_sensitivity() {
    return mouse_state.sensitivity;
}

// ------------------------------
// modem status callback
void sermouse_rts_callback(void *userptr, uint8_t data) {
    // echo DTR to DSR
    uartemu_set_dsr(data & UARTEMU_MCR_DTR ? 1 : 0);
    
    // check RTS pin change
    if ((mouse_state.modem_control ^ data) & UARTEMU_MCR_RTS) {
        if (data & UARTEMU_MCR_RTS) {
            // RTS 0->1 - pull out of reset and prepare for identification
            mouse_state.next_state = SERMOUSE_STATE_ID;
        } else {
            // RTS 1->0 - go to reset
            mouse_state.next_state = SERMOUSE_STATE_RESET;
        }
    }
    mouse_state.modem_control = data;
}

// -------------------------------
// core 1 task!
void sermouse_core1_task() {
    // process state change
    if (mouse_state.next_state != SERMOUSE_STATE_NULL) {
        switch (mouse_state.next_state) {
            case SERMOUSE_STATE_ID:
                // push ID string
                sermouse_send_id();
                break;

            default: break;
        }
        mouse_state.state = mouse_state.next_state;
        mouse_state.next_state = SERMOUSE_STATE_NULL;
    }

    // process current state
    switch (mouse_state.state) {
        case SERMOUSE_STATE_ID:
            if (mouse_state.idbuf.read_cursor >= mouse_state.idbuf.length) {
                // ID read done, accept reports now
                mouse_state.state = SERMOUSE_STATE_RUN;
                mouse_state.last_pkt_timestamp_us = time_us_32();
            }
            break;
        case SERMOUSE_STATE_RUN: {
            // check if report interval is passed
            if (time_us_32() - mouse_state.last_pkt_timestamp_us >= mouse_state.report_interval_us) {
                // readjust new timestamp to a grid
                if (time_us_32() - mouse_state.last_pkt_timestamp_us >= mouse_state.report_interval_us * 2) {
                    mouse_state.last_pkt_timestamp_us = time_us_32();
                } else {
                    mouse_state.last_pkt_timestamp_us += mouse_state.report_interval_us;
                }

                // send new report!
                sermouse_send_report();
            }
            break;
        }
        default:
            break;
    }
}

