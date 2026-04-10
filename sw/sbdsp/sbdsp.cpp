#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "system/pico_pic.h"
#include "audio/volctrl.h"
#include "sbdsp.h"
#ifdef SOUND_MPU
#include "mpu401/export.h"
#endif

/*
Title  : SoundBlaster DSP Emulation
Date   : 2023-12-30
Author : Kevin Moonlight <me@yyzkevin.com>

Copyright (C) 2023 Kevin Moonlight
Copyright (C) 2024-2026 Ian Scott
Copyright (C) 2026 Artem Vasilev

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

#include "isa/isa_dma.h"
#ifdef INTERP_SB_LINEAR
#include "hardware/interp.h"
#endif

#ifndef MAX
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#endif

static irq_handler_t SBDSP_DMA_isr_pt;
static dma_inst_t dma_config;
#define DMA_PIO_SM 2

#define SB_TYPE_SB1         0x1
#define SB_TYPE_SBPRO1      0x2
#define SB_TYPE_SB2         0x3
#define SB_TYPE_SBPRO2      0x4
#define SB_TYPE_SBPRO2MCA   0x5
#define SB_TYPE_SB16        0x6

// Sound Blaster DSP I/O port offsets
#define DSP_RESET           0x6
#define DSP_READ            0xA
#define DSP_WRITE           0xC
#define DSP_WRITE_STATUS    0xC
#define DSP_READ_STATUS     0xE
#define DSP_IRQ_16_STATUS   0xF
#define MIXER_COMMAND       0x4
#define MIXER_DATA          0x5

#define OUTPUT_SAMPLERATE   49716ul

// Sound Blaster DSP commands.
#define DSP_DMA_HS_SINGLE       0x91
#define DSP_DMA_HS_AUTO         0x90
#define DSP_DMA_SINGLE          0x14    //follosed by length
#define DSP_DMA_AUTO            0X1C    //length based on 48h
#define DSP_DMA_BLOCK_SIZE      0x48    //block size for highspeed/dma

// SB16 DSP commands
#define DSP_DMA_IO_START            0xB0
#define DSP_DMA_IO_END              0xCF
#define DSP_PAUSE_DMA_16            0xD5
#define DSP_CONTINUE_DMA_16         0xD6
#define DSP_CONTINUE_AUTOINIT_8     0x45
#define DSP_CONTINUE_AUTOINIT_16    0x47
#define DSP_EXIT_AUTOINIT_16        0xD9
#define DSP_EXIT_AUTOINIT_8         0xDA

//#define DSP_DMA_DAC 0x14
#define DSP_DIRECT_DAC          0x10
#define DSP_DIRECT_ADC          0x20
#define DSP_MIDI_READ_POLL      0x30
#define DSP_MIDI_WRITE_POLL     0x38
#define DSP_SET_TIME_CONSTANT   0x40
#define DSP_SET_SAMPLING_RATE   0x41
#define DSP_PAUSE_DMA_8         0xD0
#define DSP_DAC_PAUSE_DURATION  0x80    // Pause DAC for a duration, then generate an interrupt. Used by Tyrian.
#define DSP_ENABLE_SPEAKER      0xD1
#define DSP_DISABLE_SPEAKER     0xD3
#define DSP_CONTINUE_DMA_8      0xD4
#define DSP_SPEAKER_STATUS      0xD8
// SB16 ASP commands
#define DSP_ASP_SET_CODEC_PARAM 0x05
#define DSP_ASP_SET_REGISTER    0x0E
#define DSP_ASP_GET_REGISTER    0x0F

#define DSP_IDENT               0xE0
#define DSP_VERSION             0xE1
#define DSP_IDENT_E2            0xE2
#define DSP_COPYRIGHT           0xE3
#define DSP_WRITETEST           0xE4
#define DSP_READTEST            0xE8
#define DSP_SINE                0xF0
#define DSP_IRQ_8               0xF2
#define DSP_IRQ_16              0xF3
#define DSP_CHECKSUM            0xF4
#define DSP_8051_READ           0xF9
#define DSP_8051_WRITE          0xFA

#define MIXER_INTERRUPT         0x80
#define MIXER_DMA               0x81
#define MIXER_IRQ_STATUS        0x82
#define MIXER_STEREO            0xE

#define MIXER_VOL_VOICE         0x04
#define MIXER_VOL_MASTER        0x22
#define MIXER_VOL_MIDI          0x26
#define MIXER_VOL_CD            0x28
#define MIXER_VOL_LINE          0x2E

#define MIXER_VOL_MASTER_L      0x30        // SB16
#define MIXER_VOL_MASTER_R      0x31        // SB16
#define MIXER_VOL_VOICE_L       0x32        // SB16
#define MIXER_VOL_VOICE_R       0x33        // SB16
#define MIXER_VOL_MIDI_L        0x34        // SB16
#define MIXER_VOL_MIDI_R        0x35        // SB16
#define MIXER_VOL_CD_L          0x36        // SB16
#define MIXER_VOL_CD_R          0x37        // SB16
#define MIXER_VOL_LINE_L        0x38        // SB16
#define MIXER_VOL_LINE_R        0x39        // SB16
#define MIXER_OUT_SWITCH        0x3C        // SB16

#define DSP_UNUSED_STATUS_BITS_PULLED_HIGH 0x7F

// mixer locking settings
#define MIXER_LOCK_NONE                 0   // none, all registers writeable
#define MIXER_LOCK_ALL_BUT_SBP_VOICE    1   // lock all but SBPro Voice (for stereo FX like in Wolf3D)
#define MIXER_LOCK_ALL                  2   // lock all registers 

static char const copyright_string[] = "COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992.";

union sample32 {
    uint32_t data32;
    int16_t data16[2];
    uint8_t data8[4];
};

sbdsp_t sbdsp;

// Creative ADPCM decoders. Each sub-sample is decoded as:
//   delta = magnitude * accumulator + accumulator/2
//   reference += +/-delta (clamped to 0..255, sign from the sub-sample's sign bit)
// The accumulator doubles on max-magnitude sub-samples (up to a per-format
// cap) and halves on zero-magnitude sub-samples (down to 1).
//
// Algorithm derived from the SB v2.02 DSP firmware disassembly (TubeTime,
// https://github.com/schlae/sb-firmware) and VocTool (MIT, Torsten Stremlau).
// Not derived from DOSBox/dosbox-x or 86Box.

// 4-bit: nibble is sign (bit 3) + 3 magnitude bits (bits 2..0).
// Accumulator range [1..8].
// Structure adapted from VocTool's CreativeAdpcmDecoder4Bit (MIT, Torsten
// Stremlau); the sign formula below is inverted relative to VocTool's so that
// bit 3 set means subtract, matching the SB DSP firmware convention.
static inline uint8_t decode_ADPCM_4_sample(uint8_t bits) {
    int32_t sign = 1 - ((bits & 8) >> 2);            // bit 3 is the sign bit (0 -> +1, 1 -> -1)
    int32_t data = bits & 7;                         // the lower 3 bits are the sample data
    int32_t delta =
        (data * sbdsp.adpcm.accum) +
        (sbdsp.adpcm.accum >> 1);                     // scale sample data using accumulator value
    int32_t result = sbdsp.adpcm.reference + sign * delta;   // calculate the next value
    if (result > 0xff) result = 0xff;                // limit value to 0..255
    else if (result < 0) result = 0;
    sbdsp.adpcm.reference = (uint8_t)result;

    if ((data == 0) && (sbdsp.adpcm.accum > 1))       // if input value is 0, and accumulator is
        sbdsp.adpcm.accum >>= 1;                      // larger than 1, then halve accumulator.
    if ((data >= 5) && (sbdsp.adpcm.accum < 8))       // if input value larger than 5, and accumulator is
        sbdsp.adpcm.accum <<= 1;                      // lower than 8, then double accumulator.

    return sbdsp.adpcm.reference;
}

// 2.6-bit: 3 samples per byte (3 bits, 3 bits, 2 bits). Accumulator range [1..16].
// Caller passes magnitude bits (0..3 for the first two samples, 0..1 for the third)
// and sign separately since sign-bit position varies per sub-sample.
static inline uint8_t decode_ADPCM_3_sample(uint8_t bits, bool negative) {
    int32_t sign = negative ? -1 : 1;                // sign bit extracted by caller
    int32_t data = bits;                             // 0..3, or 0..1 for the final sample
    int32_t delta =
        (data * sbdsp.adpcm.accum) +
        (sbdsp.adpcm.accum >> 1);                     // scale sample data using accumulator value
    int32_t result = sbdsp.adpcm.reference + sign * delta;   // calculate the next value
    if (result > 0xff) result = 0xff;                // limit value to 0..255
    else if (result < 0) result = 0;
    sbdsp.adpcm.reference = (uint8_t)result;

    if ((data == 0) && (sbdsp.adpcm.accum > 1))       // if input value is 0, and accumulator is
        sbdsp.adpcm.accum >>= 1;                      // larger than 1, then halve accumulator.
    if ((data >= 3) && (sbdsp.adpcm.accum < 0x10))    // if input value is 3, and accumulator is
        sbdsp.adpcm.accum <<= 1;                      // lower than 0x10, then double accumulator.

    return sbdsp.adpcm.reference;
}

// 2-bit: sign (bit 1) + 1 magnitude bit (bit 0). Accumulator range [1..32].
static inline uint8_t decode_ADPCM_2_sample(uint8_t bits) {
    int32_t sign = 1 - (bits & 2);                   // bit 1 is the sign bit (0 -> +1, 1 -> -1)
    int32_t data = bits & 1;                         // the lower bit is the sample data
    int32_t delta =
        (data * sbdsp.adpcm.accum) +
        (sbdsp.adpcm.accum >> 1);                     // scale sample data using accumulator value
    int32_t result = sbdsp.adpcm.reference + sign * delta;   // calculate the next value
    if (result > 0xff) result = 0xff;                // limit value to 0..255
    else if (result < 0) result = 0;
    sbdsp.adpcm.reference = (uint8_t)result;

    if ((data == 0) && (sbdsp.adpcm.accum > 1))       // if input value is 0, and accumulator is
        sbdsp.adpcm.accum >>= 1;                      // larger than 1, then halve accumulator.
    if ((data >= 1) && (sbdsp.adpcm.accum < 0x20))    // if input value is 1, and accumulator is
        sbdsp.adpcm.accum <<= 1;                      // lower than 0x20, then double accumulator.

    return sbdsp.adpcm.reference;
}

// Convert decoded 8-bit unsigned mono sample to packed signed stereo
static inline uint32_t adpcm_to_stereo(uint8_t ref) {
    int16_t s = ((int16_t)(int8_t)(ref ^ 0x80)) << 8;
    return (uint32_t)(uint16_t)s | ((uint32_t)(uint16_t)s << 16);
}

// Ring buffer helpers
static inline uint8_t ring_count() {
    return (uint8_t)(sbdsp.rs.ring_head - sbdsp.rs.ring_tail);
}
static inline uint8_t ring_free() {
    return SB_RING_SIZE - ring_count();
}
static inline bool ring_empty() {
    return sbdsp.rs.ring_head == sbdsp.rs.ring_tail;
}
static inline void ring_push(uint32_t sample) {
    sbdsp.rs.ring[sbdsp.rs.ring_head & (SB_RING_SIZE - 1)] = sample;
    sbdsp.rs.ring_head++;
}
static inline uint32_t ring_pop() {
    uint32_t s = sbdsp.rs.ring[sbdsp.rs.ring_tail & (SB_RING_SIZE - 1)];
    sbdsp.rs.ring_tail++;
    return s;
}
static inline uint8_t samples_per_transfer() {
    switch (sbdsp.adpcm.format) {
        case 4: return 2;
        case 3: return 3;
        case 2: return 4;
        default: return 1;
    }
}

static uint8_t mixer_state[256] = { 0 };
static uint8_t sb_8051_ram[256] = { 0 };

// Wavetable volume pass-through state
static uint8_t *wt_volume_ptr = NULL;
static sbmixer_wtvol_cb_t wtvol_cb = NULL;

void sbdsp_set_wtvol_passthrough(uint8_t *wt_volume, sbmixer_wtvol_cb_t cb) {
    wt_volume_ptr = wt_volume;
    wtvol_cb = cb;
}

static __force_inline void sbdsp_dma_disable(bool pause) {
    sbdsp.dma_enabled = false;
    sbdsp.rs.dma_pending = false;
    // sbdsp.cur_sample = 0; // hold last sample to prevent pops!
    if (!pause) {
        sbdsp.rs.ring_head = sbdsp.rs.ring_tail = 0;
        sbdsp.adpcm.format = 0;
        sbdsp.dma_16bit = false;
        sbdsp.dma_signed = false;
        sbdsp.dma_stereo = false;
    }
}

uint32_t sbdsp_generate_sample() {
    sbdsp.rs.phase_acc += sbdsp.rateratio;
    while (sbdsp.rs.phase_acc >= (1 << SB_RSM_FRAC)) {
        sbdsp.rs.phase_acc -= (1 << SB_RSM_FRAC);

        // advance interpolation buffer state
        sbdsp.rs.interp[1] = sbdsp.rs.interp[0];

        if (!ring_empty()) {
            sbdsp.rs.interp[0] = ring_pop();
        }
        // else: hold last sample (graceful degradation)
    }

    // Restart DMA chain if ring drained and DMA still active
    if (sbdsp.dma_enabled && !sbdsp.rs.dma_pending) {
        uint8_t samples_per_xfer = samples_per_transfer();
        if (ring_free() >= samples_per_xfer) {
            sbdsp.rs.dma_pending = true;
            DMA_Multi_Start_Write(&dma_config, sbdsp.dma_bytes_per_frame);
        }
    }

    // interpolate sample
#ifdef INTERP_SB_LINEAR
    // interp0 blend: BASE0 + alpha * (BASE1 - BASE0) >> 8
    // base01 packs older sample (low 16) and newer sample (high 16), sign-extended
    interp0->accum[1] = sbdsp.rs.phase_acc;

    // left channel: older_L in low 16, newer_L in high 16
    interp0->base01 = (sbdsp.rs.interp[1] & 0xFFFF)
                     | ((sbdsp.rs.interp[0] & 0xFFFF) << 16);
    int32_t out_l = interp0->peek[1];

    // right channel: older_R in low 16, newer_R in high 16
    interp0->base01 = (sbdsp.rs.interp[1] >> 16)
                     | (sbdsp.rs.interp[0] & 0xFFFF0000);
    int32_t out_r = interp0->peek[1];

    return (uint32_t)((out_l & 0xFFFF) | (out_r << 16));
#else
    int32_t l0 = (int16_t)(sbdsp.rs.interp[0] & 0xFFFF);
    int32_t r0 = (int16_t)(sbdsp.rs.interp[0] >> 16);
    int32_t l1 = (int16_t)(sbdsp.rs.interp[1] & 0xFFFF);
    int32_t r1 = (int16_t)(sbdsp.rs.interp[1] >> 16);

    int32_t phase_frac = sbdsp.rs.phase_acc & ((1 << SB_RSM_FRAC) - 1);
    int32_t out_l = ((l0 * phase_frac) + (l1 * ((1 << SB_RSM_FRAC) - phase_frac))) >> SB_RSM_FRAC;
    int32_t out_r = ((r0 * phase_frac) + (r1 * ((1 << SB_RSM_FRAC) - phase_frac))) >> SB_RSM_FRAC;

    return (uint32_t)((out_l & 0xFFFF) | (out_r << 16));
#endif
}

static void sbdsp_handle_time_constant(uint8_t tc) {
    if (sbdsp.options.fixTC) {
        // round time constants to 11/22/44 KHz sample rates
        switch (tc) {
            case (256 - 62): // 16129 Hz
            case (256 - 63): // 15873 Hz
                sbdsp.time_constant = 0; sbdsp.sample_rate = 16000; return;
            case (256 - 31): // 32258 Hz
                sbdsp.time_constant = 0; sbdsp.sample_rate = 32000; return;
            case (256 - 91): // 10989 Hz
            case (256 - 90): // 11111 Hz
                sbdsp.time_constant = 0; sbdsp.sample_rate = 11025; return;
            case (256 - 45): // 22222 Hz
            case (256 - 46): // 21739 Hz
                sbdsp.time_constant = 0; sbdsp.sample_rate = 22050; return;
            case (256 - 22): // 45454 Hz
            case (256 - 23): // 43478 Hz
                sbdsp.time_constant = 0; sbdsp.sample_rate = 44100; return;
            default: break;
        }
    }
    sbdsp.time_constant = tc;
    sbdsp.sample_rate = 0; // Rate of 0 indicates time constant drives DMA timing
}

static __force_inline void sbdsp_set_dma_interval() {
    if (sbdsp.sample_rate) {
        uint32_t actual_sr = (sbdsp.sample_rate >> (sbdsp.dma_stereo_sbpro ? 1 : 0));
        sbdsp.dma_interval = 1000000000ul / actual_sr / 1000;
        sbdsp.rateratio = (actual_sr << SB_RSM_FRAC) / 44100;
    } else {
        sbdsp.dma_interval = (256 - sbdsp.time_constant) << (sbdsp.dma_stereo_sbpro ? 1 : 0);
        sbdsp.rateratio = ((1000000ul / sbdsp.dma_interval) << SB_RSM_FRAC) / 44100;
    }
    // No halving for stereo/16-bit (unless SBPro stereo): dma_write_multi transfers a complete
    // frame per event, so the interval is always one sample period.
}

static __force_inline void sbdsp_dma_enable() {
    if (!sbdsp.dma_enabled) {
        sbdsp.dma_enabled = true;
        // Set autopush bits to number of bits per audio frame.
        // 32 will get masked to 0 by this operation which is correct behavior.
        DMA_Multi_Set_Push_Threshold(&dma_config, sbdsp.dma_bytes_per_frame << 3);
        sbdsp.rs.dma_pending = true;
        DMA_Multi_Start_Write(&dma_config, sbdsp.dma_bytes_per_frame);
    }
}

static void sbdsp_dma_isr(void) {
    // dma_write_multi uses right-shift: bytes fill from MSB down.
    // Complete frame arrives as one push thanks to autopush threshold.
    const uint32_t dma_data = DMA_Complete_Write(&dma_config);
    sbdsp.rs.dma_pending = false;

    // Decode and push to ring buffer.
    // Reference byte case: consumes a DMA byte but pushes no samples.
    if (sbdsp.adpcm.have_ref) {
        sbdsp.adpcm.reference = dma_data >> 24;
        sbdsp.adpcm.accum = 1;  // accumulator starts at 1 in SB DSP firmware
        sbdsp.adpcm.have_ref = false;
    } else if (sbdsp.adpcm.format == 0) {
        // PCM decode (existing logic), push 1 sample
        uint32_t sample;
        if (sbdsp.dma_stereo) {
            if (sbdsp.dma_16bit) {
                sample = dma_data;
            } else {
                // 8-bit stereo: L at [23:16], R at [31:24] to MSB of each 16-bit half
                sample = (dma_data & 0xFF000000) | ((dma_data >> 8) & 0x0000FF00);
            }
        } else {
            if (sbdsp.dma_16bit) {
                // 16-bit mono: duplicate upper 16 to both halves
                sample = (dma_data & 0xFFFF0000) | (dma_data >> 16);
            } else {
                // 8-bit mono: byte at [31:24], duplicate to MSB of each half
                sample = (dma_data & 0xFF000000) | ((dma_data >> 16) & 0x0000FF00);
            }
        }
        if (!sbdsp.dma_signed) sample ^= 0x80008000;
        ring_push(sample);
    } else {
        // ADPCM decode: extract samples from byte, push each
        uint8_t byte = dma_data >> 24;  // ADPCM is always 1-byte mono
        switch (sbdsp.adpcm.format) {
            case 4:  // 4-bit: 2 samples per byte
                ring_push(adpcm_to_stereo(decode_ADPCM_4_sample(byte >> 4)));
                ring_push(adpcm_to_stereo(decode_ADPCM_4_sample(byte & 0xf)));
                break;
            case 3:  // 2.6-bit: 3 samples per byte (3 bits, 3 bits, 2 bits)
                ring_push(adpcm_to_stereo(decode_ADPCM_3_sample((byte >> 5) & 3, byte & 0x80)));
                ring_push(adpcm_to_stereo(decode_ADPCM_3_sample((byte >> 2) & 3, byte & 0x10)));
                ring_push(adpcm_to_stereo(decode_ADPCM_3_sample( byte       & 1, byte & 0x02)));
                break;
            case 2:  // 2-bit: 4 samples per byte
                ring_push(adpcm_to_stereo(decode_ADPCM_2_sample((byte >> 6) & 3)));
                ring_push(adpcm_to_stereo(decode_ADPCM_2_sample((byte >> 4) & 3)));
                ring_push(adpcm_to_stereo(decode_ADPCM_2_sample((byte >> 2) & 3)));
                ring_push(adpcm_to_stereo(decode_ADPCM_2_sample(byte & 3)));
                break;
        }
    }

    // Transfer counting (runs for all cases including reference byte)
    sbdsp.dma_xfer_count_left--;
    if (!sbdsp.dma_xfer_count_left) {
        sbdsp.dma_done = true;
        if (sbdsp.dma_16bit) {
            sbdsp.irq_16_pending = true;
        } else {
            sbdsp.irq_8_pending = true;
        }
        PIC_ActivateIRQ();
        if (sbdsp.autoinit) {
            sbdsp.dma_xfer_count_left = sbdsp.dma_xfer_count;
        } else {
            sbdsp.dma_enabled = false;
            return;
        }
    }

    // Chain next DMA if ring has space
    uint8_t samples_per_xfer = samples_per_transfer();
    if (sbdsp.dma_enabled && ring_free() >= samples_per_xfer) {
        sbdsp.rs.dma_pending = true;
        DMA_Multi_Start_Write(&dma_config, sbdsp.dma_bytes_per_frame);
    }
}

static uint32_t DSP_DAC_Resume_eventHandler(Bitu val) {
    if (sbdsp.dma_16bit) {
        sbdsp.irq_16_pending = true;
    } else {
        sbdsp.irq_8_pending = true;
    }
    PIC_ActivateIRQ();
    sbdsp.dac_resume_pending = false;
    return 0;
}
static PIC_TimerEvent DSP_DAC_Resume_event = {
    .handler = DSP_DAC_Resume_eventHandler,
};

int16_t sbdsp_muted() {
    return (!sbdsp.speaker_on || sbdsp.dac_resume_pending);
}

uint16_t sbdsp_sample_rate() {
    return sbdsp.sample_rate;
}

void sbdsp_set_options(uint8_t options) {
    sbdsp.options.b = options;
    // TODO if any to apply
}

void sbdsp_set_type(uint8_t type) {
    if ((type == 0) && (type > SB_TYPE_SB16)) type = SB_TYPE_SB16;
    switch (type) {
        default:
        case SB_TYPE_SB1:       sbdsp.dsp_version.major = 1; sbdsp.dsp_version.minor = 5; break;
        case SB_TYPE_SB2:       sbdsp.dsp_version.major = 2; sbdsp.dsp_version.minor = 1; break;
        case SB_TYPE_SBPRO1:    sbdsp.dsp_version.major = 3; sbdsp.dsp_version.minor = 0; break;
        case SB_TYPE_SBPRO2:
        case SB_TYPE_SBPRO2MCA: sbdsp.dsp_version.major = 3; sbdsp.dsp_version.minor = 1; break;
        case SB_TYPE_SB16:      sbdsp.dsp_version.major = 4; sbdsp.dsp_version.minor = 5; break;
    }
    // SB16 hardware ignores speaker on/off commands; speaker is always on
    sbdsp.speaker_on = (sbdsp.dsp_version.major >= 4);
}

void sbdsp_set_irq(uint8_t irq) {
    sbdsp.resources.irq = irq;
    int mixer_irq;
    switch (irq) {
        case 2:
        case 9:  mixer_irq = 1; break;
        case 7:  mixer_irq = 4; break;
        case 10: mixer_irq = 8; break;
        case 5:
        default: mixer_irq = 2; break;
    }
    mixer_state[MIXER_INTERRUPT] = mixer_irq | 0xF0;  // as seen on real SB16
}

void sbdsp_set_dma(uint8_t dma) {
    sbdsp.resources.dma = dma;
    mixer_state[MIXER_DMA] = dma < 4 ? (1 << dma) : 0;  // 8-bit DMA channel only
}

static void sbmixer_reset(void);
void sbdsp_init() {
    puts("Initing ISA DMA PIO...");
    SBDSP_DMA_isr_pt = sbdsp_dma_isr;

    sbdsp.outbox = 0xAA;
    dma_config = DMA_multi_init(pio0, DMA_PIO_SM, SBDSP_DMA_isr_pt);

    // Initialize 8051 RAM with SB16 default values (per DOSBox-X)
    sb_8051_ram[0x0e] = 0xff;
    sb_8051_ram[0x0f] = 0x07;
    sb_8051_ram[0x37] = 0x38;

    // Initialize buffer for the E2 command
    sbdsp.ident_e2[0] = 0xAA;
    sbdsp.ident_e2[1] = 0x96;
    
    // Reset mixer
    sbmixer_reset();

#ifdef INTERP_SB_LINEAR
    // Configure interp0 blend mode for SB DSP PCM linear interpolation.
    // Lane 0: blend=true. Lane 1: shift maps 12-bit phase fraction to 8-bit alpha.
    {
        interp_config cfg = interp_default_config();
        interp_config_set_blend(&cfg, true);
        interp_set_config(interp0, 0, &cfg);

        cfg = interp_default_config();
        interp_config_set_shift(&cfg, SB_RSM_FRAC - 8);
        interp_config_set_signed(&cfg, true);
        interp_set_config(interp0, 1, &cfg);
    }
#endif
}


static __force_inline void sbdsp_output(uint8_t value) {
    sbdsp.outbox = value;
    sbdsp.dav_pc = 1;
}

// start ADPCM DMA transfer
static void sbdsp_start_adpcm_dma(int autoinit, uint16_t xfer_size, uint8_t format, bool with_ref) {
    sbdsp.autoinit = autoinit;
    sbdsp.adpcm.format = format;
    sbdsp.adpcm.have_ref = with_ref;
    sbdsp.adpcm.accum = 1;  // SB DSP firmware initializes accumulator to 1 at DMA start
                            // (for have_ref it's overwritten again when the ref byte arrives)
    sbdsp.dma_16bit  = false;
    sbdsp.dma_signed = false;
    sbdsp.dma_stereo = false;
    sbdsp.dma_stereo_sbpro = false;
    sbdsp.dma_bytes_per_frame = 1;  // ADPCM always 1 byte at a time
    sbdsp.dma_xfer_count = xfer_size + 1;  // byte count
    sbdsp.dma_xfer_count_left = sbdsp.dma_xfer_count;
    sbdsp_set_dma_interval();
    sbdsp.dma_done = false;
    sbdsp_dma_enable();
}

// start pre-SB16 PCM DMA transfer
static void sbdsp_start_pcm_dma(int autoinit, uint16_t xfer_size) {
    sbdsp.autoinit = autoinit;
    sbdsp.adpcm.format = 0;  // ensure PCM mode

    bool dma_stereo = (mixer_state[0x0E] & 2);
    if (dma_stereo && ((xfer_size + 1) == 1)) {
        // HACK: many SBPro programs follow Creative's documentation and perform 1 byte single-cycle
        // DMA transfer to seemingly reset the mixer stereo state, then waiting for IRQ to come.
        // since the sbdsp.dma_xfer_count calculation divides by 2 for stereo case, it results in
        // xfer_count==0, so the transfer never actually occurs.
        // for those transfers, force mono mode.
        dma_stereo = false;
    }
    sbdsp.dma_16bit  = false;       // always 8 bit
    sbdsp.dma_signed = false;       // always unsigned
    sbdsp.dma_stereo = dma_stereo;
    sbdsp.dma_stereo_sbpro = dma_stereo;
    sbdsp.dma_bytes_per_frame = sbdsp.dma_stereo ? 2 : 1;
    sbdsp.dma_xfer_count = (xfer_size + 1) / sbdsp.dma_bytes_per_frame;
    sbdsp.dma_xfer_count_left = sbdsp.dma_xfer_count;
    sbdsp_set_dma_interval();
    sbdsp.dma_done = false;
    sbdsp_dma_enable();
}

void sbdsp_process(void) {
    if (sbdsp.reset_state) return;
    sbdsp.dsp_busy = 1;

    if (sbdsp.dav_dsp) {
        if (!sbdsp.current_command) {
            sbdsp.current_command = sbdsp.inbox;
            sbdsp.current_command_index = 0;
            sbdsp.dav_dsp = 0;
        }
    }

    switch (sbdsp.current_command) {
        case DSP_PAUSE_DMA_8:
        case DSP_PAUSE_DMA_16:
            sbdsp.current_command = 0;
            sbdsp_dma_disable(true);
            break;
        case DSP_CONTINUE_DMA_8:
        case DSP_CONTINUE_DMA_16:
            sbdsp.current_command = 0;
            sbdsp.dma_done = false;
            sbdsp_dma_enable();
            break;
        case DSP_CONTINUE_AUTOINIT_8:
        case DSP_CONTINUE_AUTOINIT_16:
            sbdsp.autoinit = 1;
            sbdsp.current_command = 0;
            break;
        case DSP_EXIT_AUTOINIT_8:
        case DSP_EXIT_AUTOINIT_16:
            sbdsp.autoinit = 0;
            sbdsp.current_command = 0;
            break;
        case DSP_DMA_AUTO:
            sbdsp.dav_dsp = 0;
            sbdsp.current_command = 0;    
            sbdsp_start_pcm_dma(1, sbdsp.dma_block_size);
            break;
        case DSP_DMA_HS_AUTO:
            sbdsp.dav_dsp = 0;
            sbdsp.current_command = 0;
            sbdsp_start_pcm_dma(1, sbdsp.dma_block_size);
            break;

        case DSP_SET_TIME_CONSTANT:
            if (sbdsp.dav_dsp) {
                if (sbdsp.current_command_index == 1) {
                    sbdsp_handle_time_constant(sbdsp.inbox);
                    sbdsp_set_dma_interval();
                    sbdsp.dav_dsp = 0;
                    sbdsp.current_command = 0;
                }
                sbdsp.current_command_index++;
            }
            break;
        case DSP_SET_SAMPLING_RATE:     // set playback  sample rate
        case DSP_SET_SAMPLING_RATE+1:   // set recording sample rate - misused by FT2
            if (sbdsp.dav_dsp) {
                if (sbdsp.current_command_index == 1) { // wSamplingRate.HighByte
                    sbdsp.sample_rate = sbdsp.inbox << 8;
                    sbdsp.dav_dsp = 0;
                } else if (sbdsp.current_command_index == 2) { // wSamplingRate.LowByte
                    sbdsp.sample_rate |= sbdsp.inbox;
                    sbdsp.time_constant = 0;
                    sbdsp_set_dma_interval();
                    sbdsp.dav_dsp = 0;
                    sbdsp.current_command = 0;
                }
                sbdsp.current_command_index++;
            }
            break;
        case DSP_DMA_BLOCK_SIZE:
            if (sbdsp.dav_dsp) {
                if (sbdsp.current_command_index == 1) {
                    sbdsp.dma_block_size = sbdsp.inbox;
                    sbdsp.dav_dsp = 0;
                } else if (sbdsp.current_command_index == 2) {
                    sbdsp.dma_block_size += (sbdsp.inbox << 8);
                    sbdsp.dav_dsp = 0;
                    sbdsp.current_command = 0;
                }
                sbdsp.current_command_index++;
            }
            break;

        case DSP_DMA_HS_SINGLE:
            sbdsp.dav_dsp = 0;
            sbdsp.current_command = 0;
            sbdsp_start_pcm_dma(0, sbdsp.dma_block_size);
            break;
        case DSP_DMA_SINGLE:
            if (sbdsp.dav_dsp) {
                if (sbdsp.current_command_index == 1) {
                    sbdsp.dma_sample_count = sbdsp.inbox;
                    sbdsp.dav_dsp = 0;
                } else if (sbdsp.current_command_index == 2) {
                    sbdsp.dma_sample_count += (sbdsp.inbox << 8);
                    sbdsp.dav_dsp = 0;
                    sbdsp.current_command = 0;
                    sbdsp_start_pcm_dma(0, sbdsp.dma_sample_count);
                }
                sbdsp.current_command_index++;
            }
            break;
        // Single-cycle ADPCM (2 param bytes: length-1 low, length-1 high)
        case 0x74: case 0x75:  // 4-bit ADPCM / with ref
        case 0x76: case 0x77:  // 2.6-bit ADPCM / with ref
        case 0x16: case 0x17:  // 2-bit ADPCM / with ref
            if (sbdsp.dav_dsp) {
                if (sbdsp.current_command_index == 1) {
                    sbdsp.dma_sample_count = sbdsp.inbox;
                    sbdsp.dav_dsp = 0;
                } else if (sbdsp.current_command_index == 2) {
                    sbdsp.dma_sample_count += (sbdsp.inbox << 8);
                    uint8_t cmd = sbdsp.current_command;
                    uint8_t fmt = (cmd >= 0x74) ? ((cmd <= 0x75) ? 4 : 3) : 2;
                    bool ref = cmd & 1;
                    sbdsp.dav_dsp = 0;
                    sbdsp.current_command = 0;
                    sbdsp_start_adpcm_dma(0, sbdsp.dma_sample_count, fmt, ref);
                }
                sbdsp.current_command_index++;
            }
            break;
        // Auto-init ADPCM (no params, use dma_block_size)
        case 0x7D:  // 4-bit ADPCM auto-init
            sbdsp.dav_dsp = 0;
            sbdsp.current_command = 0;
            sbdsp_start_adpcm_dma(1, sbdsp.dma_block_size, 4, true);
            break;
        case 0x7F:  // 2.6-bit ADPCM auto-init
            sbdsp.dav_dsp = 0;
            sbdsp.current_command = 0;
            sbdsp_start_adpcm_dma(1, sbdsp.dma_block_size, 3, true);
            break;
        case 0x1F:  // 2-bit ADPCM auto-init
            sbdsp.dav_dsp = 0;
            sbdsp.current_command = 0;
            sbdsp_start_adpcm_dma(1, sbdsp.dma_block_size, 2, true);
            break;

        case DSP_IRQ_8:
            sbdsp.current_command = 0;
            sbdsp.irq_8_pending = true;
            PIC_ActivateIRQ();
            break;
        case DSP_IRQ_16:
            sbdsp.current_command = 0;
            sbdsp.irq_16_pending = true;
            PIC_ActivateIRQ();
            break;
        case DSP_VERSION:
            if (sbdsp.current_command_index == 0) {
                sbdsp.current_command_index = 1;
                sbdsp_output(sbdsp.dsp_version.major);
            } else {
                if (!sbdsp.dav_pc) {
                    sbdsp.current_command = 0;
                    sbdsp_output(sbdsp.dsp_version.minor);
                }
            }
            break;
        case DSP_IDENT:
            if (sbdsp.dav_dsp) {
                if (sbdsp.current_command_index == 1) {
                    sbdsp.dav_dsp = 0;
                    sbdsp.current_command = 0;
                    sbdsp_output(~sbdsp.inbox);
                }
                sbdsp.current_command_index++;
            }
            break;
        case DSP_IDENT_E2: // yet another "protection"... used by CT-VOICE.DRV
            if (sbdsp.dav_dsp) {
                if (sbdsp.current_command_index == 1) {
                    sbdsp.dav_dsp = 0;
                    sbdsp.current_command = 0;
                    sbdsp.ident_e2[0] += sbdsp.inbox ^ sbdsp.ident_e2[1];
                    sbdsp.ident_e2[1] = (sbdsp.ident_e2[1] >> 2) | (sbdsp.ident_e2[1] << 6);
                    // ideally it should output this byte via DMA, but we don't support DMA IORs yet
                    // so stuff to outbox for now
                    sbdsp_output(sbdsp.ident_e2[0]);
                }
                sbdsp.current_command_index++;
            }
            break;
        case DSP_COPYRIGHT:
            if (sbdsp.current_command_index == 0) {
                sbdsp.current_command_index = 1;
                sbdsp_output(copyright_string[0]);
            } else {
                if (!sbdsp.dav_pc) {
                    if (copyright_string[sbdsp.current_command_index] == '\0') {
                        sbdsp_output('\0');
                        sbdsp.current_command = 0;
                    } else {
                        sbdsp_output(copyright_string[sbdsp.current_command_index]);
                    }
                    sbdsp.current_command_index++;
                }
            }
            break;
        case DSP_ENABLE_SPEAKER:
            if (sbdsp.dsp_version.major < 4) sbdsp.speaker_on = true;
            sbdsp.current_command = 0;
            break;
        case DSP_DISABLE_SPEAKER:
            if (sbdsp.dsp_version.major < 4) sbdsp.speaker_on = false;
            sbdsp.current_command = 0;
            break;
        case DSP_SPEAKER_STATUS:
            if (sbdsp.current_command_index == 0) {
                sbdsp.current_command = 0;
                sbdsp_output(sbdsp.speaker_on ? 0xff : 0x00);
            }
            break;
        case DSP_DIRECT_DAC:
            if (sbdsp.dav_dsp) {
                if (sbdsp.current_command_index == 1) {
                    int16_t s = ((int16_t)(int8_t)(sbdsp.inbox ^ 0x80)) << 8;
                    sbdsp.cur_sample = (uint32_t)(uint16_t)s | ((uint32_t)(uint16_t)s << 16);
                    sbdsp.dav_dsp = 0;
                    sbdsp.current_command = 0;
                }
                sbdsp.current_command_index++;
            }
            break;
        case DSP_DIRECT_ADC:
            if (sbdsp.current_command_index == 0) {
                sbdsp.current_command = 0;
                sbdsp_output(0x80);         // fake silent input
            }
            break;
        case DSP_WRITETEST:
            if (sbdsp.dav_dsp) {
                if (sbdsp.current_command_index == 1) {
                    sbdsp.test_register = sbdsp.inbox;
                    sbdsp.dav_dsp = 0;
                    sbdsp.current_command = 0;
                }
                sbdsp.current_command_index++;
            }
            break;
        case DSP_READTEST:
            if (sbdsp.current_command_index == 0) {
                sbdsp.current_command = 0;
                sbdsp_output(sbdsp.test_register);
            }
            break;

        case DSP_DAC_PAUSE_DURATION:
            if (sbdsp.dav_dsp) {
                if (sbdsp.current_command_index == 1) {
                    sbdsp.dac_pause_duration_low = sbdsp.inbox;
                    sbdsp.dav_dsp = 0;
                } else if (sbdsp.current_command_index == 2) {
                    sbdsp.dac_pause_duration = sbdsp.dac_pause_duration_low + (sbdsp.inbox << 8);
                    sbdsp.dac_resume_pending = true;
                    // length of pause is samples - 1, so add 1
                    PIC_AddEvent(&DSP_DAC_Resume_event, sbdsp.dma_interval * (sbdsp.dac_pause_duration + 1), 0);
                    sbdsp.dav_dsp = 0;
                    sbdsp.current_command = 0;
                }
                sbdsp.current_command_index++;
            }
            break;
        case DSP_8051_READ:
            if (sbdsp.dav_dsp) {
                if (sbdsp.current_command_index == 1) {
                    sbdsp.dav_dsp = 0;
                    sbdsp.current_command = 0;
                    sbdsp_output(sb_8051_ram[sbdsp.inbox]);
                }
                sbdsp.current_command_index++;
            }
            break;
        case DSP_8051_WRITE:
            if (sbdsp.dav_dsp) {
                if (sbdsp.current_command_index == 1) {
                    sb_8051_ram[0xff] = sbdsp.inbox; // stash address in unused location
                    sbdsp.dav_dsp = 0;
                } else if (sbdsp.current_command_index == 2) {
                    sb_8051_ram[sb_8051_ram[0xff]] = sbdsp.inbox;
                    sbdsp.dav_dsp = 0;
                    sbdsp.current_command = 0;
                }
                sbdsp.current_command_index++;
            }
            break;
        case DSP_DMA_IO_START ... DSP_DMA_IO_END:
            // SB16 DMA transfer
            if (sbdsp.dav_dsp) {
                if (sbdsp.current_command_index == 0) { // bCommand + bMode
                    sbdsp.adpcm.format = 0;  // SB16 commands are always PCM
                    sbdsp.dma_16bit = (sbdsp.current_command & 0xf0) == 0xb0;
                    sbdsp.autoinit = sbdsp.current_command & 0x4;
                    sbdsp.dma_signed = sbdsp.inbox & 0x10;
                    sbdsp.dma_stereo = sbdsp.inbox & 0x20;
                    sbdsp.dma_stereo_sbpro = 0;
                    sbdsp.dav_dsp = 0;
                } else if (sbdsp.current_command_index == 1) { // wLength.LowByte
                    sbdsp.dma_sample_count = sbdsp.inbox;
                    sbdsp.dav_dsp = 0;
                } else if (sbdsp.current_command_index == 2) { // wLength.HighByte
                    sbdsp.dma_sample_count |= (sbdsp.inbox << 8);
                    // HACK: force mono when stereo halving would round the
                    // count to 0 (e.g. count=0 -> 1 sample -> 0 frames).
                    // Miles AIL detection triggers this: 16-bit signed
                    // stereo single-cycle DMA with a 1-sample transfer.
                    // Same logic as the SBPro hack in sbdsp_start_pcm_dma()
                    if (sbdsp.dma_stereo && (sbdsp.dma_sample_count + 1) == 1) {
                        sbdsp.dma_stereo = false;
                    }
                    sbdsp.dma_bytes_per_frame = 1;
                    if (sbdsp.dma_16bit) {
                        sbdsp.dma_bytes_per_frame <<= 1;
                    }
                    if (sbdsp.dma_stereo) {
                        sbdsp.dma_bytes_per_frame <<= 1;
                    }
                    // SB16: sample_count counts individual DMA transfers minus 1.
                    // Each DMA event transfers one complete frame, so for stereo
                    // we halve the count (2 DMA words per stereo frame).
                    sbdsp.dma_xfer_count = sbdsp.dma_stereo
                        ? (sbdsp.dma_sample_count + 1) >> 1
                        : sbdsp.dma_sample_count + 1;
                    sbdsp.dma_xfer_count_left = sbdsp.dma_xfer_count;
                    sbdsp.dav_dsp = 0;
                    sbdsp.current_command = 0;
                    sbdsp.speaker_on = true;
                    sbdsp_set_dma_interval();
                    sbdsp.dma_done = false;
                    sbdsp_dma_enable();
                }
                sbdsp.current_command_index++;
            }
            break;
        case DSP_ASP_SET_CODEC_PARAM:
        case DSP_ASP_SET_REGISTER:
            // 2 data bytes, accept and ignore (no ASP on ViBRA)
            if (sbdsp.dav_dsp) {
                if (sbdsp.current_command_index == 1) {
                    sbdsp.dav_dsp = 0;
                } else if (sbdsp.current_command_index == 2) {
                    sbdsp.dav_dsp = 0;
                    sbdsp.current_command = 0;
                }
                sbdsp.current_command_index++;
            }
            break;
        case DSP_ASP_GET_REGISTER:
            // 1 data byte. Returns chip identification values for specific registers.
            if (sbdsp.dav_dsp) {
                if (sbdsp.current_command_index == 1) {
                    sbdsp.dav_dsp = 0;
                    sbdsp.current_command = 0;
                    uint8_t val;
                    switch (sbdsp.inbox) {
                        case 0x05: val = 0x01; break;
                        case 0x09: val = 0xF8; break;
                        case 0x83: val = 0xFF; break;
                        default:   val = 0x00; break;
                    }
                    sbdsp_output(val);
                }
                sbdsp.current_command_index++;
            }
            break;
        // SB MIDI commands
        case DSP_MIDI_WRITE_POLL: // 0x38: write MIDI byte
            if (sbdsp.dav_dsp) {
                if (sbdsp.current_command_index == 1) {
#ifdef SOUND_MPU
                    MIDI_RawOutByte(sbdsp.inbox);
#endif
                    sbdsp.dav_dsp = 0;
                    sbdsp.current_command = 0;
                }
                sbdsp.current_command_index++;
            }
            break;
        case DSP_MIDI_READ_POLL: // 0x30: read MIDI byte
            if (sbdsp.current_command_index == 0) {
                sbdsp.current_command = 0;
                sbdsp_output(0x00); // no MIDI input
            }
            break;
        case 0x31: // MIDI Read Interrupt - accept and ignore
        case 0xA0: // Disable Stereo Input Mode
        case 0xA8: // Enable Stereo Input Mode
            sbdsp.current_command = 0;
            break;
        case 0x34: // MIDI UART read+write poll
        case 0x35: // MIDI UART read interrupt+Write poll
            sbdsp.midi_uart_mode = true;
            sbdsp.current_command = 0;
            break;
            sbdsp.current_command = 0;
            break;
        case 0:
            //not in a command
            break;
        default:
            //printf("Unknown Command: %x\n", sbdsp.current_command);
            sbdsp.current_command = 0;
            break;
    }
    sbdsp.dsp_busy = 0;
}

static uint32_t DSP_Reset_EventHandler(Bitu val) {
    sbdsp.current_command = 0;
    sbdsp.current_command_index = 0;

    sbdsp.dma_block_size = 0x7FF; //default per 2.01
    sbdsp.dma_xfer_count = 0;
    sbdsp.dma_xfer_count_left = 0;
    sbdsp.dma_stereo = false;
    sbdsp.dma_stereo_sbpro = false;
    sbdsp.dma_signed = false;
    sbdsp.speaker_on = (sbdsp.dsp_version.major >= 4); // SB16 speaker always on
    sbdsp.dma_done = false;
    sbdsp.dac_resume_pending = false;
    memset(&sbdsp.adpcm, 0, sizeof(sbdsp.adpcm));
    sbdsp.rs.ring_head = sbdsp.rs.ring_tail = 0;

    sbdsp.outbox = 0xAA;
    sbdsp.dav_pc = 1;
    sbdsp.reset_state = 0;
    return 0;
}
static PIC_TimerEvent DSP_Reset_Event = {
    .handler = DSP_Reset_EventHandler,
};

static __force_inline void sbdsp_reset(uint8_t value) {
    value &= 1; // Some games may write unknown data for bits other than the LSB.
    switch (value) {
        case 1:
            PIC_RemoveEvent(&DSP_Reset_Event);
            while (sbdsp.dsp_busy) tight_loop_contents();   // TODO timeout!!!!!!
            sbdsp.reset_state = 1;
            sbdsp.autoinit = 0;
            sbdsp.midi_uart_mode = false;
            sbdsp_dma_disable(false);
            break;
        case 0:
            if (sbdsp.reset_state == 1) {
                sbdsp.reset_state = 2;
                sbdsp.dav_pc = 0;
                PIC_RemoveEvent(&DSP_Reset_Event);
                PIC_AddEvent(&DSP_Reset_Event, 100, 0);
            }
            break;
        default:
            break;
    }
}

// --------------
// SB Mixer functions

#if 1
// volume table, 0.12 fixedpoint, -1.5dB per step
static const int32_t sbmixer_voltab[32] = {
        0,     9,    15,    21,    29,    39,    51,    65,  // vol 0..7
       77,    92,   109,   130,   154,   183,   217,   258,  // vol 8..15
      307,   365,   434,   516,   613,   728,   866,  1029,  // vol 16..23
     1223,  1453,  1727,  2053,  2440,  2900,  3446,  4096   // vol 24..31
};
#else
// volume table, 0.12 fixedpoint, -2dB per step (matches SB16 mixer scale?)
static const int32_t sbmixer_voltab[32] = {
        0,     2,     4,     6,     8,    10,    13,    16,  // vol 0..7
       21,    26,    33,    41,    52,    65,    82,   103,  // vol 8..15
      130,   163,   205,   258,   325,   410,   516,   649,  // vol 16..23
      817,  1029,  1295,  1631,  2053,  2584,  3254,  4096   // vol 24..31
};
#endif

// set SB16 output switches
static void sbmixer_out_switch(uint8_t value) {
    mixer_state[MIXER_OUT_SWITCH] = value;

    volume.cd_audio[1] = (value & (1 << 1)) ? sbmixer_voltab[mixer_state[MIXER_VOL_CD_R] >> 3] : 0;
    volume.cd_audio[0] = (value & (1 << 2)) ? sbmixer_voltab[mixer_state[MIXER_VOL_CD_L] >> 3] : 0;
    
    // ignore Mic/Line at this moment
}

// set volume using SBPro mixer registers
static void sbmixer_pro_set(uint8_t sbpro_reg, uint8_t value) {
    mixer_state[sbpro_reg] = value;

    // update SB16 and actual mixer registers
    int vol_r = ((value >> 0) & 0xF), vol_l = (value >> 4) & 0xF;
    vol_l = (vol_l << 1) | (vol_l >> 3);
    vol_r = (vol_r << 1) | (vol_r >> 3);

    int     sb16_reg = 0;
    int32_t *volreg  = NULL;

    switch (sbpro_reg) {
        case MIXER_VOL_MASTER: sb16_reg = MIXER_VOL_MASTER_L; volreg = &volume.sb_master[0]; break;
        case MIXER_VOL_VOICE:  sb16_reg = MIXER_VOL_VOICE_L;  volreg = &volume.sb_pcm[0];    break;
        case MIXER_VOL_MIDI:   sb16_reg = MIXER_VOL_MIDI_L;   volreg = &volume.opl[0];       break;
        case MIXER_VOL_CD:     sb16_reg = MIXER_VOL_CD_L;     volreg = (mixer_state[MIXER_OUT_SWITCH] & (3 << 1)) ? &volume.cd_audio[0] : NULL; break;
        case MIXER_VOL_LINE:   sb16_reg = MIXER_VOL_LINE_L;   volreg = NULL;  break;
        default: break;
    }

    if (sb16_reg) {
        mixer_state[sb16_reg + 0] = vol_l << 3;
        mixer_state[sb16_reg + 1] = vol_r << 3;
    }
    if (volreg) {
        volreg[0] = sbmixer_voltab[vol_l];
        volreg[1] = sbmixer_voltab[vol_r];
    }
    if (sbpro_reg == MIXER_VOL_LINE && wtvol_cb) {
        uint8_t vol5 = (vol_l > vol_r) ? vol_l : vol_r;
        wtvol_cb(vol5 * 100 / 31);
    }
}

// set volume using SB16 mixer registers
static void sbmixer_16_set(uint8_t sb16_reg, uint8_t value) {
    mixer_state[sb16_reg] = value & 0xF8;

    int sbpro_reg = 0;
    int32_t *volreg  = NULL;

    switch (sb16_reg) {
        case MIXER_VOL_MASTER_L: sbpro_reg = MIXER_VOL_MASTER; volreg = &volume.sb_master[0]; break;
        case MIXER_VOL_MASTER_R: sbpro_reg = MIXER_VOL_MASTER; volreg = &volume.sb_master[1]; break;

        case MIXER_VOL_VOICE_L:  sbpro_reg = MIXER_VOL_VOICE;  volreg = &volume.sb_pcm[0]; break;
        case MIXER_VOL_VOICE_R:  sbpro_reg = MIXER_VOL_VOICE;  volreg = &volume.sb_pcm[1]; break;

        case MIXER_VOL_MIDI_L:   sbpro_reg = MIXER_VOL_MIDI;   volreg = &volume.opl[0]; break;
        case MIXER_VOL_MIDI_R:   sbpro_reg = MIXER_VOL_MIDI;   volreg = &volume.opl[1]; break;

        case MIXER_VOL_CD_L:     sbpro_reg = MIXER_VOL_CD;     volreg = (mixer_state[MIXER_OUT_SWITCH] & (1 << 2)) ? &volume.cd_audio[0] : NULL; break;
        case MIXER_VOL_CD_R:     sbpro_reg = MIXER_VOL_CD;     volreg = (mixer_state[MIXER_OUT_SWITCH] & (1 << 1)) ? &volume.cd_audio[1] : NULL; break;

        case MIXER_VOL_LINE_L:   sbpro_reg = MIXER_VOL_LINE;   volreg = NULL; break;
        case MIXER_VOL_LINE_R:   sbpro_reg = MIXER_VOL_LINE;   volreg = NULL; break;
        default: break;
    }

    if (sbpro_reg) {
        int regmask = (sb16_reg & 1) ? 0x0F : 0xF0;
        int proval  = (value & 0xF0) >> ((sb16_reg & 1) ? 0 : 4);
        mixer_state[sbpro_reg] = (mixer_state[sbpro_reg] & regmask) | proval;
    }
    if (volreg) {
        *volreg = sbmixer_voltab[value >> 3];
    }
    if ((sb16_reg == MIXER_VOL_LINE_L || sb16_reg == MIXER_VOL_LINE_R) && wtvol_cb) {
        wtvol_cb((value >> 3) * 100 / 31);
    }
}

static void sbmixer_reset(void) {
    // preserve IRQ/DMA select registers across mixer reset
    uint8_t saved_irq = mixer_state[MIXER_INTERRUPT];
    uint8_t saved_dma = mixer_state[MIXER_DMA];
    memset(mixer_state, 0, sizeof(mixer_state));
    mixer_state[MIXER_INTERRUPT] = saved_irq;
    mixer_state[MIXER_DMA] = saved_dma;

    // initialize default values
    sbmixer_pro_set(MIXER_VOL_MASTER, 0xFF);
    sbmixer_pro_set(MIXER_VOL_VOICE,  0xEE);
    sbmixer_pro_set(MIXER_VOL_MIDI,   0xEE);
    sbmixer_pro_set(MIXER_VOL_CD,     0xEE);
    sbmixer_pro_set(MIXER_VOL_LINE,   0x00);
    sbmixer_out_switch(0x6); // CD L/R enabled, Line/Mic disabled

    // preinit (non-functional) bass/treble settings
    mixer_state[0x44] = mixer_state[0x45] = mixer_state[0x46] = mixer_state[0x47] = 0x80;
}

static __force_inline uint8_t sbmixer_read(void) {
    switch (sbdsp.mixer_command) {
        case MIXER_IRQ_STATUS:
            // Bits 0-2: IRQ pending flags. Upper bits: ViBRA type identifier.
            return (sbdsp.irq_8_pending ? 0x01 : 0) | (sbdsp.irq_16_pending ? 0x02 : 0) | 0x70;
        case MIXER_VOL_LINE_L:
        case MIXER_VOL_LINE_R:
            if (wt_volume_ptr) {
                return (uint8_t)((*wt_volume_ptr * 31 / 100) << 3);
            }
            return mixer_state[sbdsp.mixer_command];
        case MIXER_VOL_LINE:
            if (wt_volume_ptr) {
                uint8_t nibble = *wt_volume_ptr * 15 / 100;
                return (nibble << 4) | nibble;
            }
            return mixer_state[sbdsp.mixer_command];
        case 0x8E:
            return mixer_state[0x8E] | 0xC0;
        case 0xFF:
            return 0xFF;
        default:
            return mixer_state[sbdsp.mixer_command];
    }
}

static __force_inline void sbmixer_write(uint8_t value) {
    // mixer locking
    switch(sbdsp.options.lockMixer) {
        case MIXER_LOCK_ALL_BUT_SBP_VOICE:
            if (sbdsp.mixer_command != MIXER_VOL_VOICE)
            // fallthrough
        case MIXER_LOCK_ALL:
            return;
        default:
            break;
    }

    switch (sbdsp.mixer_command) {
        case 0x00: // Mixer reset
            sbmixer_reset();
            break;
        case 0x02: // SBPro Master Volume alias (chains to 0x22)
            mixer_state[0x02] = value;
            sbmixer_pro_set(MIXER_VOL_MASTER, value);
            break;
        case MIXER_VOL_MASTER:
        case MIXER_VOL_MIDI:
        case MIXER_VOL_CD:
        case MIXER_VOL_LINE:
        case MIXER_VOL_VOICE:
            sbmixer_pro_set(sbdsp.mixer_command, value);
            break;
        case MIXER_VOL_MASTER_L ... MIXER_VOL_LINE_R:
            sbmixer_16_set(sbdsp.mixer_command, value);
            break;
        case MIXER_OUT_SWITCH:
            sbmixer_out_switch(value);
            break;
        case MIXER_INTERRUPT:
            // sbdsp.interrupt = value;
            break;
        case MIXER_DMA:
            // sbdsp.dma = value;
            break;
        case 0xFF:
            break;
        case MIXER_STEREO:
            //sbdsp.dma_stereo = value & 2;
            // no break
        default:
            mixer_state[sbdsp.mixer_command] = value;
            break;
    }
}


uint8_t sbdsp_read(uint8_t address) {
    uint8_t out = 0xFF;

    // emulate address aliasing seen on pre-SB16
    switch (address) {
        case DSP_READ:   // +0xA
        case DSP_READ+1: // +0xB
            out = sbdsp.outbox;
            sbdsp.dav_pc = 0;
            return out;
        case DSP_READ_STATUS: // +0xE
            if (sbdsp.irq_8_pending) {
                sbdsp.irq_8_pending = false;
                PIC_DeActivateIRQ();
            }
            return (sbdsp.dav_pc << 7) | DSP_UNUSED_STATUS_BITS_PULLED_HIGH;
        case DSP_IRQ_16_STATUS: // +0xF
            if (sbdsp.irq_16_pending) {
                sbdsp.irq_16_pending = false;
                PIC_DeActivateIRQ();
            }
            return 0xff;
        case DSP_WRITE_STATUS:  // +0xC
        case DSP_WRITE_STATUS+1:// +0xD
            if (sbdsp.reset_state)
                return 0xFF; // busy during reset
            return ((sbdsp.dav_dsp | sbdsp.dsp_busy | sbdsp.dac_resume_pending) << 7) | DSP_UNUSED_STATUS_BITS_PULLED_HIGH;
        case MIXER_DATA:
            return sbmixer_read();
        default:
            return 0xFF;
    }
}

void sbdsp_write(uint8_t address, uint8_t value) {
    switch (address) {
        case DSP_WRITE:   // 0xC
        case DSP_WRITE+1: // 0xD
            // in MIDI UART mode, all writes to DSP data port are MIDI output
            if (sbdsp.midi_uart_mode) {
#ifdef SOUND_MPU
                MIDI_RawOutByte(value);
#endif
                break;
            }
            if (sbdsp.dav_dsp) printf("WARN - DAV_DSP OVERWRITE\n");
            sbdsp.inbox = value;
            sbdsp.dav_dsp = 1;
            break;
        case DSP_RESET:   // 0x6
        case DSP_RESET+1: // 0x7
            sbdsp_reset(value);
            break;
        case MIXER_COMMAND:
            sbdsp.mixer_command = value;
            break;
        case MIXER_DATA:
            sbmixer_write(value);
            break;
        default:
            break;
    }
}
