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
#include "audio/audio_fifo.h"

typedef struct sbdsp_t {
    uint8_t inbox;
    uint8_t outbox;
    uint8_t test_register;
    uint8_t current_command;
    uint8_t current_command_index;

    uint16_t dma_interval;     
    int16_t dma_interval_trim;
    audio_fifo_t audio_fifo;

    uint16_t dac_pause_duration;
    uint8_t dac_pause_duration_low;

    uint16_t dma_block_size;
    uint32_t dma_sample_count;
    uint32_t dma_sample_count_rx;

    uint8_t time_constant;
    uint16_t sample_rate;
                
    bool autoinit;    
    bool dma_enabled;

    bool speaker_on;
        
    volatile bool dav_pc;
    volatile bool dav_dsp;
    volatile bool dsp_busy;
    bool dac_resume_pending;

    uint8_t reset_state;  
   
#ifdef SB_BUFFERLESS
    volatile int16_t cur_sample;
#endif
} sbdsp_t;

void sbdsp_init();
void sbdsp_process();
void sbdsp_write(uint8_t address, uint8_t value);
uint8_t sbdsp_read(uint8_t address);
uint16_t sbdsp_sample_rate();
int16_t sbdsp_muted();

#ifdef SB_BUFFERLESS
static inline int16_t sbdsp_sample() {
    extern sbdsp_t sbdsp;
    return (sbdsp.speaker_on & ~sbdsp.dac_resume_pending) ? sbdsp.cur_sample : 0;
}
#endif

void sbdsp_fifo_rx(uint8_t byte);
void sbdsp_fifo_clear();

#endif // SBDSP_H
