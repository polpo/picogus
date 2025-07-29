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
// #include "pico/mutex.h"
// #include "pico/sem.h"

// Configuration
#define AUDIO_FIFO_SIZE 1024  // Must be power of 2
#define AUDIO_FIFO_BITS (AUDIO_FIFO_SIZE - 1)
#define AUDIO_FIFO_START_THRESHOLD (AUDIO_FIFO_SIZE >> 1)  // Start playback when half full

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
uint32_t fifo_level(audio_fifo_t *fifo);
void fifo_reset(audio_fifo_t *fifo);
bool fifo_add_sample(audio_fifo_t *fifo, audio_sample_t sample);
bool fifo_add_samples(audio_fifo_t *fifo, const audio_sample_t *samples_buffer, uint32_t num_samples_to_add);

uint32_t fifo_take_samples(audio_fifo_t *fifo, uint32_t num_samples);

inline uint32_t fifo_free_space(audio_fifo_t *fifo) {
   return AUDIO_FIFO_SIZE - fifo->samples_in_fifo;
}
// Helper for consumer to get one sample, this is a more fundamental FIFO op

#ifdef __cplusplus
}  // extern "C"
#endif
