#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "system/pico_pic.h"
#include "audio/volctrl.h"
#include "sbdsp.h"

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

#include "isa/isa_dma.h"

#ifndef MAX
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#endif

#define SB_RSM_FRAC 10

static irq_handler_t SBDSP_DMA_isr_pt;
static dma_inst_t dma_config;
#define DMA_PIO_SM 2

#define DSP_VERSION_MAJOR 4
#define DSP_VERSION_MINOR 5

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
#define DSP_DMA_ADPCM           0x7F    //creative ADPCM 8bit to 3bit
#define DSP_DMA_SINGLE          0x14    //follosed by length
#define DSP_DMA_AUTO            0X1C    //length based on 48h
#define DSP_DMA_BLOCK_SIZE      0x48    //block size for highspeed/dma

// SB16 DSP commands
#define DSP_DMA_IO_START        0xB0
#define DSP_DMA_IO_END          0xCF
#define DSP_PAUSE_DMA_16        0xD5
#define DSP_CONTINUE_DMA_16     0x47
#define DSP_CONTINUE_DMA_8      0x45
#define DSP_EXIT_DMA_16         0xD9
#define DSP_EXIT_DMA_8          0xDA

//#define DSP_DMA_DAC 0x14
#define DSP_DIRECT_DAC          0x10
#define DSP_DIRECT_ADC          0x20
#define DSP_MIDI_READ_POLL      0x30
#define DSP_MIDI_WRITE_POLL     0x38
#define DSP_SET_TIME_CONSTANT   0x40
#define DSP_SET_SAMPLING_RATE   0x41
#define DSP_DMA_PAUSE           0xD0
#define DSP_DAC_PAUSE_DURATION  0x80    // Pause DAC for a duration, then generate an interrupt. Used by Tyrian.
#define DSP_ENABLE_SPEAKER      0xD1
#define DSP_DISABLE_SPEAKER     0xD3
#define DSP_DMA_RESUME          0xD4
#define DSP_SPEAKER_STATUS      0xD8
// SB16 ASP commands
#define DSP_ASP_SET_CODEC_PARAM 0x05
#define DSP_ASP_SET_REGISTER    0x0E
#define DSP_ASP_GET_REGISTER    0x0F

#define DSP_IDENT               0xE0
#define DSP_VERSION             0xE1
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

#define DSP_UNUSED_STATUS_BITS_PULLED_HIGH 0x7F

static char const copyright_string[] = "COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992.";

union sample32 {
    uint32_t data32;
    int16_t data16[2];
    uint8_t data8[4];
};

sbdsp_t sbdsp;

static uint8_t mixer_state[256] = { 0 };
static uint8_t sb_8051_ram[256] = { 0 };

static __force_inline void sbdsp_dma_disable(bool pause) {
    sbdsp.dma_enabled = false;
    memset(&sbdsp.rsm, 0, sizeof(sbdsp.rsm));
    sbdsp.cur_sample = 0;
    if (!pause) {
        sbdsp.dma_16bit = false;
        sbdsp.dma_signed = false;
        sbdsp.dma_stereo = false;
    }
}

uint32_t sbdsp_generate_sample() {
    // Consume ready DMA samples when the fractional accumulator says we need them
    while (sbdsp.rsm.samplecnt >= sbdsp.rateratio) {
        if (!sbdsp.rsm.dma_sample_ready) break;  // DMA not ready yet

        sbdsp.rsm.old_sample = sbdsp.rsm.new_sample;
        sbdsp.rsm.new_sample = sbdsp.rsm.dma_sample;
        sbdsp.rsm.dma_sample_ready = false;
        sbdsp.rsm.samplecnt -= sbdsp.rateratio;

        // Track transfer count
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
                sbdsp_dma_disable(false);
                break;
            }
        }

        // Pipeline: kick off next DMA transfer
        if (sbdsp.dma_enabled && !sbdsp.rsm.dma_pending) {
            sbdsp.rsm.dma_pending = true;
            DMA_Multi_Start_Write(&dma_config, sbdsp.dma_bytes_per_frame);
        }
    }

    // Linear interpolation between old_sample and new_sample
    int16_t old_l = (int16_t)(sbdsp.rsm.old_sample & 0xFFFF);
    int16_t old_r = (int16_t)(sbdsp.rsm.old_sample >> 16);
    int16_t new_l = (int16_t)(sbdsp.rsm.new_sample & 0xFFFF);
    int16_t new_r = (int16_t)(sbdsp.rsm.new_sample >> 16);

    int16_t out_l = (int16_t)((old_l * (sbdsp.rateratio - sbdsp.rsm.samplecnt)
                     + new_l * sbdsp.rsm.samplecnt) / sbdsp.rateratio);
    int16_t out_r = (int16_t)((old_r * (sbdsp.rateratio - sbdsp.rsm.samplecnt)
                     + new_r * sbdsp.rsm.samplecnt) / sbdsp.rateratio);

    sbdsp.rsm.samplecnt += 1 << SB_RSM_FRAC;

    return (uint32_t)(uint16_t)out_l | ((uint32_t)(uint16_t)out_r << 16);
}

static __force_inline void sbdsp_set_dma_interval() {
    if (sbdsp.sample_rate) {
        uint32_t actual_sr = (sbdsp.sample_rate >> (sbdsp.dma_stereo_sbpro ? 1 : 0));
        sbdsp.dma_interval = 1000000000ul / actual_sr / 1000;
        sbdsp.rateratio = (44100 << SB_RSM_FRAC) / actual_sr;
    } else {
        sbdsp.dma_interval = (256 - sbdsp.time_constant) << (sbdsp.dma_stereo_sbpro ? 1 : 0);
        sbdsp.rateratio = (44100 << SB_RSM_FRAC) / (1000000ul / sbdsp.dma_interval);
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
        memset(&sbdsp.rsm, 0, sizeof(sbdsp.rsm));
        sbdsp.rsm.dma_pending = true;
        DMA_Multi_Start_Write(&dma_config, sbdsp.dma_bytes_per_frame);
    }
}

static void sbdsp_dma_isr(void) {
    // dma_write_multi uses right-shift: bytes fill from MSB down.
    // Complete frame arrives as one push thanks to autopush threshold.
    const uint32_t dma_data = DMA_Complete_Write(&dma_config);
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
    sbdsp.rsm.dma_sample = sample;
    sbdsp.rsm.dma_sample_ready = true;
    sbdsp.rsm.dma_pending = false;
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

void sbdsp_init() {
    puts("Initing ISA DMA PIO...");
    SBDSP_DMA_isr_pt = sbdsp_dma_isr;

    sbdsp.dma = 0x2; // force DMA 1 for now
    sbdsp.interrupt = 0x2; // force IRQ 5 for now

    sbdsp.outbox = 0xAA;
    dma_config = DMA_multi_init(pio0, DMA_PIO_SM, SBDSP_DMA_isr_pt);

    // Initialize 8051 RAM with SB16 default values (per DOSBox-X)
    sb_8051_ram[0x0e] = 0xff;
    sb_8051_ram[0x0f] = 0x07;
    sb_8051_ram[0x37] = 0x38;
}


static __force_inline void sbdsp_output(uint8_t value) {
    sbdsp.outbox = value;
    sbdsp.dav_pc = 1;
}

// start pre-SB16 PCM DMA transfer
// TODO: Creative ADPCM?
static void sbdsp_start_pcm_dma(int autoinit, uint16_t xfer_size) {
    sbdsp.autoinit = autoinit;

    bool dma_stereo = (mixer_state[0x0E] & 2);
    if (dma_stereo && ((xfer_size + 1) == 1)) {
        // HACK: many SBPro programs follow Creative's documentation and perform 1 byte single-cycle
        // DMA transfer to seemingly reset the mixer stereo state, then waiting for IRQ to come.
        // since the sbdsp.dma_xfer_count calculation divides by 2 for stereo case, it results in
        // xfer_count==0, so the transfer never actually occurs.
        // for those transfers, force mono mode.
        dma_stereo = false;
    }
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
        case DSP_DMA_PAUSE:
        case DSP_PAUSE_DMA_16:
            sbdsp.current_command = 0;
            sbdsp_dma_disable(true);
            break;
        case DSP_DMA_RESUME:
            sbdsp.current_command = 0;
            sbdsp.dma_done = false;
            sbdsp_dma_enable();
            break;
        case DSP_CONTINUE_DMA_16:
        case DSP_CONTINUE_DMA_8:
            sbdsp.autoinit = 1;
            sbdsp.current_command = 0;
            break;
        case DSP_EXIT_DMA_16:
        case DSP_EXIT_DMA_8:
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
                    sbdsp.time_constant = sbdsp.inbox;
                    sbdsp.sample_rate = 0; // Rate of 0 indicates time constant drives DMA timing
                    sbdsp_set_dma_interval();
                    sbdsp.dav_dsp = 0;
                    sbdsp.current_command = 0;
                }
                sbdsp.current_command_index++;
            }
            break;
        case DSP_SET_SAMPLING_RATE:
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
                sbdsp_output(DSP_VERSION_MAJOR);
            } else {
                if (!sbdsp.dav_pc) {
                    sbdsp.current_command = 0;
                    sbdsp_output(DSP_VERSION_MINOR);
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
            sbdsp.speaker_on = true;
            sbdsp.current_command = 0;
            break;
        case DSP_DISABLE_SPEAKER:
            sbdsp.speaker_on = false;
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
                    PIC_AddEvent(&DSP_DAC_Resume_event, sbdsp.dma_interval * sbdsp.dac_pause_duration, 0);
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
                    // SB16: sample_count counts individual DMA transfers minus 1.
                    // Each DMA event transfers one complete frame, so for stereo
                    // we halve the count (2 DMA words per stereo frame).
                    sbdsp.dma_bytes_per_frame = 1;
                    if (sbdsp.dma_16bit) {
                        sbdsp.dma_bytes_per_frame <<= 1;
                    }
                    if (sbdsp.dma_stereo) {
                        sbdsp.dma_bytes_per_frame <<= 1;
                    }
                    sbdsp.dma_xfer_count = sbdsp.dma_stereo
                        ? (sbdsp.dma_sample_count + 1) >> 1
                        : sbdsp.dma_sample_count + 1;
                    sbdsp.dma_xfer_count_left = sbdsp.dma_xfer_count;
                    sbdsp.dav_dsp = 0;
                    sbdsp.current_command = 0;
                    sbdsp.dma_done = false;
                    sbdsp.speaker_on = true;
                    sbdsp_set_dma_interval();
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
        case 0:
            //not in a command
            break;
        default:
            printf("Unknown Command: %x\n", sbdsp.current_command);
            sbdsp.current_command = 0;
            break;
    }
    sbdsp.dsp_busy = 0;
}

static uint32_t DSP_Reset_EventHandler(Bitu val) {
    sbdsp.reset_state = 0;
    sbdsp.outbox = 0xAA;
    sbdsp.dav_pc = 1;
    sbdsp.current_command = 0;
    sbdsp.current_command_index = 0;

    sbdsp.dma_block_size = 0x7FF; //default per 2.01
    sbdsp.dma_xfer_count = 0;
    sbdsp.dma_xfer_count_left = 0;
    sbdsp.dma_stereo = false;
    sbdsp.dma_signed = false;
    sbdsp.speaker_on = false;
    sbdsp.dma_done = false;
    sbdsp.dac_resume_pending = false;
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
            sbdsp.autoinit = 0;
            sbdsp_dma_disable(false);
            sbdsp.reset_state = 1;
            break;
        case 0:
            if (sbdsp.reset_state == 1) {
                sbdsp.dav_pc = 0;
                PIC_RemoveEvent(&DSP_Reset_Event);
                PIC_AddEvent(&DSP_Reset_Event, 100, 0);
                sbdsp.reset_state = 2;
            }
            break;
        default:
            break;
    }
}

static __force_inline uint8_t sbmixer_read(void) {
    switch (sbdsp.mixer_command) {
        case MIXER_INTERRUPT:
            return sbdsp.interrupt;
        case MIXER_DMA:
            return sbdsp.dma;
        case MIXER_IRQ_STATUS:
            // Bits 0-2: IRQ pending flags. Upper bits: ViBRA type identifier.
            return (sbdsp.irq_8_pending ? 0x01 : 0) | (sbdsp.irq_16_pending ? 0x02 : 0) | 0x70;
        case 0x8E:
            return mixer_state[0x8E] | 0xC0;
        case 0xFF:
            return 0xFF;
        default:
            return mixer_state[sbdsp.mixer_command];
    }
}

static __force_inline void sbmixer_write(uint8_t value) {
    switch (sbdsp.mixer_command) {
        case 0x00: // Mixer reset
            memset(mixer_state, 0, sizeof(mixer_state));
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
    switch (address) {
        case DSP_READ:
            sbdsp.dav_pc = 0;
            return sbdsp.outbox;
        case DSP_READ_STATUS: //e
            if (sbdsp.irq_8_pending) {
                sbdsp.irq_8_pending = false;
                PIC_DeActivateIRQ();
            }
            return sbdsp.dav_pc << 7 | DSP_UNUSED_STATUS_BITS_PULLED_HIGH;
        case DSP_IRQ_16_STATUS:
            if (sbdsp.irq_16_pending) {
                sbdsp.irq_16_pending = false;
                PIC_DeActivateIRQ();
            }
            return 0xff;
        case DSP_WRITE_STATUS://c
            if (sbdsp.reset_state)
                return 0xFF; // busy during reset
            return (sbdsp.dav_dsp | sbdsp.dsp_busy | sbdsp.dac_resume_pending) << 7 | DSP_UNUSED_STATUS_BITS_PULLED_HIGH;
        case MIXER_DATA:
            return sbmixer_read();
        default:
            return 0xFF;
    }
}

void sbdsp_write(uint8_t address, uint8_t value) {
    switch (address) {
        case DSP_WRITE://c
            if (sbdsp.dav_dsp) printf("WARN - DAV_DSP OVERWRITE\n");
            sbdsp.inbox = value;
            sbdsp.dav_dsp = 1;
            break;
        case DSP_RESET:
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
