#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "pico_pic.h"

/*
Title  : SoundBlaster DSP Emulation
Date   : 2023-12-30
Author : Kevin Moonlight <me@yyzkevin.com>

Copyright (C) 2023 Kevin Moonlight
Copyright (C) 2024 Ian Scott

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

SPDX-License-Identifier: MIT
*/

extern uint LED_PIN;

#include "isa_dma.h"

static irq_handler_t AD1848_DMA_isr_pt;
static dma_inst_t dma_config;
#define DMA_PIO_SM 2

union sample32 {
    uint32_t data32;
    int16_t data16[2];
    uint8_t data8[4];
};

typedef struct ad1848_t {
    union {
        struct {
            uint8_t linp;  //  0
            uint8_t rinp;  //  1
            uint8_t laux1; //  2
            uint8_t raux1; //  3
            uint8_t laux2; //  4
            uint8_t raux2; //  5
            uint8_t lout;  //  6
            uint8_t rout;  //  7
            uint8_t dform; //  8
            uint8_t iface; //  9
            uint8_t pinc;  // 10
            uint8_t init;  // 11
            uint8_t misc;  // 12
            uint8_t mix;   // 13
            uint8_t ubase; // 14
            uint8_t lbase; // 15
        };
        uint8_t idx[16];
    } regs;

    uint8_t idx_addr;   // Register index address
    uint8_t status;

    bool play;
    bool mce;           // Mode change enable - mutes output during mode changes
    bool trd;           // Transfer request disable
    bool irq_enabled;
    bool irq_pending;

    uint8_t dma_bytes_per_frame;
    uint16_t dma_count;
    uint16_t dma_interval;
    uint16_t dma_count_left;
    bool dma_enabled;
    bool dma_16bit, dma_signed, dma_stereo;

    uint16_t sample_rate;

    uint8_t interrupt;
    uint8_t dma;

    sample32 cur_sample;
    sample32 next_sample;
} ad1848_t;

static ad1848_t ad1848 = {
    .regs = {
        .laux1 = 0b10000000,
        .raux1 = 0b10000000,
        .laux2 = 0b10000000,
        .raux2 = 0b10000000,
        .lout  = 0b10000000,
        .rout  = 0b10000000,
        .iface = 0b00001000,
        .misc  = 0b00001010
    },
    .status = 0b11001100
}; // all other values to 0


static uint32_t AD1848_DMA_EventHandler(Bitu val);
static PIC_TimerEvent AD1848_DMA_Event = {
    .handler = AD1848_DMA_EventHandler,
};

static __force_inline void ad1848_dma_disable() {
    ad1848.dma_enabled=false;
    PIC_RemoveEvent(&AD1848_DMA_Event);
    ad1848.cur_sample.data32 = 0;
}

static __force_inline void ad1848_dma_enable() {
    putchar('e');
    if(!ad1848.dma_enabled) {
        ad1848.dma_enabled=true;
        // Set autopush bits to number of bits per audio frame. 32 will get masked to 0 by this operation which is correct behavior
        DMA_Multi_Set_Push_Threshold(&dma_config, ad1848.dma_bytes_per_frame << 3);
        // hw_write_masked(&pio0->sm[DMA_PIO_SM].shiftctrl,
        //                 (ad1848.dma_bytes_per_frame << 3) << PIO_SM0_SHIFTCTRL_PUSH_THRESH_LSB,
        //                 PIO_SM0_SHIFTCTRL_PUSH_THRESH_BITS);
        PIC_AddEvent(&AD1848_DMA_Event, ad1848.dma_interval + 100, 0);
    }
    else {
        //printf("INFO: DMA Already Enabled");
    }
}


static uint32_t interval_avg;
static uint32_t interval_target;
static uint32_t interval_rate;
static __force_inline uint32_t simple_filter(uint32_t x, uint32_t y)
{
    return (x + 31 * y) >> 5;  //( α=1/32 )
}


static uint16_t trim = 0;

static bool drq;

static uint32_t AD1848_DMA_EventHandler(Bitu val) {
    // uint32_t current_interval;
    static uint32_t trim;

    // uint32_t to_xfer = MIN(ad1848.dma_bytes_per_frame, sbdsp.dma_xfer_count_left);
    DMA_Multi_Start_Write(&dma_config, ad1848.dma_bytes_per_frame);
    drq = true;
    // DMA_Start_Write(&dma_config);
    ad1848.dma_count_left--;
    // putchar(sbdsp.dma_xfer_count_left + 0x30);

    // current_interval = sbdsp.dma_interval;

    // sbdsp.dsp_busy = sbdsp.dma_xfer_count_left > 10000;
    // if ((sbdsp.dma_xfer_count_left & 0xfff) == 0)
    // printf("%u\n", sbdsp.dma_xfer_count_left);

    if(ad1848.dma_count_left) {
        // current_interval = ad1848.dma_interval;
        interval_avg = simple_filter(interval_rate + trim, interval_avg);
        trim = (interval_avg < interval_target) ? 1000 : 0;
        return ad1848.dma_interval + (trim ? 1 : 0);
    } else {
        // putchar('%');
        ad1848.irq_pending = true;
        if (ad1848.irq_enabled) {
            PIC_ActivateIRQ();
        }
        ad1848.dma_count_left = ad1848.dma_count;
        if (ad1848.trd) {
            ad1848_dma_disable();
            return 0;
        } else {
            return ad1848.dma_interval;
        }
    }
    // return current_interval;
}


static void ad1848_dma_isr(void) {
    // while (!pio_sm_is_rx_fifo_empty(dma_config.pio, dma_config.sm)) {
    drq = false;
    const uint32_t dma_data = DMA_Complete_Write(&dma_config);
    // printf("dma %x ", dma_data);
    if (ad1848.dma_stereo) {
        if (ad1848.dma_16bit) {
            // 16 bit stereo
            ad1848.cur_sample.data32 = ad1848.dma_signed ? dma_data : dma_data ^ 0x80008000;
        } else {
            // 8 bit stereo
            ad1848.next_sample.data16[0] = dma_data >> 16;
            ad1848.next_sample.data16[1] = dma_data >> 8;
            ad1848.cur_sample.data32 = ad1848.dma_signed ? ad1848.next_sample.data32 : ad1848.next_sample.data32 ^ 0x80008000;
        }
    } else {
        // TODO 16 bit mono
        // 8 bit mono
        ad1848.next_sample.data16[0] = ad1848.next_sample.data16[1] = dma_data >> 16;
        ad1848.cur_sample.data32 = ad1848.dma_signed ? ad1848.next_sample.data32 : ad1848.next_sample.data32 ^ 0x80008000;
    }
}


void ad1848_samples(int16_t *buf) {
    memcpy(buf, &ad1848.cur_sample, 4);
}


void ad1848_init() {
    puts("Initing AD1848...");
    puts("Initing ISA DMA PIO...");
    AD1848_DMA_isr_pt = ad1848_dma_isr;
    dma_config = DMA_multi_init(pio0, DMA_PIO_SM, AD1848_DMA_isr_pt);
    // dma_config = DMA_init(pio0, DMA_PIO_SM, AD1848_DMA_isr_pt);
}


// static __force_inline void sbdsp_set_dma_interval() {
//     if (sbdsp.sample_rate) {
//         sbdsp.dma_interval = 1000000ul / sbdsp.sample_rate;
//     } else {
//         sbdsp.dma_interval = 256 - sbdsp.time_constant;
//     }
//     if (sbdsp.dma_stereo) {
//         // printf("stereoeoeoeo");
//         sbdsp.dma_interval >>= 1;
//     }
//     if (sbdsp.dma_16bit) {
//         sbdsp.dma_interval >>= 1;
//     }
// }


// static uint32_t AD1848_Reset_EventHandler(Bitu val) {
//     return 0;
// }
// static PIC_TimerEvent AD1848_Reset_Event = {
//     .handler = AD1848_Reset_EventHandler,
// };
// static __force_inline void ad1848_reset(uint8_t value) {
//     // PIC_RemoveEvent(&DSP_Reset_Event);
//     // PIC_AddEvent(&DSP_Reset_Event, 100, 0);
// }


uint8_t ad1848_read(uint8_t port) {
    switch (port) {
        case 0:
            return ad1848.idx_addr | (ad1848.mce ? 0x40 : 0) | (ad1848.trd ? 0x20 : 0);
        case 1:
            putchar('r'); putchar(ad1848.idx_addr + 0x30);
            if (ad1848.idx_addr == 4) {
                return drq ? 0x10 : 0;
            }
            return ad1848.regs.idx[ad1848.idx_addr];
        case 2:
            putchar('r'); putchar('s');
            return ad1848.status | (ad1848.irq_pending ? 1 : 0);
        case 3:
            break; // PIO audio not supported
    }
    return 0;
}

static constexpr uint16_t sample_rates[] = {
    8000,
    5513,
    16000,
    11025,
    27429,
    18900,
    32000,
    22050,
    54857, // Unsupported
    37800,
    64000, // Unsupported
    44100,
    48000,
    33075,
    9600,
    6615
};

void ad1848_write(uint8_t port, uint8_t data) {
    static constexpr int div_factor[] = {3072, 1536, 896, 768, 448, 384, 512, 2560};
    switch (port) {
        case 0:
            ad1848.idx_addr = data & 0xf;
            ad1848.mce = data & 0x40;
            ad1848.trd = data & 0x20;
            break;
        case 1:
            putchar('w'); putchar(ad1848.idx_addr + 0x30);
            if(ad1848.idx_addr == 12)
                return;
            if (ad1848.idx_addr != 11) {
                ad1848.regs.idx[ad1848.idx_addr] = data;
            }
            switch (ad1848.idx_addr)
            {
                case 8:
                    if (!ad1848.mce)
                        return;
                    ad1848.regs.dform &= 0x7f;
                    ad1848.sample_rate = sample_rates[data & 0xF];
                    interval_target = 1000000000ul / ad1848.sample_rate;
                    interval_avg = interval_target;
                    ad1848.dma_interval = interval_target / 1000;
                    interval_rate = ad1848.dma_interval * 1000;
                    ad1848.dma_16bit = data & 0x40;
                    ad1848.dma_signed = data & 0x40;
                    ad1848.dma_stereo = data & 0x10;
                    ad1848.dma_bytes_per_frame = 1;
                    if (ad1848.dma_16bit) {
                        ad1848.dma_bytes_per_frame <<= 1;
                        // ad1848.dma_interval >>= 1;
                        // ad1848.dma_count <<= 1;
                    }
                    if (ad1848.dma_stereo) {
                        ad1848.dma_bytes_per_frame <<= 1;
                        // ad1848.dma_interval >>= 1;
                        // ad1848.dma_count <<= 1;
                    }
                    // ad1848.dma_interval += 1;
                    break;
                case 9:
                    if (!ad1848.dma_enabled && (data & 1)) { // start playback
                        printf("starting playback! sr %u intvl %u 16b %u stereo %u bpf %u", ad1848.sample_rate, ad1848.dma_interval, ad1848.dma_16bit, ad1848.dma_stereo, ad1848.dma_bytes_per_frame);
                        ad1848_dma_enable();
                    }
                    if (ad1848.dma_enabled && !(data & 1)) { // stop playback
                        ad1848_dma_disable();
                    }
                    break;
                case 10:
                    ad1848.irq_enabled = data & 2;
                case 14:
                    ad1848.dma_count = ((ad1848.regs.ubase << 8) | ad1848.regs.lbase) + 1;
                    ad1848.dma_count_left = ad1848.dma_count;
                    break;
            }
            break;
        case 2:
            putchar('w'); putchar('s');
            ad1848.irq_pending = false;
            PIC_DeActivateIRQ();
            ad1848_dma_enable();
            break;
        case 3:
            break; // playback
    }
}
