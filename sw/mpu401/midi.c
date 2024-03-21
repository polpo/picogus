/*
 *  Copyright (C) 2002-2013  The DOSBox Team
 *  Copyright (C) 2013-2014  bjt, elianda
 *  Copyright (C) 2015       ab0tj
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
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 * --------------------------------------------
 * HardMPU by ab0tj - Hardware MPU-401 Emulator
 * --------------------------------------------
 * 
 * Based on midi.c from SoftMPU by bjt which was
 * based on original midi.c from DOSBox
 *
 */

/* SOFTMPU: Moved exported functions & types to header */
#include "export.h"

/* HardMPU includes */
/*
#include "config.h"
#include <util/delay.h>
*/
#include "hardware/timer.h"
#include "pico/critical_section.h"
static critical_section_t midi_crit;

/* SOFTMPU: Additional defines, typedefs etc. for C */
typedef uint32_t Bit32u;
typedef int32_t Bits;

#define SYSEX_SIZE 8192     // sysex buffer for delay calculation

/* RAWBUF: This is the buffer for outgoing MIDI data. The larger the buffer,
    the less likely it is to overrun when SysEx delay is enabled and large SysEx
    transfers are occurring. */
//#define RAWBUF  14336
#define RAWBUF  65536
#define RAWBUF_BITS  65535

typedef struct ring_buffer {
    uint8_t buffer[RAWBUF];
    uint32_t head;
    uint32_t tail;
} ring_buffer;

static ring_buffer midi_out_buff = { {0}, 0, 0 };

/* SOFTMPU: Note tracking for RA-50 */
#define MAX_TRACKED_CHANNELS 16
#define MAX_TRACKED_NOTES 8

static const char* MIDI_welcome_msg = "\xf0\x41\x10\x16\x12\x20\x00\x00  *** PicoGUS ***   \x0a\xf7";   // message to show on MT-32 display

static Bit8u MIDI_note_off[3] = { 0x80,0x00,0x00 }; /* SOFTMPU */

static const Bit8u MIDI_evt_len[256] = {
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x00
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x10
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x20
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x30
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x40
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x50
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x60
  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  // 0x70

  3,3,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3,  // 0x80
  3,3,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3,  // 0x90
  3,3,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3,  // 0xa0
  3,3,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3,  // 0xb0

  2,2,2,2, 2,2,2,2, 2,2,2,2, 2,2,2,2,  // 0xc0
  2,2,2,2, 2,2,2,2, 2,2,2,2, 2,2,2,2,  // 0xd0

  3,3,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3,  // 0xe0

  0,2,3,2, 0,0,1,0, 1,0,1,1, 1,0,1,0   // 0xf0
};

/* SOFTMPU: Note tracking for RA-50 */
typedef struct {
    Bit8u used;
    Bit8u next;
    Bit8u notes[MAX_TRACKED_NOTES];
} channel;

channel tracked_channels[MAX_TRACKED_CHANNELS];

static struct {
    Bit8u status;
    Bit8u cmd_len;
    Bit8u cmd_pos;
    Bit8u cmd_buf[8];
    Bit8u rt_buf[8];
    struct {
        Bit8u buf[SYSEX_SIZE];
        Bitu used;
        /* Bit8u usedbufs; */
        Bitu delay;
        bool extra_delay;
        Bit32u start;
        // bool delay;
        Bit8u status;
    } sysex;
    bool fakeallnotesoff;
    bool available;
    /*MidiHandler * handler;*/ /* SOFTMPU */
} midi;

/* SOFTMPU: Sysex delay is decremented from PIC_Update */
//volatile Bitu MIDI_sysex_delaytime;

/* HardMPU: Output a byte to the physical UART */
void __force_inline static output_to_uart(Bit8u val)
{
    uart_write_blocking(uart0, &val, 1);
}

/* HardMPU: Wait for UART TX buffer to be empty */
void __force_inline static wait_for_uart()
{
    uart_tx_wait_blocking(uart0);
}

/* HardMPU: Check UART TX status, returns 0 for ready */
Bit8u __force_inline static uart_tx_status()
{
    return uart_is_writable(uart0) ? 0 : 1;
}

__force_inline static void PlayMsg(Bit8u* msg, Bitu len)
{
    // despite the name of this function, we're just going to buffer this message to send later.
    for (Bitu i = 0; i < len; i++) {
        /* putchar('m'); */
        critical_section_enter_blocking(&midi_crit);
        uint32_t next = (midi_out_buff.head + 1) & RAWBUF_BITS;
        if (next != midi_out_buff.tail) {
            midi_out_buff.buffer[midi_out_buff.head] = msg[i];
            midi_out_buff.head = next;
        }
        critical_section_exit(&midi_crit);
    }
}

__force_inline static void send_midi_byte_now(Bit8u byte) {
    /* wait_for_uart(); */
    output_to_uart(byte);
}

/* SOFTMPU: Fake "All Notes Off" for Roland RA-50 */
__force_inline static void FakeAllNotesOff(Bit8u chan)
{
    Bit8u note;
    channel* pChan;

    MIDI_note_off[0] &= 0xf0;
    MIDI_note_off[0] |= chan;

    pChan=&tracked_channels[chan];

    for (note=0;note<pChan->used;note++)
    {
        MIDI_note_off[1]=pChan->notes[note];
        PlayMsg(MIDI_note_off,3);
    }

    pChan->used=0;
    pChan->next=0;
}

void MIDI_RawOutByte(Bit8u data) {
    channel* pChan; /* SOFTMPU */

    /* Test for a realtime MIDI message */
    if (data>=0xf8) {
        midi.rt_buf[0]=data;
        PlayMsg(midi.rt_buf,1);
        return;
    }        
    if (data&0x80) {
        midi.status=data;
        midi.cmd_pos=0;
        midi.cmd_len=MIDI_evt_len[data];
    }
    if (midi.cmd_len) {
        midi.cmd_buf[midi.cmd_pos++]=data;
        if (midi.cmd_pos >= midi.cmd_len) {
            /*if (CaptureState & CAPTURE_MIDI) {
              CAPTURE_AddMidi(false, midi.cmd_len, midi.cmd_buf);
              }*/ /* SOFTMPU */

            if (midi.fakeallnotesoff)
            {
                /* SOFTMPU: Test for "Note On" */
                if ((midi.status&0xf0)==0x90)
                {
                    if (midi.cmd_buf[2]>0)
                    {
                        pChan=&tracked_channels[midi.status&0x0f];
                        pChan->notes[pChan->next++]=midi.cmd_buf[1];
                        if (pChan->next==MAX_TRACKED_NOTES) pChan->next=0;
                        if (pChan->used<MAX_TRACKED_NOTES) pChan->used++;
                    }

                    PlayMsg(midi.cmd_buf,midi.cmd_len);
                }
                /* SOFTMPU: Test for "All Notes Off" */
                else if (((midi.status&0xf0)==0xb0) &&
                        (midi.cmd_buf[1]>=0x7b) &&
                        (midi.cmd_buf[1]<=0x7f))
                {
                    FakeAllNotesOff(midi.status&0x0f);
                }
                else
                {
                    PlayMsg(midi.cmd_buf,midi.cmd_len);
                }
            }
            else
            {
                PlayMsg(midi.cmd_buf,midi.cmd_len);
            }
            midi.cmd_pos=1;         //Use Running status
        }
    }
    else 
    {
        midi.rt_buf[0] = data;
        PlayMsg(midi.rt_buf,1);
    }
}

void send_midi_byte() { 
    if (uart_tx_status()) return;   // can't send yet
    /* putchar('s'); */
    critical_section_enter_blocking(&midi_crit);
    if (midi_out_buff.head == midi_out_buff.tail) {
        critical_section_exit(&midi_crit);
        return;   // nothing to send
    }
    if (midi.sysex.start) {
        Bit32u passed_ticks = time_us_32() - midi.sysex.start;
        if (passed_ticks < midi.sysex.delay) { // still waiting for sysex delay
            critical_section_exit(&midi_crit);
            return; // Don't send data yet
        }
    }
    Bit8u data = midi_out_buff.buffer[midi_out_buff.tail];
    midi_out_buff.tail = (midi_out_buff.tail + 1) & RAWBUF_BITS;   // increment tail, wrap to 0 if we're at the end
    critical_section_exit(&midi_crit);

    if (midi.sysex.status==0xf0) { // Start 
        /* putchar('s'); */
        if (!(data&0x80)) {
            /*
            if (midi.sysex.used==SYSEX_SIZE) {
                midi.sysex.used = 0;
                midi.sysex.usedbufs++;
            }
            */
            
            output_to_uart(data);
            if (midi.sysex.used<(SYSEX_SIZE-1)) midi.sysex.buf[midi.sysex.used++] = data;
            /* midi.sysex.buf[midi.sysex.used++] = data; */
            return;
        } else {
            output_to_uart(0xf7);
            midi.sysex.buf[midi.sysex.used++] = 0xf7;
            midi.sysex.status = 0xf7;
                /*LOG(LOG_ALL,LOG_NORMAL)("Play sysex; address:%02X %02X %02X, length:%4d, delay:%3d", midi.sysex.buf[5], midi.sysex.buf[6], midi.sysex.buf[7], midi.sysex.used, midi.sysex.delay);*/
            if (midi.sysex.start) {
                /* if (midi.sysex.usedbufs == 0 && midi.sysex.buf[5] == 0x7F) { */
                if (midi.sysex.buf[5] == 0x7F) {
                    midi.sysex.delay = 290; // PicoGUS // All Parameters reset
                /* } else if (midi.sysex.usedbufs == 0 && midi.sysex.buf[5] == 0x10 && midi.sysex.buf[6] == 0x00 && midi.sysex.buf[7] == 0x04) { */
                } else if (midi.sysex.buf[5] == 0x10 && midi.sysex.buf[6] == 0x00 && midi.sysex.buf[7] == 0x04) {
                    midi.sysex.delay = 145;  // PicoGUS // Viking Child
                /* } else if (midi.sysex.usedbufs == 0 && midi.sysex.buf[5] == 0x10 && midi.sysex.buf[6] == 0x00 && midi.sysex.buf[7] == 0x01) { */
                } else if (midi.sysex.buf[5] == 0x10 && midi.sysex.buf[6] == 0x00 && midi.sysex.buf[7] == 0x01) {
                    midi.sysex.delay = 30;  // PicoGUS  // Dark Sun 1
                } else {
                    // HardMPU:
                    /* midi.sysex.delay = ((((midi.sysex.usedbufs*SYSEX_SIZE)+midi.sysex.used)/2)+2); */
                    // DOSBox:
                    /* midi.sysex.delay = (Bitu)(((float)(midi.sysex.used) * 1.25f) * 1000.0f / 3125.0f) + 2 */
                    // PicoGUS: 1.25 * 1000.0 / 3125.0 = 0.4
                    /* midi.sysex.delay = ((midi.sysex.usedbufs*SYSEX_SIZE)+midi.sysex.used) * 0.4f + 2; */
                    midi.sysex.delay = midi.sysex.used * 0.4f + 2;
                    if (midi.sysex.extra_delay && midi.sysex.delay < 40) {
                        midi.sysex.delay = 40;
                    }
                }
                midi.sysex.start = time_us_32();  // PicoGUS
            }
            return;
            /*LOG(LOG_ALL,LOG_NORMAL)("Sysex message size %d",midi.sysex.used);*/ /* SOFTMPU */
            /*if (CaptureState & CAPTURE_MIDI) {
                CAPTURE_AddMidi( true, midi.sysex.used-1, &midi.sysex.buf[1]);
            }*/ /* SOFTMPU */
        }
    }
    if (data&0x80) {
        midi.sysex.status=data;
        if (midi.sysex.status==0xf0) {
            output_to_uart(0xf0);
            midi.sysex.used=1;
            midi.sysex.buf[0]=0xf0;
            /* midi.sysex.usedbufs=0; */
            return;
        }
    }
    
    output_to_uart(data);   // not sysex
}

bool MIDI_Available(void)  {
    return midi.available;
}

/* SOFTMPU: Initialization */
void MIDI_Init(bool delaysysex,bool fakeallnotesoff){
    Bit8u i; /* SOFTMPU */
    // MIDI_sysex_delaytime = 0; /* SOFTMPU */
    // midi.sysex.delay = delaysysex;
    
    if (!critical_section_is_initialized(&midi_crit)) {
        critical_section_init(&midi_crit);
    }

    midi.status=0x00;
    midi.sysex.status=0x00;
    midi.sysex.delay = 0;
    midi.sysex.start = delaysysex ? time_us_32() : 0; // PicoGUS
    midi.sysex.extra_delay = delaysysex;
    /* midi.sysex.start = time_us_32(); // PicoGUS */
    midi.cmd_pos=0;
    midi.cmd_len=0;
    midi.fakeallnotesoff = fakeallnotesoff;
    midi.available=true;

    midi_out_buff.head = midi_out_buff.tail = 0;
        
    /* SOFTMPU: Display welcome message on MT-32 */
    for (i=0;i<30;i++)
    {
        send_midi_byte_now(MIDI_welcome_msg[i]);
    }
        
    /* HardMPU: Turn off any stuck notes */
    for (i=0xb0;i<0xc0;i++)
    {
        send_midi_byte_now(i);
        send_midi_byte_now(0x7b);
        send_midi_byte_now(0);
    }
        
    /* SOFTMPU: Init note tracking */
    for (i=0;i<MAX_TRACKED_CHANNELS;i++)
    {
        tracked_channels[i].used=0;
        tracked_channels[i].next=0;
    }
}
