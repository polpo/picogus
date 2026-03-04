/*
 *  Copyright (C) 2025  Ian Scott
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
/**
 * audio_fifo.h - Shared header for audio producer and consumer
 *
 * Implements a thread-safe FIFO for audio data between cores on RP2040
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "hardware/sync.h"

// Configuration
#define AUDIO_FIFO_SIZE 4096  // Must be power of 2
                              // Needs to be > 2*SAMPLES_PER_SECTOR (1176) to absorb
                              // slow FAT/USB sector reads without underrun (~93ms at 44100Hz)
#define AUDIO_FIFO_BITS (AUDIO_FIFO_SIZE - 1)
#define AUDIO_FIFO_START_THRESHOLD 512  // Start playback after 512 samples (~11ms); lower = faster post-seek response

#ifdef __cplusplus
extern "C" {
#endif

// Audio sample type - adjust as needed for your audio format
typedef int16_t audio_sample_t;

// FIFO state
typedef enum {
    FIFO_STATE_STOPPED,
    FIFO_STATE_RUNNING,
} fifo_state_t;

// FIFO structure
typedef struct {
    audio_sample_t buffer[AUDIO_FIFO_SIZE];
    volatile uint32_t write_idx;
    volatile uint32_t read_idx;
    volatile fifo_state_t state;
    // mutex_t mutex;
    // semaphore_t data_available;
    volatile uint32_t samples_in_fifo;
} audio_fifo_t;

// FIFO management functions (implemented in audio_fifo.c)
// No more get_audio_fifo() - caller owns the fifo instance(s)
void fifo_init(audio_fifo_t *fifo);
void fifo_reset(audio_fifo_t *fifo);
bool fifo_add_samples(audio_fifo_t *fifo, const audio_sample_t *samples_buffer, uint32_t num_samples_to_add);

// Inline functions for performance
static inline uint32_t fifo_level(audio_fifo_t *fifo) {
    return fifo->samples_in_fifo;
}

static inline bool fifo_add_sample(audio_fifo_t *fifo, audio_sample_t sample) {
    if (fifo->samples_in_fifo == AUDIO_FIFO_SIZE) {
        return false;
    }
    fifo->buffer[fifo->write_idx] = sample;
    fifo->write_idx = (fifo->write_idx + 1) & AUDIO_FIFO_BITS;
    // Disable interrupts around the counter increment: the PWM ISR (consumer)
    // runs on the same core and its samples_in_fifo-- can preempt the
    // load-add-store here, causing a lost decrement and an inflated count.
    uint32_t irq_save = save_and_disable_interrupts();
    fifo->samples_in_fifo++;
    if (fifo->samples_in_fifo >= AUDIO_FIFO_START_THRESHOLD) {
        fifo->state = FIFO_STATE_RUNNING;
    }
    restore_interrupts(irq_save);
    return true;
}

uint32_t fifo_take_samples(audio_fifo_t *fifo, uint32_t num_samples);

static inline uint32_t fifo_free_space(audio_fifo_t *fifo) {
   return AUDIO_FIFO_SIZE - fifo->samples_in_fifo;
}

// Inline version of fifo_take_samples for performance-critical code
static inline uint32_t fifo_take_samples_inline(audio_fifo_t *fifo, uint32_t num_samples) {
    if (fifo->state == FIFO_STATE_STOPPED) {
        return 0;
    }
    uint32_t samples_returned = fifo->samples_in_fifo < num_samples ? fifo->samples_in_fifo : num_samples;
    fifo->read_idx = (fifo->read_idx + samples_returned) & AUDIO_FIFO_BITS;
    fifo->samples_in_fifo -= samples_returned;
    if (fifo->samples_in_fifo == 0) {
        fifo->state = FIFO_STATE_STOPPED;
    }
    return samples_returned;
}

// Helper for consumer to get one sample, this is a more fundamental FIFO op

#ifdef __cplusplus
}  // extern "C"
#endif
