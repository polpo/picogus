/*
Title  : SoundBlaster DSP Emulation Header
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

#ifndef SBDSP_H
#define SBDSP_H

#include <inttypes.h>
#include <stdbool.h>

typedef struct sbdsp_t {
    uint8_t inbox;
    uint8_t outbox;
    uint8_t test_register;
    uint8_t current_command;
    uint8_t current_command_index;

    uint8_t mixer_command;

    uint16_t dma_interval;

    uint16_t dac_pause_duration;
    uint8_t dac_pause_duration_low;

    uint16_t dma_block_size;
    uint32_t dma_bytes_per_frame;
    uint16_t dma_sample_count;
    uint32_t dma_xfer_count;
    uint32_t dma_xfer_count_left;

    uint8_t time_constant;
    uint16_t sample_rate;

    bool autoinit;
    bool dma_enabled;
    bool dma_16bit;
    bool dma_stereo;
    bool dma_stereo_sbpro;
    bool dma_signed;
    volatile bool dma_done;

    bool speaker_on;

    volatile bool dav_pc;
    volatile bool dav_dsp;
    volatile bool dsp_busy;
    bool dac_resume_pending;
    volatile bool irq_8_pending;
    volatile bool irq_16_pending;
    uint8_t reset_state;

    union {
        struct { uint8_t minor, major; };
        uint16_t w;
    } dsp_version;

    union {
        uint8_t irq;
        uint8_t dma;
    } resources;

    uint8_t ident_e2[2];

    union {
        struct {
            uint8_t fixTC      : 1;
            uint8_t lockMixer  : 2;
        };
        uint8_t b;
    } options;

    volatile uint32_t cur_sample;  // packed stereo pair: L in low 16, R in high 16
#if 0
    struct {
        uint32_t old_sample;
        uint32_t new_sample;
        volatile uint32_t dma_sample;
        volatile bool dma_sample_ready;
        bool dma_pending;        // a DMA transfer is in flight
        int32_t samplecnt;
    } rsm;                       // resampling state, memset to zero on start/stop
#else
    struct {
        int32_t  phase_acc;
        uint32_t buf[2];        // sample buffer, used for linear interpolation

        // DMA stuff
        // TODO: make it a very short (max. 8 samples) ring buffer for Creative ADPCM
        // and rates slightly higher than 44100Hz (SB16 cap is 45454Hz)
    } rs;
    struct {
        volatile uint32_t dma_sample;
        volatile bool dma_sample_ready;
        bool dma_pending;        // a DMA transfer is in flight
    } rsm;
#endif

    int32_t rateratio;
} sbdsp_t;

void sbdsp_init();
void sbdsp_process();
void sbdsp_write(uint8_t address, uint8_t value);
uint8_t sbdsp_read(uint8_t address);
uint16_t sbdsp_sample_rate();
int16_t sbdsp_muted();

uint32_t sbdsp_generate_sample();
static inline uint32_t sbdsp_sample_stereo() {
    extern sbdsp_t sbdsp;
    if (sbdsp.dma_enabled && !sbdsp.dac_resume_pending) sbdsp.cur_sample = sbdsp_generate_sample();
    return sbdsp.speaker_on ? sbdsp.cur_sample : 0;
}

// -------------------------
// PicoGUS configuration interface helpers

void sbdsp_set_type(uint8_t type);
void sbdsp_set_irq(uint8_t irq); 
void sbdsp_set_dma(uint8_t dma); 
void sbdsp_set_options(uint8_t options); 

#endif // SBDSP_H
