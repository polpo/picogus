#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "pico_pic.h"

/*
Title  : SoundBlaster DSP Emulation 
Date   : 2023-12-30
Author : Kevin Moonlight <me@yyzkevin.com>

*/

#include "isa_dma.h"


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
#define DSP_DMA_PAUSE_DURATION  0x80    //Used by Tryrian
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

#define DSP_DMA_FIFO_SIZE       1024

typedef struct sbdsp_t {
    uint8_t inbox;
    uint8_t outbox;
    uint8_t test_register;
    uint8_t current_command;
    uint8_t current_command_index;

    uint16_t dma_interval;     
    // int16_t dma_interval_trim;
    uint8_t dma_transfer_size;
    uint8_t  dma_buffer[DSP_DMA_FIFO_SIZE];
    volatile uint16_t dma_buffer_tail;
    volatile uint16_t dma_buffer_head;

    uint16_t dma_pause_duration;
    uint8_t dma_pause_duration_low;

    uint16_t dma_block_size;
    uint32_t dma_sample_count;
    uint32_t dma_sample_count_rx;

    uint8_t time_constant;
    // uint16_t sample_rate;
    // uint32_t sample_step;
    // uint64_t cycle_us;

    // uint64_t sample_offset;  
    // uint8_t sample_factor;
                
    bool autoinit;    
    bool dma_enabled;

    bool speaker_on;
        
    volatile bool dav_pc;
    volatile bool dav_dsp;
    volatile bool dsp_busy;

    uint8_t reset_state;  
   
    volatile int16_t cur_sample;
} sbdsp_t;

static sbdsp_t sbdsp;

static uint32_t DSP_DMA_Event(Bitu val);

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

static __force_inline void sbdsp_dma_disable() {
    sbdsp.dma_enabled=false;    
    PIC_RemoveEvents(DSP_DMA_Event);  
    sbdsp.cur_sample = 0;  // zero current sample
}

static __force_inline void sbdsp_dma_enable() {    
    if(!sbdsp.dma_enabled) {
        // sbdsp_fifo_clear();
        sbdsp.dma_enabled=true;            
        if(sbdsp.dma_pause_duration) {            
            PIC_AddEvent(DSP_DMA_Event,sbdsp.dma_interval * sbdsp.dma_pause_duration,1);
            sbdsp.dma_pause_duration=0;
        }
        else {
            PIC_AddEvent(DSP_DMA_Event,sbdsp.dma_interval,1);
        }
    }
    else {
        //printf("INFO: DMA Already Enabled");        
    }
}

static uint32_t DSP_DMA_Event(Bitu val) {
    DMA_Start_Write(&dma_config);    
    uint32_t current_interval;
    sbdsp.dma_sample_count_rx++;    

    if(sbdsp.dma_pause_duration) {
        current_interval = sbdsp.dma_interval * sbdsp.dma_pause_duration;        
        sbdsp.dma_pause_duration=0;
    }
    else {
        current_interval = sbdsp.dma_interval;
    }

    if(sbdsp.dma_sample_count_rx <= sbdsp.dma_sample_count) {        
        return current_interval;
    }
    else {                  
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
    sbdsp.cur_sample = (int16_t)(dma_data & 0xFF) - 0x80 << 5;
}

int16_t sbdsp_sample() {
    return sbdsp.speaker_on ? sbdsp.cur_sample : 0;
}

void sbdsp_init() {    
    // uint8_t x,y,z;    
    // char buffer[32];
       

    puts("Initing ISA DMA PIO...");    
    SBDSP_DMA_isr_pt = sbdsp_dma_isr;
    dma_config = DMA_init(pio0, DMA_PIO_SM, SBDSP_DMA_isr_pt);         
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
                    /*
                    sbdsp.sample_rate = 1000000ul / (256 - sbdsp.time_constant);           
                    sbdsp.dma_interval = 1000000ul / sbdsp.sample_rate; // redundant.                    
                    */
                    sbdsp.dma_interval = 256 - sbdsp.time_constant;
                    // sbdsp.sample_rate = 1000000ul / sbdsp.dma_interval;           
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
                    sbdsp.cur_sample=(int16_t)(sbdsp.inbox) - 0x80 << 5;
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
        
        case DSP_DMA_PAUSE_DURATION:
            if(sbdsp.dav_dsp) {                             
                if(sbdsp.current_command_index==1) {                    
                    sbdsp.dma_pause_duration_low=sbdsp.inbox;
                    sbdsp.dav_dsp=0;                    
                }
                else if(sbdsp.current_command_index==2) {                    
                    sbdsp.dma_pause_duration = sbdsp.dma_pause_duration_low + (sbdsp.inbox << 8);
                    sbdsp.dav_dsp=0;
                    sbdsp.current_command=0;          
                    //printf("(0x80) Pause Duration:%u\n\r",sbdsp.dma_pause_duration);                                        
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
static __force_inline void sbdsp_reset(uint8_t value) {
    //TODO: COLDBOOT ? WARMBOOT ?    
    value &= 1; // Some games may write unknown data for bits other than the LSB.
    switch(value) {
        case 1:                        
            sbdsp.autoinit=0;
            sbdsp_dma_disable();
            sbdsp.reset_state=1;
            break;
        case 0:
            if(sbdsp.reset_state==0) return; 
            if(sbdsp.reset_state==1) {                
                sbdsp.reset_state=0;                
                sbdsp.outbox = 0xAA;
                sbdsp.dav_pc=1;
                sbdsp.current_command=0;
                sbdsp.current_command_index=0;

                sbdsp.dma_block_size=0x7FF; //default per 2.01
                sbdsp.dma_sample_count=0;
                sbdsp.dma_sample_count_rx=0;              
                sbdsp.speaker_on = false;
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
            //printf("i");
            return (sbdsp.dav_pc << 7);            
        case DSP_WRITE_STATUS://c                        
            return (sbdsp.dav_dsp | sbdsp.dsp_busy) << 7;                                
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
