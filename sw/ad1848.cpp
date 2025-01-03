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

    union {
        struct {
            bool irq_pending: 1;    // IRQ pending
            bool prdy:        1;    // Playback register data ready
            uint8_t pulplr:   2;    // Playback upper/lower left/right flags
            bool sour:        1;    // Sample over/unerrun
            uint8_t :         3;    // Unimplemented capture status bits 
        };
        uint8_t data;
    } status;

    bool play;
    bool mce;           // Mode change enable - mutes output during mode changes
    bool trd;           // Transfer request disable
    bool aci;
    bool irq_enabled;

    uint8_t frame_bytes;
    uint8_t frame_bytes_pio_rcvd;
    uint16_t current_count;
    uint16_t frame_interval;
    uint16_t current_count_left;
    bool playback_enabled;
    bool playback_16bit, playback_signed, playback_stereo;

    bool ppio;

    uint16_t sample_rate;

    sample32 cur_sample;
    // sample32 next_sample;
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
    .status = { .data = 0b11001100 },
    .frame_interval = 125,
    .sample_rate = 8000
}; // all other values to 0


static uint32_t AD1848_DMA_EventHandler(Bitu val);
static PIC_TimerEvent AD1848_DMA_Event = {
    .handler = AD1848_DMA_EventHandler,
};
static uint32_t AD1848_PIO_EventHandler(Bitu val);
static PIC_TimerEvent AD1848_PIO_Event = {
    .handler = AD1848_PIO_EventHandler,
};

static __force_inline void ad1848_playback_stop() {
    ad1848.playback_enabled=false;
    if (!ad1848.ppio) {
        PIC_RemoveEvent(&AD1848_DMA_Event);
        DMA_Cancel_Write(&dma_config);
    } else {
        PIC_RemoveEvent(&AD1848_PIO_Event);
    }
    ad1848.cur_sample.data32 = 0;
}


static __force_inline void ad1848_playback_start() {
    putchar('e');
    putchar('\n');
    if(!ad1848.playback_enabled) {
        ad1848.playback_enabled=true;
        if (!ad1848.ppio) {
            // Set autopush bits to number of bits per audio frame. 32 will get masked to 0 by this operation which is correct behavior
            DMA_Multi_Set_Push_Threshold(&dma_config, ad1848.frame_bytes << 3);
            PIC_AddEvent(&AD1848_DMA_Event, ad1848.frame_interval/* + 1000*/, 0);
        } else {
            PIC_AddEvent(&AD1848_PIO_Event, ad1848.frame_interval/* + 1000*/, 0);
        }
    }

}

static uint32_t interval_avg;
static uint32_t interval_target;
static uint32_t interval_rate;
static __force_inline uint32_t simple_filter(uint32_t x, uint32_t y)
{
    return (x + 31 * y) >> 5;  //( α=1/32 )
}


static bool drq = false;


static __force_inline ad1848_finishEvent() {
    static uint32_t trim = 0;
    ad1848.current_count_left--;
    if(ad1848.current_count_left) {
        // current_interval = ad1848.frame_interval;
        interval_avg = simple_filter(interval_rate + trim, interval_avg);
        trim = (interval_avg < interval_target) ? 1000 : 0;
        return ad1848.frame_interval + (trim ? 1 : 0);
    } else {
        putchar('%');
        ad1848.status.irq_pending = true;
        if (ad1848.irq_enabled) {
            PIC_ActivateIRQ();
        }
        ad1848.current_count_left = ad1848.current_count;
        if (ad1848.trd) {
            ad1848_playback_stop();
            return 0;
        } else {
            return ad1848.frame_interval;
        }
    }
    // return current_interval;
}


static uint32_t AD1848_DMA_EventHandler(Bitu val) {
    DMA_Multi_Start_Write(&dma_config, ad1848.frame_bytes);
    return ad1848_finishEvent();
    ad1848.current_count_left--;
}

static void ad1848_dma_isr(void) {
    // putchar('*');
    // while (!pio_sm_is_rx_fifo_empty(dma_config.pio, dma_config.sm)) {
    // drq = false;
    const uint32_t dma_data = DMA_Complete_Write(&dma_config);
    // printf("dma %x ", dma_data);
    if (ad1848.playback_stereo) {
        if (ad1848.playback_16bit) {
            // 16 bit stereo
            ad1848.cur_sample.data32 = ad1848.playback_signed ? dma_data : dma_data ^ 0x80008000;
        } else {
            // 8 bit stereo
            sample32 next_sample;
            next_sample.data16[0] = dma_data >> 8;
            next_sample.data16[1] = dma_data >> 16;
            ad1848.cur_sample.data32 = ad1848.playback_signed ? next_sample.data32 : next_sample.data32 ^ 0x80008000;
        }
    } else {
        // both 16 and 8 bit mono the same
        sample32 next_sample;
        next_sample.data16[0] = next_sample.data16[1] = dma_data >> 16;
        ad1848.cur_sample.data32 = ad1848.playback_signed ? next_sample.data32 : next_sample.data32 ^ 0x80008000;
    }
}


static uint32_t AD1848_PIO_EventHandler(Bitu val) {
    ad1848.status.prdy = true;
    ad1848.status.sour = (ad1848.frame_bytes_pio_rcvd < ad1848.frame_bytes);
    ad1848.frame_bytes_pio_rcvd = 0;
    return ad1848_finishEvent();
}


static void ad1848_pio_handle(uint8_t data) {
    if (ad1848.playback_stereo) {
        if (ad1848.playback_16bit) {
            ad1848.status.pulplr = ad1848.current_count_left & 0x3;
        } else {
            ad1848.status.pulplr = 0x2 | (ad1848.current_count_left & 0x1);
        }
    } else {
        if (ad1848.playback_16bit) {
            ad1848.status.pulplr = 0x2 | (ad1848.current_count_left & 0x1);
        } else {
            ad1848.status.pulplr = 0x3;
        }
    }
    ad1848.frame_bytes_pio_rcvd++;
    if (ad1848.frame_bytes_pio_rcvd >= ad1848.frame_bytes) {
        ad1848.status.prdy = false;
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


static uint32_t AD1848_Reset_EventHandler(Bitu val) {
    ad1848.aci = false;
    return 0;
}
static PIC_TimerEvent AD1848_Reset_Event = {
    .handler = AD1848_Reset_EventHandler,
};
// static __force_inline void ad1848_reset(uint8_t value) {
//     // PIC_RemoveEvent(&AD1848_Reset_Event);
//     // PIC_AddEvent(&AD1848_Reset_Event, 100, 0);
// }


uint8_t ad1848_read(uint8_t port) {
    switch (port) {
        case 0:
            return ad1848.idx_addr | (ad1848.mce ? 0x40 : 0) | (ad1848.trd ? 0x20 : 0);
        case 1:
            // putchar('r'); putchar(ad1848.idx_addr + 0x30);
            if (ad1848.idx_addr == 11) {
                // putchar('0');
                // return 0x0;
                return (drq ? 0x10 : 0) | (ad1848.aci ? 0x20 : 0);
            }
            return ad1848.regs.idx[ad1848.idx_addr];
        case 2:
            // putchar('r'); putchar('s');
            return ad1848.status.data;
        case 3:
            break; // PIO audio capture not supported
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
            {
            ad1848.idx_addr = data & 0xf;
            bool new_mce = data & 0x40;
            if (!new_mce && ad1848.mce) { // exiting mce
                ad1848.aci = true;
                PIC_AddEvent(&AD1848_Reset_Event, MAX(ad1848.frame_interval << 7, 3000), 0);
            }
            ad1848.mce = new_mce;
            ad1848.trd = data & 0x20;
            }
            break;
        case 1:
            // putchar('w'); putchar(ad1848.idx_addr + 0x30);
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
                    ad1848.frame_interval = interval_target / 1000;
                    interval_rate = ad1848.frame_interval * 1000;
                    ad1848.playback_16bit = data & 0x40;
                    ad1848.playback_signed = data & 0x40;
                    ad1848.playback_stereo = data & 0x10;
                    ad1848.frame_bytes = 1;
                    if (ad1848.playback_16bit) {
                        ad1848.frame_bytes <<= 1;
                    }
                    if (ad1848.playback_stereo) {
                        ad1848.frame_bytes <<= 1;
                    }
                    break;
                case 9:
                    // putchar(data);
                    if (!ad1848.play && (data & 1)) { // start playback
                        ad1848.ppio = (data & 0x40);
                        ad1848.play = true;
                        printf("start sr %u intvl %u 16b %u st %u bpf %u ct %u ppio %u\n", ad1848.sample_rate, ad1848.frame_interval, ad1848.playback_16bit, ad1848.playback_stereo, ad1848.frame_bytes, ad1848.current_count, ad1848.ppio);
                        ad1848_playback_start();
                    } else if (ad1848.play && !(data & 1)) { // stop playback
                        ad1848.play = false;
                        ad1848_playback_stop();
                    }
                    break;
                case 10:
                    ad1848.irq_enabled = data & 2;
                case 14:
                    ad1848.current_count = ((ad1848.regs.ubase << 8) | ad1848.regs.lbase) + 1;
                    ad1848.current_count_left = ad1848.current_count;
                    break;
            }
            break;
        case 2:
            // putchar('w'); putchar('s');
            ad1848.status.irq_pending = false;
            PIC_DeActivateIRQ();
            if (ad1848.play && ad1848.trd) {
                ad1848_playback_start();
            }
            break;
        case 3:
            ad1848_pio_handle(data);
            // putchar('w'); putchar('p');
            break;
    }
}
