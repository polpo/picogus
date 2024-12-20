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
#define DSP_IDENT               0xE0
#define DSP_VERSION             0xE1
#define DSP_COPYRIGHT           0xE3
#define DSP_WRITETEST           0xE4
#define DSP_READTEST            0xE8
#define DSP_SINE                0xF0
#define DSP_IRQ_8               0xF2
#define DSP_IRQ_16              0xF3
#define DSP_CHECKSUM            0xF4

#define MIXER_INTERRUPT         0x80
#define MIXER_DMA               0x81
#define MIXER_IRQ_STATUS        0x82
#define MIXER_STEREO            0xE

#define DSP_DMA_FIFO_SIZE       1024

#define DSP_UNUSED_STATUS_BITS_PULLED_HIGH 0x7F

static char const * const copyright_string="COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992.";


union sample {
    int16_t data16;
    uint8_t data8[2];
};

union sample32 {
    uint32_t data32;
    int16_t data16[2];
    uint8_t data8[4];
};

typedef struct sbdsp_t {
    uint8_t inbox;
    uint8_t outbox;
    uint8_t test_register;
    uint8_t current_command;
    uint8_t current_command_index;

    uint8_t mixer_command;

    uint16_t dma_interval;     
    // int16_t dma_interval_trim;
    uint8_t dma_transfer_size;
    uint8_t  dma_buffer[DSP_DMA_FIFO_SIZE];
    volatile uint16_t dma_buffer_tail;
    volatile uint16_t dma_buffer_head;

    uint16_t dac_pause_duration;
    uint8_t dac_pause_duration_low;

    uint16_t dma_block_size;
    uint32_t dma_bytes_per_frame;
    uint16_t dma_sample_count;      // Number of 8 or 16 bit samples minus 1
    uint32_t dma_xfer_count;        // Number of 8-bit DMA transfers minus 1
    // uint32_t dma_xfer_count_rx;     // Number of started DMA transfers
    // uint32_t dma_xfer_count_played; // Number of actually received DMA xfers
    uint32_t dma_xfer_count_left;     // Number of started DMA transfers

    uint8_t time_constant;
    uint16_t sample_rate;
    // uint32_t sample_step;
    // uint64_t cycle_us;

    // uint64_t sample_offset;  
    // uint8_t sample_factor;
                
    bool autoinit;    
    bool dma_enabled;
    bool dma_16bit;
    bool dma_stereo;
    bool dma_signed;
    volatile bool dma_done;

    bool speaker_on;
        
    volatile bool dav_pc;
    volatile bool dav_dsp;
    volatile bool dsp_busy;
    bool dac_resume_pending;
    volatile bool irq_8_pending;
    volatile bool irq_16_pending;

    uint8_t interrupt;
    uint8_t dma;

    uint8_t reset_state;  
   
    // volatile int16_t cur_sample_l;
    // volatile int16_t cur_sample_r;

    sample32 cur_sample;
    sample32 next_sample;

    // volatile sample next_sample_l;
    // volatile sample next_sample_r;
} sbdsp_t;

static sbdsp_t sbdsp;

#if 0
uint16_t sbdsp_fifo_level() {
    if(sbdsp.dma_buffer_tail < sbdsp.dma_buffer_head) return DSP_DMA_FIFO_SIZE - (sbdsp.dma_buffer_head - sbdsp.dma_buffer_tail);
    return sbdsp.dma_buffer_tail - sbdsp.dma_buffer_head;
}
void sbdsp_fifo_rx(uint8_t byte) {    
    if(sbdsp_fifo_level()+1 == DSP_DMA_FIFO_SIZE) printf("OVERRRUN");
    sbdsp.dma_buffer[sbdsp.dma_buffer_tail]=byte;        
    sbdsp.dma_buffer_tail++;
    if(sbdsp.dma_buffer_tail == DSP_DMA_FIFO_SIZE) sbdsp.dma_buffer_tail=0;
}
void sbdsp_fifo_clear() {    
    sbdsp.dma_buffer_head=sbdsp.dma_buffer_tail;
}
bool sbdsp_fifo_half() {
    if(sbdsp_fifo_level() >= (DSP_DMA_FIFO_SIZE/2)) return true;
    return false;
}

uint16_t sbdsp_fifo_tx(char *buffer,uint16_t len) {
    uint16_t level = sbdsp_fifo_level();
    if(!level) return 0;
    if(!len) return 0;
    if(len > level) len=level;
    if(sbdsp.dma_buffer_head < sbdsp.dma_buffer_tail || ((sbdsp.dma_buffer_head+len) < DSP_DMA_FIFO_SIZE)) {          
            memcpy(buffer,sbdsp.dma_buffer+sbdsp.dma_buffer_head,len);
            sbdsp.dma_buffer_head += len;
            return len;
    }           
    else {                
        memcpy(buffer,sbdsp.dma_buffer+sbdsp.dma_buffer_head,DSP_DMA_FIFO_SIZE-sbdsp.dma_buffer_head);
        memcpy(buffer+256-sbdsp.dma_buffer_head,sbdsp.dma_buffer,len-(DSP_DMA_FIFO_SIZE-sbdsp.dma_buffer_head));        
        sbdsp.dma_buffer_head += (len-DSP_DMA_FIFO_SIZE);
        return len;
    }
    return 0;    
}
#endif

static uint32_t DSP_DMA_EventHandler(Bitu val);
static PIC_TimerEvent DSP_DMA_Event = {
    .handler = DSP_DMA_EventHandler,
};

static __force_inline void sbdsp_dma_disable(bool pause) {
    sbdsp.dma_enabled=false;    
    PIC_RemoveEvent(&DSP_DMA_Event);  
    // sbdsp.cur_sample_l = 0;  // zero current sample
    // sbdsp.cur_sample_r = 0;  // zero current sample
    sbdsp.cur_sample.data32 = 0;
    if (!pause) {
        sbdsp.dma_16bit = false;
        sbdsp.dma_signed = false;
        sbdsp.dma_stereo = false;
    }
}

static __force_inline void sbdsp_dma_enable() {    
    if(!sbdsp.dma_enabled) {
        // sbdsp_fifo_clear();
        sbdsp.dma_enabled=true;
        // Set autopush bits to number of bits per audio frame. 32 will get masked to 0 by this operation which is correct behavior
        hw_write_masked(&pio0->sm[DMA_PIO_SM].shiftctrl,
                        (sbdsp.dma_bytes_per_frame << 3) << PIO_SM0_SHIFTCTRL_PUSH_THRESH_LSB,
                        PIO_SM0_SHIFTCTRL_PUSH_THRESH_BITS);
        PIC_AddEvent(&DSP_DMA_Event, sbdsp.dma_interval, 0);
    }
    else {
        //printf("INFO: DMA Already Enabled");        
    }
}


static uint32_t DSP_DMA_EventHandler(Bitu val) {
    // DMA_Multi_Start_Write(&dma_config, sbdsp.dma_bytes_per_frame);
    uint32_t current_interval;

    // uint32_t to_xfer = MIN(sbdsp.dma_bytes_per_frame, sbdsp.dma_xfer_count_left);
    // DMA_Multi_Start_Write(&dma_config, to_xfer);
    DMA_Start_Write(&dma_config);
    // sbdsp.dma_xfer_count_left -= to_xfer;
    sbdsp.dma_xfer_count_left--;
    // putchar(sbdsp.dma_xfer_count_left + 0x30);

    // current_interval = sbdsp.dma_interval;
    
    // sbdsp.dsp_busy = sbdsp.dma_xfer_count_left > 10000;
    // if ((sbdsp.dma_xfer_count_left & 0xfff) == 0)
    // printf("%u\n", sbdsp.dma_xfer_count_left);

    if(sbdsp.dma_xfer_count_left) {
        // TODO adjust interval based on to_xfer and actual sampling rate
        current_interval = sbdsp.dma_interval;
        // return sbdsp.dma_interval;
    } else {
        putchar('%');
        sbdsp.dma_done = true;
        if (sbdsp.dma_16bit) {
            sbdsp.irq_16_pending = true;
        } else {
            sbdsp.irq_8_pending = true;
        }
        PIC_ActivateIRQ();
        if(sbdsp.autoinit) {            
            sbdsp.dma_xfer_count_left = sbdsp.dma_xfer_count;            
            current_interval = sbdsp.dma_interval;
            // return sbdsp.dma_interval;
        }
        else {
            sbdsp_dma_disable(false);
            current_interval = 0;
        }
    } 
    // sbdsp.dma_xfer_count_rx++;
    /*
    if (MIN(sbdsp.dma_bytes_per_frame, (sbdsp.dma_xfer_count + 1 - sbdsp.dma_xfer_count_rx)) != sbdsp.dma_bytes_per_frame) {
        putchar((sbdsp.dma_xfer_count + 1 - sbdsp.dma_xfer_count_rx) + 0x30);
    }
    DMA_Multi_Start_Write(&dma_config, MIN(sbdsp.dma_bytes_per_frame, (sbdsp.dma_xfer_count + 1 - sbdsp.dma_xfer_count_rx)));
    if (!sbdsp.dma_done) {
        sbdsp.dma_xfer_count_rx += sbdsp.dma_bytes_per_frame;
        sbdsp.dma_done = false;
    }
    // DMA_Start_Write(&dma_config);    
    // return 0;
    */
    return current_interval;
}

static void sbdsp_dma_isr(void) {
    while (!pio_sm_is_rx_fifo_empty(dma_config.pio, dma_config.sm)) {
    const uint32_t dma_data = DMA_Complete_Write(&dma_config);
    // printf("dma %x ", dma_data);
    if (sbdsp.dma_stereo) {
        if (sbdsp.dma_16bit) {
            // 16 bit stereo
            /*
            sbdsp.next_sample.data8[sbdsp.dma_xfer_count_played & 3] = dma_data;
            if ((sbdsp.dma_xfer_count_played & 3) == 3) {
                sbdsp.cur_sample.data32 = sbdsp.dma_signed ? sbdsp.next_sample.data32 : sbdsp.next_sample.data32 ^ 0x80008000;
            }
            */
            sbdsp.cur_sample.data32 = sbdsp.dma_signed ? dma_data : dma_data ^ 0x80008000;
        } else {
            // 8 bit stereo
            /*
            sbdsp.next_sample.data16[sbdsp.dma_xfer_count_played & 1] = dma_data << 8;
            if ((sbdsp.dma_xfer_count_played & 1) == 1) {
                sbdsp.cur_sample.data32 = sbdsp.dma_signed ? sbdsp.next_sample.data32 : sbdsp.next_sample.data32 ^ 0x80008000;
            }
            */
            // sbdsp.next_sample.data16[0] = dma_data << 8;
            // sbdsp.next_sample.data16[1] = dma_data & 0xff00;
            sbdsp.next_sample.data16[0] = dma_data >> 16;
            sbdsp.next_sample.data16[1] = dma_data >> 8;
            sbdsp.cur_sample.data32 = sbdsp.dma_signed ? sbdsp.next_sample.data32 : sbdsp.next_sample.data32 ^ 0x80008000;
        }
    } else {
        // TODO 16 bit mono
        // 8 bit mono
        // sbdsp.next_sample.data16[0] = sbdsp.next_sample.data16[1] = dma_data << 8;
        sbdsp.next_sample.data16[0] = sbdsp.next_sample.data16[1] = dma_data >> 16;
        sbdsp.cur_sample.data32 = sbdsp.dma_signed ? sbdsp.next_sample.data32 : sbdsp.next_sample.data32 ^ 0x80008000;
    }
    }
    // if (sbdsp.dma_done) {
    //     if (sbdsp.dma_16bit) {
    //         sbdsp.irq_16_pending = true;
    //     } else {
    //         sbdsp.irq_8_pending = true;
    //     }
    //     PIC_ActivateIRQ();
    //     sbdsp.dma_done = false;
    // }
    // sbdsp.dma_xfer_count_played++;
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

void sbdsp_samples(int16_t *buf) {
    if (sbdsp.speaker_on & ~sbdsp.dac_resume_pending) {
        // buf[0] = sbdsp.cur_sample_l;
        // buf[1] = sbdsp.cur_sample_r;
        // *buf = sbdsp.cur_sample.data16;
        memcpy(buf, &sbdsp.cur_sample, 4);
    } else {
        buf[0] = buf[1] = 0;
    }
}

void sbdsp_init() {    
    // uint8_t x,y,z;    
    // char buffer[32];

    puts("Initing ISA DMA PIO...");    
    SBDSP_DMA_isr_pt = sbdsp_dma_isr;

    sbdsp.dma = 0x2; // force DMA 1 for now
    sbdsp.interrupt = 0x2; // force IRQ 5 for now

    sbdsp.outbox = 0xAA;
    // dma_config = DMA_multi_init(pio0, DMA_PIO_SM, SBDSP_DMA_isr_pt);         
    dma_config = DMA_init(pio0, DMA_PIO_SM, SBDSP_DMA_isr_pt);         
}


static __force_inline void sbdsp_output(uint8_t value) {
    sbdsp.outbox=value;
    sbdsp.dav_pc=1;    
}


static __force_inline void sbdsp_set_dma_interval() {
    if (sbdsp.sample_rate) {
        sbdsp.dma_interval = 1000000ul / sbdsp.sample_rate;
    } else {
        sbdsp.dma_interval = 256 - sbdsp.time_constant;
    }
    if (sbdsp.dma_stereo) {
        // printf("stereoeoeoeo");
        sbdsp.dma_interval >>= 1;
    }
    if (sbdsp.dma_16bit) {
        sbdsp.dma_interval >>= 1;
    }
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
    // if (sbdsp.current_command) {
    //     putchar(sbdsp.inbox);
    // }
    switch(sbdsp.current_command) {  
        case DSP_DMA_PAUSE:
        case DSP_PAUSE_DMA_16:
            sbdsp.current_command=0;                                    
            sbdsp_dma_disable(true);
            //printf("(0xD0)DMA PAUSE\n\r");            
            break;
        case DSP_DMA_RESUME:
            sbdsp.current_command=0;
            sbdsp.dma_done = false;
            sbdsp_dma_enable();                        
            //printf("(0xD4)DMA RESUME\n\r");                                            
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
            // printf("(0x1C)DMA_AUTO\n\r");                   
            sbdsp.autoinit=1;           
            sbdsp.dma_xfer_count = sbdsp.dma_block_size + 1;
            sbdsp.dma_xfer_count_left = sbdsp.dma_xfer_count;
            // sbdsp.dma_xfer_count_rx=0;
            // sbdsp.dma_xfer_count_played=0;
            sbdsp.dma_bytes_per_frame = sbdsp.dma_stereo ? 2 : 1;
            sbdsp_set_dma_interval();
            sbdsp.dma_done = false;
            sbdsp_dma_enable();            
            sbdsp.current_command=0;                 
            break;        
        case DSP_DMA_HS_AUTO:
            // printf("(0x90) DMA_HS_AUTO\n\r");            
            sbdsp.dav_dsp=0;
            sbdsp.autoinit=1;
            sbdsp.dma_xfer_count = sbdsp.dma_block_size + 1;
            sbdsp.dma_xfer_count_left = sbdsp.dma_xfer_count;
            // sbdsp.dma_xfer_count_rx=0;
            // sbdsp.dma_xfer_count_played=0;
            sbdsp.dma_bytes_per_frame = sbdsp.dma_stereo ? 2 : 1;
            sbdsp_set_dma_interval();
            sbdsp.dma_done = false;
            sbdsp_dma_enable();            
            sbdsp.current_command=0;  
            break;            

        case DSP_SET_TIME_CONSTANT:
            if(sbdsp.dav_dsp) {
                if(sbdsp.current_command_index==1) {
                    //printf("(0x40) DSP_SET_TIME_CONSTANT\n\r");                                
                    sbdsp.time_constant = sbdsp.inbox;
                    /*
                    sbdsp.sample_rate = 1000000ul / (256 - sbdsp.time_constant);           
                    sbdsp.dma_interval = 1000000ul / sbdsp.sample_rate; // redundant.                    
                    */
                    // sbdsp.dma_interval = 256 - sbdsp.time_constant;
                    sbdsp.sample_rate = 0; // Rate of 0 indicates time constant drives DMA timing           
                    // sbdsp.sample_step = sbdsp.sample_rate * 65535ul / OUTPUT_SAMPLERATE;                    
                    // sbdsp.sample_factor = (OUTPUT_SAMPLERATE / sbdsp.sample_rate)+5; //Estimate
                    
                    // sbdsp.dma_transfer_size = 4;
                    
                    //sbdsp.i2s_buffer_size = ((OUTPUT_SAMPLERATE * 65535ul) / sbdsp.sample_rate * sbdsp.dma_buffer_size) >> 16;
                    
                    
                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;                    
                }    
                sbdsp.current_command_index++;
            }
            break;
        case DSP_SET_SAMPLING_RATE:
            if(sbdsp.dav_dsp) {
                if(sbdsp.current_command_index==1) { // wSamplingRate.HighByte
                    // Need to fix this (some kind of trim?)
                    sbdsp.sample_rate = sbdsp.inbox << 8;
                    sbdsp.dav_dsp=0;
                } else if (sbdsp.current_command_index==2) { // wSamplingRate.LowByte
                    sbdsp.sample_rate |= sbdsp.inbox;
                    sbdsp.time_constant = 0;
                    // Default interval for 8-bit mono, will adjust when DMA starts
                    // sbdsp.dma_interval = 1000000ul / sbdsp.sample_rate;
                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;
                    // printf("(0x40) DSP_SET_SAMPLING_RATE: %d %d\n\r", sbdsp.sample_rate, sbdsp.dma_interval);
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
            sbdsp.dma_xfer_count = sbdsp.dma_block_size + 1;
            sbdsp.dma_xfer_count_left = sbdsp.dma_xfer_count;
            // sbdsp.dma_xfer_count_rx=0;
            // sbdsp.dma_xfer_count_played=0;
            sbdsp.dma_bytes_per_frame = sbdsp.dma_stereo ? 2 : 1;
            sbdsp_set_dma_interval();
            sbdsp.dma_done = false;
            sbdsp_dma_enable();            
            break;            
        case DSP_DMA_SINGLE:              
            if(sbdsp.dav_dsp) {            
                if(sbdsp.current_command_index==1) {
                    sbdsp.dma_xfer_count = sbdsp.inbox;
                    sbdsp.dav_dsp=0;                    
                }
                else if(sbdsp.current_command_index==2) {
                    // printf("(0x14)DMA_SINGLE\n\r");                      
                    sbdsp.dma_xfer_count += (sbdsp.inbox << 8);
                    // sbdsp.dma_xfer_count_rx=0;                    
                    // sbdsp.dma_xfer_count_played=0;
                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;  
                    sbdsp.autoinit=0;                                  
                    //printf("Sample Count:%u\n",sbdsp.dma_xfer_count);                                        
                    sbdsp.dma_bytes_per_frame = sbdsp.dma_stereo ? 2 : 1;
                    sbdsp_set_dma_interval();
                    sbdsp.dma_done = false;
                    sbdsp_dma_enable();                                                                                                                            
                }
                sbdsp.current_command_index++;
            }                        
            break;            
        case DSP_IRQ_8:
            sbdsp.current_command=0;             
            sbdsp.irq_8_pending = true;
            PIC_ActivateIRQ();                
            break;            
        case DSP_IRQ_16:
            sbdsp.current_command=0;             
            sbdsp.irq_16_pending = true;
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
            // printf("ENABLE SPEAKER\n");
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
                    // sbdsp.cur_sample_l = sbdsp.cur_sample_r = (int16_t)(sbdsp.inbox) - 0x80 << 5;
                    sbdsp.cur_sample.data16[0] = sbdsp.cur_sample.data16[1] = (sbdsp.inbox << 8) ^ 0x8000;
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
        case DSP_DMA_IO_START ... DSP_DMA_IO_END: 
            if (sbdsp.dav_dsp) {
                if (sbdsp.current_command_index==0) { // bCommand + bMode
                    // printf("%x\n", sbdsp.current_command);
                    // printf("%x\n", sbdsp.inbox);
                    sbdsp.dma_16bit = (sbdsp.current_command & 0xf0) == 0xb0;
                    sbdsp.autoinit = sbdsp.current_command & 0x4;
                    sbdsp.dma_signed = sbdsp.inbox & 0x10;
                    sbdsp.dma_stereo = sbdsp.inbox & 0x20;
                    sbdsp.dav_dsp = 0;                    
                } else if (sbdsp.current_command_index==1) { // wLength.LowByte
                    // printf("%x\n", sbdsp.inbox);
                    sbdsp.dma_sample_count = sbdsp.inbox;
                    sbdsp.dav_dsp = 0;                    
                } else if (sbdsp.current_command_index==2) { // wLength.HighByte
                    // printf("%x\n", sbdsp.inbox);
                    // printf("(0xbx/0xcx)DMA_IO\n\r");
                    sbdsp.dma_sample_count |= (sbdsp.inbox << 8);
                    sbdsp.dma_xfer_count = sbdsp.dma_sample_count + 1;
                    if (sbdsp.dma_16bit) {
                        // sbdsp.dma_sample_count += 1;
                        // sbdsp.dma_sample_count <<= 1;
                        sbdsp.dma_xfer_count <<= 1;
                        // sbdsp.dma_interval >>= 1;
                    }
                    if (sbdsp.dma_stereo) {
                        // sbdsp.dma_interval >>= 1;
                    }
                    // sbdsp.dma_interval = 10;
                    // sbdsp.dma_xfer_count_rx = 0;
                    // sbdsp.dma_xfer_count_played = 0;
                    sbdsp.dma_xfer_count_left = sbdsp.dma_xfer_count;
                    sbdsp.dav_dsp = 0;
                    sbdsp.current_command = 0;  
                    sbdsp.dma_done = false;
                    sbdsp.speaker_on = true;
                    sbdsp.dma_bytes_per_frame = 1;
                    if (sbdsp.dma_16bit) {
                        sbdsp.dma_bytes_per_frame <<= 1;
                    }
                    if (sbdsp.dma_stereo) {
                        sbdsp.dma_bytes_per_frame <<= 1;
                    }
                    sbdsp_set_dma_interval();
                    // printf("starting DMA: autoinit %d stereo %d 16b %d signed %d sample count %d dma count %d interval %d bytes per frame %d", sbdsp.autoinit, sbdsp.dma_stereo, sbdsp.dma_16bit, sbdsp.dma_signed, sbdsp.dma_sample_count, sbdsp.dma_xfer_count, sbdsp.dma_interval, sbdsp.dma_bytes_per_frame);
                    sbdsp_dma_enable();
                }
                sbdsp.current_command_index++;
            }
            break;            
        case 0:
        // case 0xff:
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
    sbdsp.dma_xfer_count=0;
    sbdsp.dma_xfer_count_left=0;
    // sbdsp.dma_xfer_count_rx=0;              
    // sbdsp.dma_xfer_count_played=0;
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
    //TODO: COLDBOOT ? WARMBOOT ?    
    value &= 1; // Some games may write unknown data for bits other than the LSB.
    switch(value) {
        case 1:                        
            PIC_RemoveEvent(&DSP_Reset_Event);  
            sbdsp.autoinit=0;
            sbdsp_dma_disable(false);
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

static uint8_t mixer_state[256] = { 0 };

static __force_inline uint8_t sbmixer_read(void) {
    switch (sbdsp.mixer_command) {
        case MIXER_INTERRUPT:
            return sbdsp.interrupt;
            break;
        case MIXER_DMA:
            return sbdsp.dma;
            break;
        case MIXER_IRQ_STATUS:
            // printf("IRQ status\n");
            return (sbdsp.irq_16_pending << 1) || sbdsp.irq_8_pending;
        default:
            // printf("Unimplemented mixer read: %x\n", sbdsp.mixer_command);
            // return mixer_state[sbdsp.mixer_command];
            return 0xff;
    }
}

static __force_inline void sbmixer_write(uint8_t value) {
    switch (sbdsp.mixer_command) {
        case MIXER_INTERRUPT:
            // sbdsp.interrupt = value;
            break;
        case MIXER_DMA:
            // sbdsp.dma = value;
            break;
        case MIXER_STEREO:
            // printf("stereo %x\n", value);
            sbdsp.dma_stereo = value & 2;
            // no break
        default:
            mixer_state[sbdsp.mixer_command] = value;
            // printf("Unimplemented mixer write: %x %x\n", sbdsp.mixer_command, value);
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
            if (sbdsp.irq_8_pending) {
                sbdsp.irq_8_pending = false;
                PIC_DeActivateIRQ();
            }
            return sbdsp.dav_pc << 7 | DSP_UNUSED_STATUS_BITS_PULLED_HIGH;
        case DSP_IRQ_16_STATUS:
            // printf(" 16 IRQ ack");
            if (sbdsp.irq_16_pending) {
                // putchar('y');
                sbdsp.irq_16_pending = false;
                PIC_DeActivateIRQ();
            }
            return 0xff;
        case DSP_WRITE_STATUS://c                        
            return (sbdsp.dav_dsp | sbdsp.dsp_busy | sbdsp.dac_resume_pending) << 7 | DSP_UNUSED_STATUS_BITS_PULLED_HIGH;
        case MIXER_DATA:
            return sbmixer_read();
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

            putchar(sbdsp.inbox);
            break;            
        case DSP_RESET:
            sbdsp_reset(value);
            break;
        case MIXER_COMMAND:
            sbdsp.mixer_command = value;
        case MIXER_DATA:
            sbmixer_write(value);
        default:
            //printf("SB WRITE: %x => %x \n\r",value,address);            
            break;
    }
}
