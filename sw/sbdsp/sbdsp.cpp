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

static irq_handler_t SBDSP_DMA_isr_pt;
static dma_inst_t dma_config;
#define DMA_PIO_SM 2

#define DSP_VERSION_MAJOR 2
#define DSP_VERSION_MINOR 1

// Sound Blaster DSP I/O port offsets
#define DSP_RESET           0x6
#define DSP_READ            0xA
#define DSP_WRITE           0xC
#define DSP_WRITE_STATUS    0xC
#define DSP_READ_STATUS     0xE

#define OUTPUT_SAMPLERATE   49716ul     

// Sound Blaster DSP commands.
#define DSP_DMA_HS_SINGLE       0x91
#define DSP_DMA_HS_AUTO         0x90
#define DSP_DMA_ADPCM           0x7F    //creative ADPCM 8bit to 3bit
#define DSP_DMA_SINGLE          0x14    //follosed by length
#define DSP_DMA_AUTO            0X1C    //length based on 48h
#define DSP_DMA_BLOCK_SIZE      0x48    //block size for highspeed/dma
//#define DSP_DMA_DAC 0x14
#define DSP_DIRECT_DAC          0x10
#define DSP_DIRECT_ADC          0x20
#define DSP_MIDI_READ_POLL      0x30
#define DSP_MIDI_WRITE_POLL     0x38
#define DSP_SET_TIME_CONSTANT   0x40
#define DSP_DMA_PAUSE           0xD0
#define DSP_EXIT_DMA_8          0xDA
#define DSP_DAC_PAUSE_DURATION  0x80    // Pause DAC for a duration, then generate an interrupt. Used by Tyrian.
#define DSP_ENABLE_SPEAKER      0xD1
#define DSP_DISABLE_SPEAKER     0xD3
#define DSP_DMA_RESUME          0xD4
#define DSP_SPEAKER_STATUS      0xD8
#define DSP_IDENT               0xE0
#define DSP_VERSION             0xE1
#define DSP_WRITETEST           0xE4
#define DSP_READTEST            0xE8
#define DSP_SINE                0xF0
#define DSP_IRQ                 0xF2
#define DSP_CHECKSUM            0xF4

// #define DSP_DMA_FIFO_SIZE       256
// constexpr uint16_t DSP_DMA_FIFO_BITS = DSP_DMA_FIFO_SIZE - 1;
#include "audio/audio_fifo.h"

#define DSP_UNUSED_STATUS_BITS_PULLED_HIGH 0x7F

sbdsp_t sbdsp;

#ifndef SB_BUFFERLESS

constexpr uint32_t AUDIO_FIFO_SIZE_HALF = AUDIO_FIFO_SIZE >> 1;

static int16_t __force_inline dma_interval_calc() {
    uint32_t level = fifo_level(&sbdsp.audio_fifo);
    if (level < AUDIO_FIFO_SIZE_HALF) {
        return sbdsp.dma_interval - 5;
    } else {
        return sbdsp.dma_interval + 5;
    }
}

void __force_inline sbdsp_fifo_rx(uint8_t byte) {
    if (!fifo_add_sample(&sbdsp.audio_fifo, (int16_t)(byte ^ 0x80) << 8)) {
        putchar('O');
    }
}
void __force_inline sbdsp_fifo_clear() {    
    fifo_reset(&sbdsp.audio_fifo);
}
audio_fifo_t* sbdsp_fifo_peek() {
    return &sbdsp.audio_fifo;
}

#endif

static uint32_t DSP_DMA_EventHandler(Bitu val);
static PIC_TimerEvent DSP_DMA_Event = {
    .handler = DSP_DMA_EventHandler,
};

static __force_inline void sbdsp_dma_disable() {
    sbdsp.dma_enabled=false;    
    PIC_RemoveEvent(&DSP_DMA_Event);  
#ifdef SB_BUFFERLESS
    sbdsp.cur_sample = 0;  // zero current sample
#endif
}

static __force_inline void sbdsp_dma_enable() {    
    if(!sbdsp.dma_enabled) {
        sbdsp.dma_enabled=true;
        PIC_AddEvent(&DSP_DMA_Event, sbdsp.dma_interval, 0);
    }
    // else {
    //     printf("INFO: DMA Already Enabled");        
    // }
}

static uint32_t DSP_DMA_EventHandler(Bitu val) {
    DMA_Start_Write(&dma_config);    
    uint32_t current_interval;
    sbdsp.dma_sample_count_rx++;    

#ifdef SB_BUFFERLESS
    current_interval = sbdsp.dma_interval;
#else
    current_interval = dma_interval_calc();
#endif
    // printf("%u\n", current_interval);

    if(sbdsp.dma_sample_count_rx <= sbdsp.dma_sample_count) {
        return current_interval;
    } else {                  
        PIC_ActivateIRQ();
        if(sbdsp.autoinit) {            
            sbdsp.dma_sample_count_rx=0;            
            return current_interval;
        }
        else {            
            sbdsp_dma_disable();            
        }
    } 
    return 0;
}

static void sbdsp_dma_isr(void) {
    const uint32_t dma_data = DMA_Complete_Write(&dma_config);    
#ifdef SB_BUFFERLESS
    sbdsp.cur_sample = scale_sample(((int16_t)(int8_t)((dma_data & 0xFF) ^ 0x80)) << 8, sb_volume, 0);
#else
    sbdsp_fifo_rx(dma_data & 0xFF);
#endif
}

static uint32_t DSP_DAC_Resume_eventHandler(Bitu val) {
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

    sbdsp.outbox = 0xAA;
    dma_config = DMA_init(pio0, DMA_PIO_SM, SBDSP_DMA_isr_pt);

#ifndef SB_BUFFERLESS
    fifo_init(&sbdsp.audio_fifo);
#endif
}


static __force_inline void sbdsp_output(uint8_t value) {
    sbdsp.outbox=value;
    sbdsp.dav_pc=1;    
}


void sbdsp_process(void) {    
    if(sbdsp.reset_state) return;     
    sbdsp.dsp_busy=1;

    if(sbdsp.dav_dsp) {
        if(!sbdsp.current_command) {            
            sbdsp.current_command = sbdsp.inbox;
            sbdsp.current_command_index=0;
            sbdsp.dav_dsp=0;
        }
    }

    switch(sbdsp.current_command) {  
        case DSP_DMA_PAUSE:
            sbdsp.current_command=0;                                    
            sbdsp_dma_disable();
            //printf("(0xD0)DMA PAUSE\n\r");            
            break;
        case DSP_DMA_RESUME:
            sbdsp.current_command=0;
            sbdsp_dma_enable();                        
            //printf("(0xD4)DMA RESUME\n\r");                                            
            break;
        case DSP_EXIT_DMA_8:
            sbdsp.autoinit = 0;
            sbdsp.current_command = 0;
            break;
        case DSP_DMA_AUTO:     
            // printf("(0x1C)DMA_AUTO\n\r");                   
            sbdsp.autoinit=1;           
            sbdsp.dma_sample_count = sbdsp.dma_block_size;
            sbdsp.dma_sample_count_rx=0;
            sbdsp_dma_enable();            
            sbdsp.current_command=0;                 
            break;        
        case DSP_DMA_HS_AUTO:
            // printf("(0x90) DMA_HS_AUTO\n\r");            
            sbdsp.dav_dsp=0;
            sbdsp.current_command=0;  
            sbdsp.autoinit=1;
            sbdsp.dma_sample_count = sbdsp.dma_block_size;
            sbdsp.dma_sample_count_rx=0;
            sbdsp_dma_enable();            
            break;            

        case DSP_SET_TIME_CONSTANT:
            if(sbdsp.dav_dsp) {
                if(sbdsp.current_command_index==1) {
                    //printf("(0x40) DSP_SET_TIME_CONSTANT\n\r");                                
                    sbdsp.time_constant = sbdsp.inbox;
                    sbdsp.dma_interval = 256 - sbdsp.time_constant;
                    sbdsp.sample_rate = 1000000ul / sbdsp.dma_interval;           
                    sbdsp.dma_interval_trim = MAX(1, sbdsp.dma_interval >> 1);
                    // printf("interval: %u rate: %u, trim: %u\n", sbdsp.dma_interval, sbdsp.sample_rate, sbdsp.dma_interval_trim);
                    
                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;                    
                }    
                sbdsp.current_command_index++;
            }
            break;
        case DSP_DMA_BLOCK_SIZE:            
            if(sbdsp.dav_dsp) {                             
                if(sbdsp.current_command_index==1) {                    
                    sbdsp.dma_block_size=sbdsp.inbox;
                    sbdsp.dav_dsp=0;                    
                }
                else if(sbdsp.current_command_index==2) {                    
                    sbdsp.dma_block_size += (sbdsp.inbox << 8);
                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;          
                    //printf("(0x48) Set BlockSize:%u\n\r",sbdsp.dma_block_size);                                        
                }
                sbdsp.current_command_index++;
            }
            break;          
        
        case DSP_DMA_HS_SINGLE:
            // printf("(0x91) DMA_HS_SINGLE\n\r");            
            sbdsp.dav_dsp=0;
            sbdsp.current_command=0;  
            sbdsp.autoinit=0;
            sbdsp.dma_sample_count = sbdsp.dma_block_size;
            sbdsp.dma_sample_count_rx=0;
            sbdsp_dma_enable();            
            break;            
        case DSP_DMA_SINGLE:              
            if(sbdsp.dav_dsp) {            
                if(sbdsp.current_command_index==1) {
                    sbdsp.dma_sample_count = sbdsp.inbox;
                    sbdsp.dav_dsp=0;                    
                }
                else if(sbdsp.current_command_index==2) {
                    // printf("(0x14)DMA_SINGLE\n\r");                      
                    sbdsp.dma_sample_count += (sbdsp.inbox << 8);
                    sbdsp.dma_sample_count_rx=0;                    
                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;  
                    sbdsp.autoinit=0;                                  
                    //printf("Sample Count:%u\n",sbdsp.dma_sample_count);                                        
                    sbdsp_dma_enable();                                                                                                                            
                }
                sbdsp.current_command_index++;
            }                        
            break;            
        case DSP_IRQ:
            sbdsp.current_command=0;             
            PIC_ActivateIRQ();                
            break;            
        case DSP_VERSION:
            if(sbdsp.current_command_index==0) {
                sbdsp.current_command_index=1;
                sbdsp_output(DSP_VERSION_MAJOR);
            }
            else {
                if(!sbdsp.dav_pc) {
                    sbdsp.current_command=0;                    
                    sbdsp_output(DSP_VERSION_MINOR);
                }
                
            }
            break;
        case DSP_IDENT:
            if(sbdsp.dav_dsp) {            
                if(sbdsp.current_command_index==1) {                    
                    //printf("(0xE0) DSP_IDENT\n\r");
                    sbdsp.dav_dsp=0;                    
                    sbdsp.current_command=0;        
                    sbdsp_output(~sbdsp.inbox);                                        
                }                
                sbdsp.current_command_index++;
            }                                       
            break;
        case DSP_ENABLE_SPEAKER:
            //printf("ENABLE SPEAKER\n");
            sbdsp.speaker_on = true;
            sbdsp.current_command=0;
            break;
        case DSP_DISABLE_SPEAKER:
            //printf("DISABLE SPEAKER\n");
            sbdsp.speaker_on = false;
            sbdsp.current_command=0;
            break;
        case DSP_SPEAKER_STATUS:
            if(sbdsp.current_command_index==0) {
                sbdsp.current_command=0;
                sbdsp_output(sbdsp.speaker_on ? 0xff : 0x00);
            }
            break;
        case DSP_DIRECT_DAC:
            if(sbdsp.dav_dsp) {
                if(sbdsp.current_command_index==1) {
#ifdef SB_BUFFERLESS
                    sbdsp.cur_sample = scale_sample(((int16_t)(int8_t)(sbdsp.inbox ^ 0x80)) << 8, sb_volume, 0);
#endif
                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;
                }
                sbdsp.current_command_index++;
            }
            break;
        //case DSP_DIRECT_ADC:
        //case DSP_MIDI_READ_POLL:
        //case DSP_MIDI_WRITE_POLL:
        
        //case DSP_HALT_DMA:       
        case DSP_WRITETEST:
            if(sbdsp.dav_dsp) {            
                if(sbdsp.current_command_index==1) {                    
                    //printf("(0xE4) DSP_WRITETEST\n\r");
                    sbdsp.test_register = sbdsp.inbox;
                    sbdsp.dav_dsp=0;                    
                    sbdsp.current_command=0;                                                
                }                
                sbdsp.current_command_index++;
            }                                       
            break;
        case DSP_READTEST:
            if(sbdsp.current_command_index==0) {
                sbdsp.current_command=0;
                sbdsp_output(sbdsp.test_register);
            }
            break;
        
        case DSP_DAC_PAUSE_DURATION:
            if(sbdsp.dav_dsp) {                             
                if(sbdsp.current_command_index==1) {                    
                    sbdsp.dac_pause_duration_low=sbdsp.inbox;
                    sbdsp.dav_dsp=0;                    
                }
                else if(sbdsp.current_command_index==2) {                    
                    sbdsp.dac_pause_duration = sbdsp.dac_pause_duration_low + (sbdsp.inbox << 8);
                    sbdsp.dac_resume_pending = true;
                    // When the specified duration elapses, the DSP generates an interrupt.
                    PIC_AddEvent(&DSP_DAC_Resume_event, sbdsp.dma_interval * sbdsp.dac_pause_duration, 0);
                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;          
                    //printf("(0x80) Pause Duration:%u\n\r",sbdsp.dac_pause_duration);                                        
                }
                sbdsp.current_command_index++;
            }
            break;       
        //case DSP_SINE:
        //case DSP_CHECKSUM:          
        case 0:
            //not in a command
            break;            
        default:
            printf("Unknown Command: %x\n",sbdsp.current_command);
            sbdsp.current_command=0;
            break;

    }                
    sbdsp.dsp_busy=0;
}

static uint32_t DSP_Reset_EventHandler(Bitu val) {
    sbdsp.reset_state=0;                
    sbdsp.outbox = 0xAA;
    sbdsp.dav_pc=1;
    sbdsp.current_command=0;
    sbdsp.current_command_index=0;

    sbdsp.dma_block_size=0x7FF; //default per 2.01
    sbdsp.dma_sample_count=0;
    sbdsp.dma_sample_count_rx=0;              
    sbdsp.speaker_on = false;
    sbdsp.dac_resume_pending = false;
    return 0;
}
static PIC_TimerEvent DSP_Reset_Event = {
    .handler = DSP_Reset_EventHandler,
};

static __force_inline void sbdsp_reset(uint8_t value) {
    //TODO: COLDBOOT ? WARMBOOT ?    
    value &= 1; // Some games may write unknown data for bits other than the LSB.
    switch(value) {
        case 1:                        
            PIC_RemoveEvent(&DSP_Reset_Event);  
            sbdsp.autoinit=0;
            sbdsp_dma_disable();
#ifndef SB_BUFFERLESS
            sbdsp_fifo_clear();
#endif
            sbdsp.reset_state=1;
            break;
        case 0:
            if(sbdsp.reset_state==1) {
                sbdsp.dav_pc=0;
                // sbdsp.outbox = 0xAA;
                PIC_RemoveEvent(&DSP_Reset_Event);  
                PIC_AddEvent(&DSP_Reset_Event, 100, 0);
                sbdsp.reset_state = 2;
            }
            break;
        default:
            break;
    }
}

uint8_t sbdsp_read(uint8_t address) {    
    uint8_t x;            
    switch(address) {        
        case DSP_READ:
            sbdsp.dav_pc=0;
            return sbdsp.outbox;
        case DSP_READ_STATUS: //e
            PIC_DeActivateIRQ();
            return sbdsp.dav_pc << 7 | DSP_UNUSED_STATUS_BITS_PULLED_HIGH;
        case DSP_WRITE_STATUS://c                        
            return (sbdsp.dav_dsp | sbdsp.dsp_busy | sbdsp.dac_resume_pending) << 7 | DSP_UNUSED_STATUS_BITS_PULLED_HIGH;
        default:
            //printf("SB READ: %x\n\r",address);
            return 0xFF;            
    }

}
void sbdsp_write(uint8_t address, uint8_t value) {    
    switch(address) {         
        case DSP_WRITE://c
            if(sbdsp.dav_dsp) printf("WARN - DAV_DSP OVERWRITE\n");
            sbdsp.inbox = value;
            sbdsp.dav_dsp = 1;            
            break;            
        case DSP_RESET:
            sbdsp_reset(value);
            break;        
        default:
            //printf("SB WRITE: %x => %x \n\r",value,address);            
            break;
    }
}
