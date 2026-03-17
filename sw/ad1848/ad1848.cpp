/*
 *  Copyright (C) 2024  Ian Scott
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "system/pico_pic.h"
#include "audio/volctrl.h"
#include "ad1848.h"

extern uint LED_PIN;

#include "isa/isa_dma.h"

#ifndef MAX
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#endif

#define AD1848_RSM_FRAC 10

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
            bool sour:        1;    // Sample over/underrun
            uint8_t :         3;    // Unimplemented capture status bits
        };
        uint8_t data;
    } status;

    bool play;
    bool capture;       // Fake capture active
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

    volatile uint32_t cur_sample;  // packed stereo pair: L in low 16, R in high 16

    struct {
        uint32_t old_sample;
        uint32_t new_sample;
        volatile uint32_t dma_sample;
        volatile bool dma_sample_ready;
        bool dma_pending;
        int32_t samplecnt;
    } rsm;
    int32_t rateratio;
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
    ad1848.playback_enabled = false;
    if (!ad1848.ppio) {
        DMA_Cancel_Write(&dma_config);
    } else {
        PIC_RemoveEvent(&AD1848_PIO_Event);
    }
    memset(&ad1848.rsm, 0, sizeof(ad1848.rsm));
    ad1848.cur_sample = 0;
}


static void ad1848_fake_capture_start() {
    // Stop DMA PIO SM so DACK cycles don't trigger it
    pio_sm_set_enabled(pio0, DMA_PIO_SM, false);

    // Switch DRQ_PIN from PIO to SIO, assert DRQ
    gpio_put(DRQ_PIN, 1);
    gpio_set_function(DRQ_PIN, GPIO_FUNC_SIO);
}

static void ad1848_fake_capture_stop() {
    // Deassert DRQ
    gpio_put(DRQ_PIN, 0);

    // Restore DRQ_PIN to PIO control
    pio_gpio_init(pio0, DRQ_PIN);

    // Restart DMA PIO SM from beginning
    pio_sm_restart(pio0, DMA_PIO_SM);
    pio_sm_exec(pio0, DMA_PIO_SM, pio_encode_jmp(dma_config.offset));
    pio_sm_set_enabled(pio0, DMA_PIO_SM, true);
}

static __force_inline void ad1848_playback_start() {
    if (!ad1848.playback_enabled) {
        ad1848.playback_enabled = true;
        if (!ad1848.ppio) {
            // Pull model: audio callback drives DMA, not timer
            DMA_Multi_Set_Push_Threshold(&dma_config, ad1848.frame_bytes << 3);
            memset(&ad1848.rsm, 0, sizeof(ad1848.rsm));
            ad1848.rsm.dma_pending = true;
            DMA_Multi_Start_Write(&dma_config, ad1848.frame_bytes);
        } else {
            PIC_AddEvent(&AD1848_PIO_Event, ad1848.frame_interval, 0);
        }
    }
}

static void ad1848_apply_iface(uint8_t iface) {
    if (!ad1848.play && (iface & 1)) { // start playback
        ad1848.ppio = (iface & 0x40);
        ad1848.play = true;
        printf("start sr %u intvl %u 16b %u st %u bpf %u ct %u ppio %u\n",
               ad1848.sample_rate, ad1848.frame_interval,
               ad1848.playback_16bit, ad1848.playback_stereo,
               ad1848.frame_bytes, ad1848.current_count, ad1848.ppio);
        ad1848_playback_start();
    } else if (ad1848.play && !(iface & 1)) { // stop playback
        ad1848.play = false;
        ad1848_playback_stop();
    }
    if (!ad1848.capture && (iface & 2)) { // start fake capture
        ad1848.capture = true;
        ad1848_fake_capture_start();
    } else if (ad1848.capture && !(iface & 2)) { // stop fake capture
        ad1848.capture = false;
        ad1848_fake_capture_stop();
    }
}

static dma_dither_t dither;

static __force_inline uint32_t ad1848_finishEvent() {
    ad1848.current_count_left--;
    if (ad1848.current_count_left) {
        return DMA_Dither_Step(&dither, ad1848.frame_interval);
    } else {
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
}


static uint32_t AD1848_DMA_EventHandler(Bitu val) {
    return 0;  // pull model: audio callback drives DMA, not timer
}

static void ad1848_dma_isr(void) {
    const uint32_t dma_data = DMA_Complete_Write(&dma_config);
    uint32_t sample;
    if (ad1848.playback_stereo) {
        if (ad1848.playback_16bit) {
            sample = dma_data;
        } else {
            // 8-bit stereo: L at [23:16], R at [31:24] to MSB of each 16-bit half
            sample = (dma_data & 0xFF000000) | ((dma_data >> 8) & 0x0000FF00);
        }
    } else {
        if (ad1848.playback_16bit) {
            // 16-bit mono: duplicate upper 16 to both halves
            sample = (dma_data & 0xFFFF0000) | (dma_data >> 16);
        } else {
            // 8-bit mono: byte at [31:24], duplicate to MSB of each half
            sample = (dma_data & 0xFF000000) | ((dma_data >> 16) & 0x0000FF00);
        }
    }
    if (!ad1848.playback_signed) sample ^= 0x80008000;
    ad1848.rsm.dma_sample = sample;
    ad1848.rsm.dma_sample_ready = true;
    ad1848.rsm.dma_pending = false;
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


static uint32_t ad1848_generate_sample() {
    // Consume ready DMA samples when the fractional accumulator says we need them
    while (ad1848.rsm.samplecnt >= ad1848.rateratio) {
        if (!ad1848.rsm.dma_sample_ready) break;

        ad1848.rsm.old_sample = ad1848.rsm.new_sample;
        ad1848.rsm.new_sample = ad1848.rsm.dma_sample;
        ad1848.rsm.dma_sample_ready = false;
        ad1848.rsm.samplecnt -= ad1848.rateratio;

        // Track transfer count
        ad1848.current_count_left--;
        if (!ad1848.current_count_left) {
            ad1848.status.irq_pending = true;
            if (ad1848.irq_enabled) {
                PIC_ActivateIRQ();
            }
            if (ad1848.trd) {
                ad1848_playback_stop();
                break;
            }
            ad1848.current_count_left = ad1848.current_count;
        }

        // Pipeline: kick off next DMA transfer
        if (ad1848.playback_enabled && !ad1848.rsm.dma_pending) {
            ad1848.rsm.dma_pending = true;
            DMA_Multi_Start_Write(&dma_config, ad1848.frame_bytes);
        }
    }

    // Linear interpolation between old_sample and new_sample
    int16_t old_l = (int16_t)(ad1848.rsm.old_sample & 0xFFFF);
    int16_t old_r = (int16_t)(ad1848.rsm.old_sample >> 16);
    int16_t new_l = (int16_t)(ad1848.rsm.new_sample & 0xFFFF);
    int16_t new_r = (int16_t)(ad1848.rsm.new_sample >> 16);

    int16_t out_l = (int16_t)((old_l * (ad1848.rateratio - ad1848.rsm.samplecnt)
                     + new_l * ad1848.rsm.samplecnt) / ad1848.rateratio);
    int16_t out_r = (int16_t)((old_r * (ad1848.rateratio - ad1848.rsm.samplecnt)
                     + new_r * ad1848.rsm.samplecnt) / ad1848.rateratio);

    ad1848.rsm.samplecnt += 1 << AD1848_RSM_FRAC;

    return (uint32_t)(uint16_t)out_l | ((uint32_t)(uint16_t)out_r << 16);
}

uint32_t ad1848_sample_stereo() {
    if (ad1848.playback_enabled && !ad1848.ppio) return ad1848_generate_sample();
    return ad1848.cur_sample;
}


void ad1848_init() {
    puts("Initing AD1848...");
    puts("Initing ISA DMA PIO...");
    AD1848_DMA_isr_pt = ad1848_dma_isr;
    dma_config = DMA_multi_init(pio0, DMA_PIO_SM, AD1848_DMA_isr_pt);

    // Pre-set DRQ_PIN SIO direction for fake capture
    gpio_set_dir(DRQ_PIN, GPIO_OUT);
}


static uint32_t AD1848_Reset_EventHandler(Bitu val) {
    ad1848.aci = false;
    return 0;
}
static PIC_TimerEvent AD1848_Reset_Event = {
    .handler = AD1848_Reset_EventHandler,
};


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

uint8_t ad1848_read(uint8_t port) {
    switch (port) {
        case 0:
            return ad1848.idx_addr | (ad1848.mce ? 0x40 : 0) | (ad1848.trd ? 0x20 : 0);
        case 1:
            if (ad1848.idx_addr == 11) {
                return (ad1848.aci ? 0x20 : 0);
            }
            return ad1848.regs.idx[ad1848.idx_addr];
        case 2:
            return ad1848.status.data;
        case 3:
            break; // PIO audio capture not supported
    }
    return 0;
}

void ad1848_write(uint8_t port, uint8_t data) {
    switch (port) {
        case 0:
            {
            ad1848.idx_addr = data & 0xf;
            bool new_mce = data & 0x40;
            if (!new_mce && ad1848.mce) { // exiting mce
                ad1848.aci = true;
                PIC_AddEvent(&AD1848_Reset_Event, MAX(ad1848.frame_interval << 7, 3000), 0);
                ad1848_apply_iface(ad1848.regs.iface);
            }
            ad1848.mce = new_mce;
            ad1848.trd = data & 0x20;
            }
            break;
        case 1:
            if (ad1848.idx_addr == 12)
                return;
            if (ad1848.idx_addr != 11) {
                ad1848.regs.idx[ad1848.idx_addr] = data;
            }
            switch (ad1848.idx_addr)
            {
                case 8: {
                    if (!ad1848.mce)
                        return;
                    ad1848.regs.dform &= 0x7f;
                    ad1848.sample_rate = sample_rates[data & 0xF];
                    uint32_t target_ns = 1000000000ul / ad1848.sample_rate;
                    ad1848.frame_interval = target_ns / 1000;
                    DMA_Dither_Init(&dither, target_ns, ad1848.frame_interval);
                    ad1848.rateratio = (44100 << AD1848_RSM_FRAC) / ad1848.sample_rate;
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
                }
                case 9:
                    if (ad1848.mce) break; // Defer until MCE exit
                    ad1848_apply_iface(data);
                    break;
                case 10:
                    ad1848.irq_enabled = data & 2;
                    break;
                case 14:
                    ad1848.current_count = ((ad1848.regs.ubase << 8) | ad1848.regs.lbase) + 1;
                    ad1848.current_count_left = ad1848.current_count;
                    break;
            }
            break;
        case 2:
            ad1848.status.irq_pending = false;
            PIC_DeActivateIRQ();
            if (ad1848.play && ad1848.trd) {
                ad1848_playback_start();
            }
            break;
        case 3:
            ad1848_pio_handle(data);
            break;
    }
}
