/*
 *  Copyright (C) 2002-2021  The DOSBox Team
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

#include <assert.h>
#include <string.h>
#include <iomanip>
#include <sstream>
#include <cmath>

#include "dosbox-x-compat.h"

#include "hardware/gpio.h"
#ifdef PSRAM
#include "psram_spi.h"
extern pio_spi_inst_t psram_spi;
#endif

#include "pico/critical_section.h"
critical_section_t gus_crit;

#include "pico_pic.h"
#include "isa_dma.h"
extern dma_inst_t dma_config;

using namespace std;

//Extra bits of precision over normal gus
#define WAVE_FRACT 9
#define WAVE_FRACT_MASK ((1 << WAVE_FRACT)-1)
#define WAVE_MSWMASK ((1 << 16)-1)
#define WAVE_LSWMASK (0xffffffff ^ WAVE_MSWMASK)

//Amount of precision the volume has
#define RAMP_FRACT (10)
#define RAMP_FRACT_MASK ((1 << RAMP_FRACT)-1)

#define GUS_BASE myGUS.portbase
#define GUS_RATE myGUS.rate
#define LOG_GUS 0

#define VOL_SHIFT 14

#define WCTRL_STOPPED           0x01
#define WCTRL_STOP              0x02
#define WCTRL_16BIT             0x04
#define WCTRL_LOOP              0x08
#define WCTRL_BIDIRECTIONAL     0x10
#define WCTRL_IRQENABLED        0x20
#define WCTRL_DECREASING        0x40
#define WCTRL_IRQPENDING        0x80

#ifdef PSRAM
#define GUS_RAM_SIZE            (1024u*1024u)
#else
#define GUS_RAM_SIZE            (1024u*128u)
#endif

// fixed panning table (avx)
static uint16_t const pantablePDF[16] = { 0, 13, 26, 41, 57, 72, 94, 116, 141, 169, 203, 244, 297, 372, 500, 4095 };
static bool gus_fixed_table = false;

static uint16_t const sample_rates[32] = {
    44100,
    44100,
    44100,
    44100,
    44100,
    44100,
    44100,
    44100,
    44100,
    44100,
    44100,
    44100,
    44100,
    44100,
    41160,
    38587,
    36317,
    34300,
    32494,
    30870,
    29400,
    28063,
    26843,
    25725,
    24696,
    23746,
    22866,
    22050,
    21289,
    20580,
    19916,
    19293,
};

uint8_t adlib_commandreg;
// static MixerChannel * gus_chan;
static uint8_t const irqtable[8] = { 0/*invalid*/, 2, 5, 3, 7, 11, 12, 15 };
static uint8_t const dmatable[8] = { 0/*NO DMA*/, 1, 3, 5, 6, 7, 0/*invalid*/, 0/*invalid*/ };
#ifndef PSRAM
static uint8_t GUSRam[GUS_RAM_SIZE + 16/*safety margin*/]; // 1024K of GUS Ram
#endif
static int32_t AutoAmp = 512;
static bool unmask_irq = false;
static bool enable_autoamp = false;
static bool startup_ultrinit = false;
static bool ignore_active_channel_write_while_active = false;
static bool dma_enable_on_dma_control_polling = false;
static uint16_t vol16bit[4096];
static uint32_t pantable[16];
static bool gus_warn_irq_conflict = false;
static bool gus_warn_dma_conflict = false;

static uint32_t buffer_size = 16;

class GUSChannels;
static void CheckVoiceIrq(void);

struct GFGus {
    uint8_t gRegSelectData;     // what is read back from 3X3. not necessarily the index selected, but
    // apparently the last byte read OR written to ports 3X3-3X5 as seen
    // on actual GUS hardware.
    uint8_t gRegSelect;
    uint16_t gRegData;
    uint32_t gDramAddr;
    uint32_t gDramAddrMask;
    uint16_t gCurChannel;

    uint8_t gUltraMAXControl;
    uint16_t DMAControl; /* NTS: bit 8 for DMA TC IRQ status. Only bits [7:0] exist on real hardware.
                We're taking the DOSBox SVN approach here (https://sourceforge.net/p/dosbox/code-0/4387/#diff-2) */
    uint16_t dmaAddr;
    uint8_t dmaAddrOffset; /* bits 0-3 of the addr */
    uint8_t TimerControl;
    uint8_t SampControl;
    uint8_t mixControl;
    uint8_t ActiveChannels;
    uint8_t ActiveChannelsUser; /* what the guest wrote */
    uint8_t gRegControl;
    uint32_t basefreq;

    struct GusTimer {
        uint32_t delay;
        uint8_t value;
        bool reached;
        bool raiseirq;
        bool masked;
        bool running;
    } timers[2];

    uint32_t rate;
    Bitu portbase;
    uint32_t memsize;
    uint8_t dma1;
    uint8_t dma2;

    uint8_t irq1;       // GF1 IRQ
    uint8_t irq2;       // MIDI IRQ

    bool irqenabled;
    bool ChangeIRQDMA;
    bool initUnmaskDMA;
    bool force_master_irq_enable;
    bool clearTCIfPollingIRQStatus;
    double lastIRQStatusPollAt;
    int lastIRQStatusPollRapidCount;
    // IRQ status register values
    uint8_t IRQStatus;
    uint32_t ActiveMask;
    uint8_t IRQChan;
    uint32_t RampIRQ;
    uint32_t WaveIRQ;
    double masterVolume;    /* decibels */
    int32_t masterVolumeMul; /* 1<<9 fixed */

    void updateMasterVolume(void) {
        double vol = masterVolume;
        if (vol > 6) vol = 6; // allow some amplification but don't let it overflow
        masterVolumeMul = (int32_t)((1 << 9) * pow(10.0,vol / 20.0));
        if (AutoAmp > masterVolumeMul) AutoAmp = masterVolumeMul;
    }
} myGUS;

extern uint8_t GUS_activeChannels(void) {
    return myGUS.ActiveChannels;
}

extern uint32_t GUS_basefreq(void) {
    return myGUS.basefreq;
}

Bitu DEBUG_EnableDebugger(void);

static uint8_t GUS_reset_reg = 0;

class GUSChannels {
    public:
        uint32_t WaveStart;
        uint32_t WaveEnd;
        uint32_t WaveAddr;
        uint32_t WaveAdd;
        uint8_t  WaveCtrl;
        uint16_t WaveFreq;

        uint32_t RampStart;
        uint32_t RampEnd;
        uint32_t RampVol;
        uint32_t RampAdd;

        uint8_t RampRate;
        uint8_t RampCtrl;

        uint8_t PanPot;
        uint8_t channum;
        uint32_t irqmask;
        uint32_t PanLeft;
        uint32_t PanRight;
        int32_t VolLeft;
        int32_t VolRight;

        struct sample_cache_8 {
            // Signed so it can hold -1 for invalid address
            int32_t addr;
            uint16_t data;
        };
        mutable sample_cache_8 cache_8;

        struct sample_cache_16 {
            // Signed so it can hold -1 for invalid address
            int32_t addr;
            uint32_t data;
        };
        mutable sample_cache_16 cache_16;

        GUSChannels(uint8_t num) { 
            channum = num;
            irqmask = 1u << num;
            WaveStart = 0;
            WaveEnd = 0;
            WaveAddr = 0;
            WaveAdd = 0;
            WaveFreq = 0;
            WaveCtrl = 3;
            RampRate = 0;
            RampStart = 0;
            RampEnd = 0;
            RampCtrl = 3;
            RampAdd = 0;
            RampVol = 0;
            VolLeft = 0;
            VolRight = 0;
            PanLeft = 0;
            PanRight = 0;
            PanPot = 0x7;
            cache_8 = {-1, 0};
            cache_16 = {-1, 0};
        }

        void ClearCache(void) {
            cache_8.addr = -1;
            cache_16.addr = -1;
        }

        INLINE int32_t LoadSample8(const uint32_t addr/*memory address without fractional bits*/) const {
#ifdef PSRAM
            return (int8_t)psram_read8(&psram_spi, addr & 0xFFFFFu/*1MB*/) << int32_t(8);
#else
            return (int8_t)GUSRam[addr & 0xFFFFFu/*1MB*/] << int32_t(8); /* typecast to sign extend 8-bit value */
#endif
        }

        INLINE int32_t LoadSample16(const uint32_t addr/*memory address without fractional bits*/) const {
            const uint32_t adjaddr = (addr & 0xC0000u/*256KB bank*/) | ((addr & 0x1FFFFu) << 1u/*16-bit sample value within bank*/);
#ifdef PSRAM
            return (int16_t)psram_read16(&psram_spi, adjaddr);
#else
            return (int16_t)host_readw(GUSRam + adjaddr);/* typecast to sign extend 16-bit value */
#endif
        }

#ifdef PSRAM
        union uint8_t_pair {
            uint16_t data16;
            uint8_t data8[2];
        };

        union uint16_t_pair {
            uint32_t data32;
            uint16_t data16[2];
        };

        struct sample32_pair {
            int32_t low;
            int32_t high;
        };

        INLINE sample32_pair LoadSamples8(const uint32_t addr/*memory address without fractional bits*/) const {
            uint8_t_pair p;
            if (cache_8.addr == addr) {
                // putchar('.');
                p.data16 = cache_8.data;
            } else {
                // putchar('!');
                p.data16 = psram_read16(&psram_spi, addr & 0xFFFFFu/*1MB*/);
                cache_8.addr = addr;
                cache_8.data = p.data16;
            }
            // uint8_t_pair p = { .data16 = psram_read16(&psram_spi, addr & 0xFFFFFu/*1MB*/) };
            return (struct sample32_pair){
                .low = (int8_t)p.data8[0] << int32_t(8),
                .high = (int8_t)p.data8[1] << int32_t(8)
            };
        }

        INLINE sample32_pair LoadSamples16(const uint32_t addr/*memory address without fractional bits*/) const {
            const uint32_t adjaddr = (addr & 0xC0000u/*256KB bank*/) | ((addr & 0x1FFFFu) << 1u/*16-bit sample value within bank*/);
            uint16_t_pair p;
            if (cache_16.addr == addr) {
                // putchar('.');
                p.data32 = cache_16.data;
            } else {
                // putchar('!');
                p.data32 = psram_read32(&psram_spi, adjaddr);
                cache_16.addr = addr;
                cache_16.data = p.data32;
            }
            // uint16_t_pair p = { .data32 = psram_read32(&psram_spi, adjaddr) };
            return (struct sample32_pair){
                .low = (int16_t)p.data16[0],
                .high = (int16_t)p.data16[1]
            };
        }
#endif

        // Returns a single 16-bit sample from the Gravis's RAM
        INLINE int32_t GetSample8() const {
            /* LoadSample*() will take care of wrapping to 1MB */
            const uint32_t useAddr = WaveAddr >> WAVE_FRACT;
            {
                // Interpolate
#ifdef PSRAM
                sample32_pair p = LoadSamples8(useAddr);
                int32_t diff = p.high - p.low;
                int32_t scale = (int32_t)(WaveAddr & WAVE_FRACT_MASK);
                return (p.low + ((diff * scale) >> WAVE_FRACT));
#else
                int32_t w1 = LoadSample8(useAddr);
                int32_t w2 = LoadSample8(useAddr + 1u);
                int32_t diff = w2 - w1;
                int32_t scale = (int32_t)(WaveAddr & WAVE_FRACT_MASK);
                return (w1 + ((diff * scale) >> WAVE_FRACT));
#endif
            }
        }

        INLINE int32_t GetSample16() const {
            /* Load Sample*() will take care of wrapping to 1MB and funky bank/sample conversion */
            const uint32_t useAddr = WaveAddr >> WAVE_FRACT;
            {
                // Interpolate
#ifdef PSRAM
                sample32_pair p = LoadSamples16(useAddr);
                int32_t diff = p.high - p.low;
                int32_t scale = (int32_t)(WaveAddr & WAVE_FRACT_MASK);
                return (p.low + ((diff * scale) >> WAVE_FRACT));
#else
                int32_t w1 = LoadSample16(useAddr);
                int32_t w2 = LoadSample16(useAddr + 1u);
                int32_t diff = w2 - w1;
                int32_t scale = (int32_t)(WaveAddr & WAVE_FRACT_MASK);
                return (w1 + ((diff * scale) >> WAVE_FRACT));
#endif
            }
        }

        __force_inline void WriteWaveFreq(uint16_t val) {
            WaveFreq = val;
            WaveAdd = ((uint32_t)(val >> 1)) << ((uint32_t)(WAVE_FRACT-9));
        }
        __force_inline void WriteWaveCtrl(uint8_t val) {
            uint32_t oldirq=myGUS.WaveIRQ;
            WaveCtrl = val & 0x7f;

            if ((val & 0xa0)==0xa0) myGUS.WaveIRQ|=irqmask;
            else myGUS.WaveIRQ&=~irqmask;

            if (oldirq != myGUS.WaveIRQ) 
                CheckVoiceIrq();
        }
        INLINE uint8_t ReadWaveCtrl(void) {
            uint8_t ret=WaveCtrl;
            if (myGUS.WaveIRQ & irqmask) ret|=0x80;
            return ret;
        }
        __force_inline void UpdateWaveRamp(void) { 
            WriteWaveFreq(WaveFreq);
            WriteRampRate(RampRate);
        }
        __force_inline void WritePanPot(uint8_t val) {
            PanPot = val;
            PanLeft = pantable[val & 0xf];
            PanRight = pantable[0x0f-(val & 0xf)];
            UpdateVolumes();
        }
        __force_inline uint8_t ReadPanPot(void) {
            return PanPot;
        }
        __force_inline void WriteRampCtrl(uint8_t val) {
            uint32_t old=myGUS.RampIRQ;
            RampCtrl = val & 0x7f;
            //Manually set the irq
            if ((val & 0xa0) == 0xa0)
                myGUS.RampIRQ |= irqmask;
            else
                myGUS.RampIRQ &= ~irqmask;
            if (old != myGUS.RampIRQ)
                CheckVoiceIrq();
        }
        INLINE uint8_t ReadRampCtrl(void) {
            uint8_t ret=RampCtrl;
            if (myGUS.RampIRQ & irqmask) ret|=0x80;
            return ret;
        }
        __force_inline void WriteRampRate(uint8_t val) {
            RampRate = val;
            /* NTS: Note RAMP_FRACT == 10, shift = 10 - (3*(val>>6)).
             * From the upper two bits, the possible shift values for 0, 1, 2, 3 are: 10, 7, 4, 1 */
            RampAdd = ((uint32_t)(RampRate & 63)) << ((uint32_t)(RAMP_FRACT - (3*(val >> 6))));
#if 0//SET TO 1 TO CHECK YOUR MATH!
            double frameadd = (double)(RampRate & 63)/(double)(1 << (3*(val >> 6)));
            double realadd = frameadd * (double)(1 << RAMP_FRACT);
            uint32_t checkadd = (uint32_t)realadd;
            signed long error = (signed long)checkadd - (signed long)RampAdd;

            if (error < -1L || error > 1L)
                LOG_MSG("RampAdd nonfixed error %ld (%lu != %lu)",error,(unsigned long)checkadd,(unsigned long)RampAdd);
#endif
        }
        INLINE void WaveUpdate(void) {
            bool endcondition;

            if ((WaveCtrl & (WCTRL_STOP | WCTRL_STOPPED)) == 0/*voice is running*/) {
                /* NTS: WaveAddr and WaveAdd are unsigned.
                 *      If WaveAddr <= WaveAdd going backwards, WaveAddr becomes negative, which as an unsigned integer,
                 *      means carrying down from the highest possible value of the integer type. Which means that if the
                 *      start position is less than WaveAdd the WaveAddr will skip over the start pointer and continue
                 *      playing downward from the top of the GUS memory, without stopping/looping as expected.
                 *
                 *      This "bug" was implemented on purpose because real Gravis Ultrasound hardware acts this way. */
                uint32_t WaveExtra = 0;
                if (WaveCtrl & WCTRL_DECREASING/*backwards (direction)*/) {
                    /* unsigned int subtract, mask, compare. will miss start pointer if WaveStart <= WaveAdd.
                     * This bug is deliberate, accurate to real GUS hardware, do not fix. */
                    WaveAddr -= WaveAdd;
                    WaveAddr &= ((Bitu)1 << ((Bitu)WAVE_FRACT + (Bitu)20/*1MB*/)) - 1;
                    endcondition = (WaveAddr < WaveStart)?true:false;
                    if (endcondition) WaveExtra = WaveStart - WaveAddr;
                }
                else {
                    WaveAddr += WaveAdd;
                    endcondition = (WaveAddr > WaveEnd)?true:false;
                    WaveAddr &= ((Bitu)1 << ((Bitu)WAVE_FRACT + (Bitu)20/*1MB*/)) - 1;
                    if (endcondition) WaveExtra = WaveAddr - WaveEnd;
                }

                if (endcondition) {
                    if (WaveCtrl & WCTRL_IRQENABLED) /* generate an IRQ if requested */ {
                        critical_section_enter_blocking(&gus_crit);
                        myGUS.WaveIRQ |= irqmask;
                        critical_section_exit(&gus_crit);
                    }

                    if ((RampCtrl & WCTRL_16BIT/*roll over*/) && !(WaveCtrl & WCTRL_LOOP)) {
                        /* "3.11. Rollover feature
                         * 
                         * Each voice has a 'rollover' feature that allows an application to be notified when a voice's playback position passes
                         * over a particular place in DRAM.  This is very useful for getting seamless digital audio playback.  Basically, the GF1
                         * will generate an IRQ when a voice's current position is  equal to the end position.  However, instead of stopping or
                         * looping back to the start position, the voice will continue playing in the same direction.  This means that there will be
                         * no pause (or gap) in the playback.  Note that this feature is enabled/disabled through the voice's VOLUME control
                         * register (since there are no more bits available in the voice control registers).   A voice's loop enable bit takes
                         * precedence over the rollover.  This means that if a voice's loop enable is on, it will loop when it hits the end position,
                         * regardless of the state of the rollover enable."
                         *
                         * Despite the confusing description above, that means that looping takes precedence over rollover. If not looping, then
                         * rollover means to fire the IRQ but keep moving. If looping, then fire IRQ and carry out loop behavior. Gravis Ultrasound
                         * Windows 3.1 drivers expect this behavior, else Windows WAVE output will not work correctly. */
                    }
                    else {
                        if (WaveCtrl & WCTRL_LOOP) {
                            if (WaveCtrl & WCTRL_BIDIRECTIONAL) WaveCtrl ^= WCTRL_DECREASING/*change direction*/;
                            WaveAddr = (WaveCtrl & WCTRL_DECREASING) ? (WaveEnd - WaveExtra) : (WaveStart + WaveExtra);
                        } else {
                            WaveCtrl |= 1; /* stop the channel */
                            WaveAddr = (WaveCtrl & WCTRL_DECREASING) ? WaveStart : WaveEnd;
                        }
                    }
                }
            }
            else if (WaveCtrl & WCTRL_IRQENABLED) {
                /* Undocumented behavior observed on real GUS hardware: A stopped voice will still rapid-fire IRQs
                 * if IRQ enabled and current position <= start position OR current position >= end position */
                if (WaveCtrl & WCTRL_DECREASING/*backwards (direction)*/)
                    endcondition = (WaveAddr <= WaveStart)?true:false;
                else
                    endcondition = (WaveAddr >= WaveEnd)?true:false;

                if (endcondition) {
                    critical_section_enter_blocking(&gus_crit);
                    myGUS.WaveIRQ |= irqmask;
                    critical_section_exit(&gus_crit);
                }
            }
        }
        INLINE void UpdateVolumes(void) {
            int32_t templeft=(int32_t)RampVol - (int32_t)PanLeft;
            templeft&=~(templeft >> 31); /* <- NTS: This is a rather elaborate way to clamp negative values to zero using negate and sign extend */
            int32_t tempright=(int32_t)RampVol - (int32_t)PanRight;
            tempright&=~(tempright >> 31); /* <- NTS: This is a rather elaborate way to clamp negative values to zero using negate and sign extend */
            VolLeft=vol16bit[templeft >> RAMP_FRACT];
            VolRight=vol16bit[tempright >> RAMP_FRACT];
        }
        INLINE void RampUpdate(void) {
            if (RampCtrl & 0x3) return; /* if the ramping is turned off, then don't change the ramp */

            int32_t RampLeft;
            if (RampCtrl & 0x40) {
                RampVol-=RampAdd;
                if ((int32_t)RampVol < (int32_t)0) RampVol=0;
                RampLeft=(int32_t)RampStart-(int32_t)RampVol;
            } else {
                RampVol+=RampAdd;
                if (RampVol > ((4096 << RAMP_FRACT)-1)) RampVol=((4096 << RAMP_FRACT)-1);
                RampLeft=(int32_t)RampVol-(int32_t)RampEnd;
            }
            if (RampLeft<0) {
                UpdateVolumes();
                return;
            }
            /* Generate an IRQ if needed */
            if (RampCtrl & 0x20) {
                critical_section_enter_blocking(&gus_crit);
                myGUS.RampIRQ|=irqmask;
                critical_section_exit(&gus_crit);
            }
            /* Check for looping */
            if (RampCtrl & 0x08) {
                /* Bi-directional looping */
                if (RampCtrl & 0x10) RampCtrl^=0x40;
                RampVol = (RampCtrl & 0x40) ? (uint32_t)((int32_t)RampEnd-(int32_t)RampLeft) : (uint32_t)((int32_t)RampStart+(int32_t)RampLeft);
            } else {
                RampCtrl|=1;    //Stop the channel
                RampVol = (RampCtrl & 0x40) ? RampStart : RampEnd;
            }
            if ((int32_t)RampVol < (int32_t)0) RampVol=0;
            if (RampVol > ((4096 << RAMP_FRACT)-1)) RampVol=((4096 << RAMP_FRACT)-1);
            UpdateVolumes();
        }

        __force_inline void generateSamples(int32_t* stream, uint32_t len) {
            int32_t tmpsamp;
            int i;

            /* NTS: The GUS is *always* rendering the audio sample at the current position,
             *      even if the voice is stopped. This can be confirmed using DOSLIB, loading
             *      the Ultrasound test program, loading a WAV file into memory, then using
             *      the Ultrasound test program's voice control dialog to single-step the
             *      voice through RAM (abruptly change the current position) while the voice
             *      is stopped. You will hear "popping" noises come out the GUS audio output
             *      as the current position changes and the piece of the sample rendered
             *      abruptly changes as well. */
            // normal output
            for (i = 0; i < (int)len; i++) {
                // Get sample
                if (WaveCtrl & WCTRL_16BIT)
                    tmpsamp = GetSample16();
                else
                    tmpsamp = GetSample8();

                // Output stereo sample if DAC enable on
                if ((GUS_reset_reg & 0x02/*DAC enable*/) == 0x02) {
                    stream[i << 1] += tmpsamp * VolLeft;
                    stream[(i << 1) + 1] += tmpsamp * VolRight;
                    WaveUpdate();
                    RampUpdate();
                }
            }
        }
};

static GUSChannels *guschan[32] = {NULL};
static GUSChannels *curchan = NULL;

#if C_DEBUG
void DEBUG_PrintGUS() { //debugger "GUS" command
        LOG_MSG("GUS regsel=%02x regseld=%02x regdata=%02x DRAMaddr=%06x/%06x memsz=%06x curch=%02x MAXctrl=%02x regctl=%02x",
                        myGUS.gRegSelect,
                        myGUS.gRegSelectData,
                        myGUS.gRegData,
                        myGUS.gDramAddr,
                        myGUS.gDramAddrMask,
                        myGUS.memsize,
                        myGUS.gCurChannel,
                        myGUS.gUltraMAXControl,
                        myGUS.gRegControl);
        LOG_MSG("DMActrl=%02x (TC=%u) dmaAddr=%04x%01x timerctl=%02x sampctl=%02x mixctl=%02x activech=%u (want=%u) DACrate=%uHz",
                        myGUS.DMAControl&0xFF,
                        (myGUS.DMAControl&0x100)?1:0,
                        myGUS.dmaAddr,
                        myGUS.dmaAddrOffset,
                        myGUS.TimerControl,
                        myGUS.SampControl,
                        myGUS.mixControl,
                        myGUS.ActiveChannels,
                        myGUS.ActiveChannelsUser,
                        myGUS.basefreq);
        LOG_MSG("IRQen=%u IRQstat=%02x IRQchan=%04x RampIRQ=%04x WaveIRQ=%04x",
                        myGUS.irqenabled,
                        myGUS.IRQStatus,
                        myGUS.IRQChan,
                        myGUS.RampIRQ,
                        myGUS.WaveIRQ);
        for (size_t t=0;t < 2;t++) {
                LOG_MSG("Timer %u: delay=%dus value=%02x reached=%u raiseirq=%u masked=%u running=%u\n",
                        (unsigned int)t + 1u,
                        myGUS.timers[t].delay,
                        myGUS.timers[t].value,
                        myGUS.timers[t].reached,
                        myGUS.timers[t].raiseirq,
                        myGUS.timers[t].masked,
                        myGUS.timers[t].running);
        }
    for (size_t t=0;t < (size_t)myGUS.ActiveChannels;t++) {
                GUSChannels *ch = guschan[t];
                if (ch == NULL) continue;

        std::string line;

                switch (ch->WaveCtrl & 3) {
                        case 0:             line += " RUN  "; break;
                        case WCTRL_STOPPED:     line += " STOPD"; break;
                        case WCTRL_STOP:        line += " STOPN"; break;
                        case WCTRL_STOPPED|WCTRL_STOP:  line += " STOP!"; break;
                }

                if (ch->WaveCtrl & WCTRL_LOOP)
                        line += " LOOP";
                else if (ch->RampCtrl & WCTRL_16BIT/*roll over*/) /* !loop and (rampctl & 4) == rollover */
                        line += " ROLLOVER";

                if (ch->WaveCtrl & WCTRL_16BIT)
                        line += " PCM16";
                if (ch->WaveCtrl & WCTRL_BIDIRECTIONAL)
                        line += " BIDI";
                if (ch->WaveCtrl & WCTRL_IRQENABLED)
                        line += " IRQEN";
                if (ch->WaveCtrl & WCTRL_DECREASING)
                        line += " REV";
                if (ch->WaveCtrl & WCTRL_IRQPENDING)
                        line += " IRQP";

                LOG_MSG("Voice %u: start=%05x.%03x end=%05x.%03x addr=%05x.%03x add=%05x.%03x ctl=%02x rampctl=%02x%s",
                        (unsigned int)t + 1u,
                        ch->WaveStart>>WAVE_FRACT,
                        (ch->WaveStart&WAVE_FRACT_MASK)<<(12-WAVE_FRACT),//current WAVE_FRACT == 9
                        ch->WaveEnd>>WAVE_FRACT,
                        (ch->WaveEnd&WAVE_FRACT_MASK)<<(12-WAVE_FRACT),//current WAVE_FRACT == 9
                        ch->WaveAddr>>WAVE_FRACT,
                        (ch->WaveAddr&WAVE_FRACT_MASK)<<(12-WAVE_FRACT),//current WAVE_FRACT == 9
                        ch->WaveAdd>>WAVE_FRACT,
                        (ch->WaveAdd&WAVE_FRACT_MASK)<<(12-WAVE_FRACT),//current WAVE_FRACT == 9
                        ch->WaveCtrl,
                        ch->RampCtrl,
                        line.c_str());
                LOG_MSG("    Ramp start=%05x.%03x end=%05x.%03x vol=%05x.%03x add=%05x.%03x pan=%x",
                        ch->RampStart>>RAMP_FRACT,
                        (ch->RampStart&RAMP_FRACT_MASK)<<(12-RAMP_FRACT),//current RAMP_FRACT == 10
                        ch->RampEnd>>RAMP_FRACT,
                        (ch->RampEnd&RAMP_FRACT_MASK)<<(12-RAMP_FRACT),//current RAMP_FRACT == 10
                        ch->RampVol>>RAMP_FRACT,
                        (ch->RampVol&RAMP_FRACT_MASK)<<(12-RAMP_FRACT),//current RAMP_FRACT == 10
                        ch->RampAdd>>RAMP_FRACT,
                        (ch->RampAdd&RAMP_FRACT_MASK)<<(12-RAMP_FRACT),//current RAMP_FRACT == 10
                        ch->PanPot);
    }
}
#endif

static INLINE void GUS_CheckIRQ(void);

static uint32_t GUS_TimerEvent(Bitu val);

///* TODO implement DMA
// static void GUS_DMA_Callback(/*DmaChannel * chan,*/DMAEvent event);
//*/ // TODO implement DMA

void GUS_StopDMA();
void GUS_StartDMA();
void GUS_Update_DMA_Event_transfer();

static void GUSReset(void) {
    gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
    unsigned char p_GUS_reset_reg = GUS_reset_reg;

    /* NTS: From the Ultrasound SDK:
     *
     *      Global Data Low (3X4) is either a 16-bit transfer, or the low half of a 16-bit transfer with 8-bit I/O.
     *
     *      Global Data High (3X5) is either an 8-bit transfer for one of the GF registers or the high part of a 16-bit wide register with 8-bit I/O.
     *
     *      Prior to 2015/12/29 DOSBox and DOSBox-X contained a programming error here where reset and master IRQ enable were handled from the
     *      LOWER 8 bits, when the code should have been checking the UPPER 8 bits. Programming error #2 was the mis-interpetation of bit 0 (bit 8 of
     *      the gRegData). According to the SDK, clearing bit 0 triggers RESET, setting bit 0 starts the card running again. The original code had
     *      it backwards. */
    GUS_reset_reg = (myGUS.gRegData >> 8) & 7;

    if ((myGUS.gRegData & 0x400) != 0x000 || myGUS.force_master_irq_enable)
        myGUS.irqenabled = true;
    else
        myGUS.irqenabled = false;

    if (GUS_reset_reg ^ p_GUS_reset_reg)
        LOG_MSG("GUS reset with 0x%04X",myGUS.gRegData);

    if ((myGUS.gRegData & 0x100) == 0x000) {
        // Stop all channels
        int i;
        for(i=0;i<32;i++) {
            guschan[i]->RampVol=0;
            guschan[i]->WriteWaveCtrl(0x1);
            guschan[i]->WriteRampCtrl(0x1);
            guschan[i]->WritePanPot(0x7);
            guschan[i]->ClearCache();
        }

        // Stop DMA
        GUS_StopDMA();

        // Reset
        adlib_commandreg = 85;
        myGUS.IRQStatus = 0;
        myGUS.RampIRQ = 0;
        myGUS.WaveIRQ = 0;
        myGUS.IRQChan = 0;

        myGUS.timers[0].delay = 80;
        myGUS.timers[1].delay = 320;
        myGUS.timers[0].value = 0xff;
        myGUS.timers[1].value = 0xff;
        myGUS.timers[0].masked = false;
        myGUS.timers[1].masked = false;
        myGUS.timers[0].raiseirq = false;
        myGUS.timers[1].raiseirq = false;
        myGUS.timers[0].reached = true;
        myGUS.timers[1].reached = true;
        myGUS.timers[0].running = false;
        myGUS.timers[1].running = false;

        PIC_RemoveEvents(GUS_TimerEvent);

        myGUS.ChangeIRQDMA = false;
        myGUS.DMAControl = 0x00;
        myGUS.mixControl = 0x0b;    // latches enabled by default LINEs disabled
        myGUS.TimerControl = 0x00;
        myGUS.SampControl = 0x00;
        myGUS.ActiveChannels = 14;
        myGUS.ActiveChannelsUser = 14;
        myGUS.ActiveMask=0xffffffffU >> (32-myGUS.ActiveChannels);
        // myGUS.basefreq = (uint32_t)(1000000.0/(1.619695497*(float)(myGUS.ActiveChannels)));
        myGUS.basefreq = sample_rates[myGUS.ActiveChannels - 1];

        myGUS.gCurChannel = 0;
        curchan = guschan[myGUS.gCurChannel];

        myGUS.dmaAddr = 0;
        myGUS.irqenabled = 0;
        myGUS.gRegControl = 0;
        myGUS.dmaAddrOffset = 0;
        myGUS.gDramAddr = 0;
        myGUS.gRegData = 0;

        // /* TODO implmeent DMA
        // GUS_Update_DMA_Event_transfer();
        // */
    }

    /* if the card was just put into reset, or the card WAS in reset, bits 1-2 are cleared */
    if ((GUS_reset_reg & 1) == 0 || (p_GUS_reset_reg & 1) == 0) {
        /* GUS classic observed behavior: resetting the card, or even coming out of reset, clears bits 1-2.
         * That means, if you write any value to GUS RESET with bit 0 == 0, bits 1-2 become zero as well.
         * And if you take the card out of reset, bits 1-2 are zeroed.
         *
         * test 1:
         * outb(0x3X3,0x4C); outb(0x3X5,0x00);
         * outb(0x3X3,0x4C); c = inb(0x3X5);      <- you'll get 0x00 as expected
         * outb(0x3X3,0x4C); outb(0x3X5,0x07);
         * outb(0x3X3,0x4C); c = inb(0x3X5);      <- you'll get 0x01, not 0x07
         *
         * test 2:
         * outb(0x3X3,0x4C); outb(0x3X5,0x00);
         * outb(0x3X3,0x4C); c = inb(0x3X5);      <- you'll get 0x00 as expected
         * outb(0x3X3,0x4C); outb(0x3X5,0x01);
         * outb(0x3X3,0x4C); c = inb(0x3X5);      <- you'll get 0x01 as expected, card taken out of reset
         * outb(0x3X3,0x4C); outb(0x3X5,0x07);
         * outb(0x3X3,0x4C); c = inb(0x3X5);      <- you'll get 0x07 as expected
         * outb(0x3X3,0x4C); outb(0x3X5,0x06);    <- bit 0 == 0, we're trying to set bits 1-2
         * outb(0x3X3,0x4C); c = inb(0x3X5);      <- you'll get 0x00, not 0x06, card is in reset state */
        myGUS.irqenabled = myGUS.force_master_irq_enable; // IRQ enable resets, unless user specified we force it on
        GUS_reset_reg &= 1;
    }

    GUS_CheckIRQ();
}

__force_inline static uint8_t GUS_EffectiveIRQStatus(void) {
    uint8_t ret = 0;
    /* Behavior observed on real GUS hardware: Master IRQ enable bit 2 of the reset register affects only voice/wave
     * IRQ signals from the GF1. It does not affect the DMA terminal count interrupt nor does it affect the Adlib timers.
     * This is how "Juice" by Psychic Link is able to play music by GUS timer even though the demo never enables the
     * Master IRQ Enable bit. */

    /* DMA */
    if (myGUS.DMAControl & 0x20/*DMA IRQ Enable*/)
        ret |= (myGUS.IRQStatus & 0x80/*DMA TC IRQ*/);

    /* Timer 1 & 2 */
    ret |= (myGUS.IRQStatus/*Timer 1&2 IRQ*/ & myGUS.TimerControl/*Timer 1&2 IRQ Enable*/ & 0x0C);

    /* Voice IRQ */
    if (myGUS.irqenabled)
        ret |= (myGUS.IRQStatus & 0x60/*Wave/Ramp IRQ*/);

    /* TODO: MIDI IRQ? */
    return ret;
}

static uint8_t gus_prev_effective_irqstat = 0;

static INLINE void GUS_CheckIRQ(void) {
    if (myGUS.mixControl & 0x08/*Enable latches*/) {
        uint8_t irqstat = GUS_EffectiveIRQStatus();

        if (irqstat != 0 /*&& gus_prev_effective_irqstat == 0*/) {
            /* The GUS fires an IRQ, then waits for the interrupt service routine to
             * clear all pending interrupt events before firing another one. if you
             * don't service all events, then you don't get another interrupt. */
                // puts("activateirq");
                PIC_ActivateIRQ();
        } else if (gus_prev_effective_irqstat != 0) {
            // puts("deactivateirq");
            PIC_DeActivateIRQ();
        }

        gus_prev_effective_irqstat = irqstat;
    }
}

__force_inline static void CheckVoiceIrq(void) {
    critical_section_enter_blocking(&gus_crit);
    Bitu totalmask=(myGUS.RampIRQ|myGUS.WaveIRQ) & myGUS.ActiveMask;
    if (!totalmask) {
        GUS_CheckIRQ();
        critical_section_exit(&gus_crit);
        return;
    }

    if (myGUS.RampIRQ) myGUS.IRQStatus|=0x40;
    if (myGUS.WaveIRQ) myGUS.IRQStatus|=0x20;
    // mega hack
    // PIC_DeActivateIRQ();
    GUS_CheckIRQ();
    for (;;) {
        uint32_t check=(1u << myGUS.IRQChan);
        if (totalmask & check) {
            critical_section_exit(&gus_crit);
            return;
        }
        myGUS.IRQChan++;
        if (myGUS.IRQChan>=myGUS.ActiveChannels) myGUS.IRQChan=0;
    }
    critical_section_exit(&gus_crit);
}

uint32_t CheckVoiceIrq_async(Bitu val) {
    CheckVoiceIrq();
    return 0;
}

__force_inline static uint16_t ExecuteReadRegister(void) {
    uint8_t tmpreg;
//  LOG_MSG("Read global reg %x",myGUS.gRegSelect);
    switch (myGUS.gRegSelect) {
    case 0x8E:  // read active channel register
        // NTS: The GUS SDK documents the active channel count as bits 5-0, which is wrong. it's bits 4-0. bits 7-5 are always 1 on real hardware.
        return ((uint16_t)(0xE0 | (myGUS.ActiveChannelsUser - 1))) << 8;
    case 0x41: // Dma control register - read acknowledges DMA IRQ
        /* here in the real world we can't ask the DMA controller if it's masked
        if (dma_enable_on_dma_control_polling) {
            if (!GetDMAChannel(myGUS.dma1)->masked && !(myGUS.DMAControl & 0x01) && !(myGUS.IRQStatus & 0x80)) {
                DEBUG_LOG_MSG("GUS: As instructed, switching on DMA ENABLE upon polling DMA control register (HACK) as workaround");
                myGUS.DMAControl |= 0x01;
                GUS_StartDMA();
            }
        }
        */

        tmpreg = myGUS.DMAControl & 0xbf;
        tmpreg |= (myGUS.DMAControl & 0x100) >> 2; /* Bit 6 on read is the DMA terminal count IRQ status */
        myGUS.DMAControl&=0xff; /* clear TC IRQ status */
        myGUS.IRQStatus&=0x7f;
        GUS_CheckIRQ();
        return (uint16_t)(tmpreg << 8);
    case 0x42:  // Dma address register
        return myGUS.dmaAddr;
    case 0x45:  // Timer control register.  Identical in operation to Adlib's timer
        return (uint16_t)(myGUS.TimerControl << 8);
        break;
    case 0x49:  // Dma sample register
        tmpreg = myGUS.DMAControl & 0xbf;
        tmpreg |= (myGUS.DMAControl & 0x100) >> 2; /* Bit 6 on read is the DMA terminal count IRQ status */
        return (uint16_t)(tmpreg << 8);
    case 0x4c:  // GUS reset register
        tmpreg = (GUS_reset_reg & ~0x4) | (myGUS.irqenabled ? 0x4 : 0x0);
        /* GUS Classic observed behavior: You can read Register 4Ch from both 3X4 and 3X5 and get the same 8-bit contents */
        return ((uint16_t)(tmpreg << 8) | (uint16_t)tmpreg);
    case 0x80: // Channel voice control read register
        if (curchan) return curchan->ReadWaveCtrl() << 8;
        else return 0x0300;
    case 0x81:  // Channel frequency control register
        if(curchan) return (uint16_t)(curchan->WaveFreq);
        else return 0x0000;
    case 0x82: // Channel MSB start address register
        if (curchan) return (uint16_t)(curchan->WaveStart >> 16);
        else return 0x0000;
    case 0x83: // Channel LSW start address register
        if (curchan) return (uint16_t)(curchan->WaveStart);
        else return 0x0000;
    case 0x84: // Channel MSB end address register
        if (curchan) return (uint16_t)(curchan->WaveEnd >> 16);
        else return 0x0000;
    case 0x85: // Channel LSW end address register
        if (curchan) return (uint16_t)(curchan->WaveEnd);
        else return 0x0000;

    case 0x89: // Channel volume register
        if (curchan) return (uint16_t)((curchan->RampVol >> RAMP_FRACT) << 4);
        else return 0x0000;
    case 0x8a: // Channel MSB current address register
        if (curchan) return (uint16_t)(curchan->WaveAddr >> 16);
        else return 0x0000;
    case 0x8b: // Channel LSW current address register
        if (curchan) return (uint16_t)(curchan->WaveAddr);
        else return 0x0000;
    case 0x8c: // Channel pan pot register
        if (curchan) return (uint16_t)(curchan->PanPot << 8);
        else return 0x0800;
    case 0x8d: // Channel volume control register
        if (curchan) return curchan->ReadRampCtrl() << 8;
        else return 0x0300;
    case 0x8f: // General channel IRQ status register
        tmpreg=myGUS.IRQChan|0x20;
        uint32_t mask;
        mask=1u << myGUS.IRQChan;
        if (!(myGUS.RampIRQ & mask)) tmpreg|=0x40;
        if (!(myGUS.WaveIRQ & mask)) tmpreg|=0x80;
        myGUS.RampIRQ&=~mask;
        myGUS.WaveIRQ&=~mask;
        myGUS.IRQStatus&=0x9f;
        // mega hack
        // PIC_DeActivateIRQ();
        CheckVoiceIrq();
        // PIC_AddEvent(CheckVoiceIrq_async, 2, 3);
        return (uint16_t)(tmpreg << 8);
    default:
#if LOG_GUS
        LOG_MSG("Read Register num 0x%x", myGUS.gRegSelect);
#endif
        return myGUS.gRegData;
    }
}

static uint32_t GUS_TimerEvent(Bitu val) {
    // putchar('-');
    critical_section_enter_blocking(&gus_crit);
    if (!myGUS.timers[val].masked) myGUS.timers[val].reached=true;
    if (myGUS.timers[val].raiseirq) {
        myGUS.IRQStatus|=0x4 << val;
        GUS_CheckIRQ();
    }
    if (myGUS.timers[val].running) {
        critical_section_exit(&gus_crit);
        // putchar('.');
        return myGUS.timers[val].delay;  // Keep timer running
    }
    critical_section_exit(&gus_crit);
    // putchar('.');
    return 0;  // Stop timer
}

 
__force_inline static void ExecuteGlobRegister(void) {
    int i;
//  if (myGUS.gRegSelect|1!=0x44) LOG_MSG("write global register %x with %x", myGUS.gRegSelect, myGUS.gRegData);
    switch(myGUS.gRegSelect) {
    case 0x0:  // Channel voice control register
        // TODO figure out what FillUp even does!!
        // gus_chan->FillUp();
        if(curchan) curchan->WriteWaveCtrl((uint16_t)myGUS.gRegData>>8);
        break;
    case 0x1:  // Channel frequency control register
        // gus_chan->FillUp();
        if(curchan) curchan->WriteWaveFreq(myGUS.gRegData);
        break;
    case 0x2:  // Channel MSW start address register
        if (curchan) {
            uint32_t tmpaddr = (uint32_t)(myGUS.gRegData & 0x1fff) << 16; /* upper 13 bits of integer portion */
            curchan->WaveStart = (curchan->WaveStart & WAVE_MSWMASK) | tmpaddr;
        }
        break;
    case 0x3:  // Channel LSW start address register
        if(curchan != NULL) {
            uint32_t tmpaddr = (uint32_t)(myGUS.gRegData & 0xffe0); /* lower 7 bits of integer portion, and all 4 bits of fractional portion. bits 4-0 of the incoming 16-bit WORD are not used */
            curchan->WaveStart = (curchan->WaveStart & WAVE_LSWMASK) | tmpaddr;
        }
        break;
    case 0x4:  // Channel MSW end address register
        if(curchan != NULL) {
            uint32_t tmpaddr = (uint32_t)(myGUS.gRegData & 0x1fff) << 16; /* upper 13 bits of integer portion */
            curchan->WaveEnd = (curchan->WaveEnd & WAVE_MSWMASK) | tmpaddr;
        }
        break;
    case 0x5:  // Channel MSW end address register
        if(curchan != NULL) {
            uint32_t tmpaddr = (uint32_t)(myGUS.gRegData & 0xffe0); /* lower 7 bits of integer portion, and all 4 bits of fractional portion. bits 4-0 of the incoming 16-bit WORD are not used */
            curchan->WaveEnd = (curchan->WaveEnd & WAVE_LSWMASK) | tmpaddr;
        }
        break;
    case 0x6:  // Channel volume ramp rate register
        // gus_chan->FillUp();
        if(curchan != NULL) {
            uint8_t tmpdata = (uint16_t)myGUS.gRegData>>8;
            curchan->WriteRampRate(tmpdata);
        }
        break;
    case 0x7:  // Channel volume ramp start register  EEEEMMMM
        if(curchan != NULL) {
            uint8_t tmpdata = (uint16_t)myGUS.gRegData >> 8;
            curchan->RampStart = (uint32_t)(tmpdata << (4+RAMP_FRACT));
        }
        break;
    case 0x8:  // Channel volume ramp end register  EEEEMMMM
        if(curchan != NULL) {
            uint8_t tmpdata = (uint16_t)myGUS.gRegData >> 8;
            curchan->RampEnd = (uint32_t)(tmpdata << (4+RAMP_FRACT));
        }
        break;
    case 0x9:  // Channel current volume register
        // gus_chan->FillUp();
        if(curchan != NULL) {
            uint16_t tmpdata = (uint16_t)myGUS.gRegData >> 4;
            curchan->RampVol = (uint32_t)(tmpdata << RAMP_FRACT);
            curchan->UpdateVolumes();
        }
        break;
    case 0xA:  // Channel MSW current address register
        // gus_chan->FillUp();
        if(curchan != NULL) {
            uint32_t tmpaddr = (uint32_t)(myGUS.gRegData & 0x1fff) << 16; /* upper 13 bits of integer portion */
            curchan->WaveAddr = (curchan->WaveAddr & WAVE_MSWMASK) | tmpaddr;
        }
        break;
    case 0xB:  // Channel LSW current address register
        // gus_chan->FillUp();
        if(curchan != NULL) {
            uint32_t tmpaddr = (uint32_t)(myGUS.gRegData & 0xffff); /* lower 7 bits of integer portion, and all 9 bits of fractional portion */
            curchan->WaveAddr = (curchan->WaveAddr & WAVE_LSWMASK) | tmpaddr;
        }
        break;
    case 0xC:  // Channel pan pot register
        // gus_chan->FillUp();
        if(curchan) curchan->WritePanPot((uint16_t)myGUS.gRegData>>8);
        break;
    case 0xD:  // Channel volume control register
        // gus_chan->FillUp();
        if(curchan) curchan->WriteRampCtrl((uint16_t)myGUS.gRegData>>8);
        break;
    case 0xE:  // Set active channel register
        /* Hack for "Ice Fever" demoscene production:
         * If the DAC is active (bit 1 of GUS reset is set), ignore writes to this register.
         * The demo resets the GUS with 14 channels, then after reset changes it to 16 for some reason.
         * Without this hack, music will sound slowed down and wrong.
         * As far as I know, real hardware will accept the change immediately and produce the same
         * slowed down sound music. --J.C. */
        if (ignore_active_channel_write_while_active) {
            if (GUS_reset_reg & 0x02/*DAC enable*/) {
                LOG_MSG("GUS: Attempt to change active channel count while DAC active rejected");
                break;
            }
        }

        // gus_chan->FillUp();
        myGUS.gRegSelect = myGUS.gRegData>>8;       //JAZZ Jackrabbit seems to assume this?
        myGUS.ActiveChannelsUser = 1+((myGUS.gRegData>>8) & 31); // NTS: The GUS SDK documents this field as bits 5-0, which is wrong, it's bits 4-0. 5-0 would imply 64 channels.

        /* The GUS SDK claims that if a channel count less than 14 is written, then it caps to 14.
         * That's not true. Perhaps what the SDK is doing, but the actual hardware acts differently.
         * This implementation is based on what the Gravis Ultrasound MAX actually does with this
         * register. You can apparently achieve higher than 44.1KHz sample rates by programming less
         * than 14 channels, and the sample rate scale ramps up appropriately, except that values
         * 0 and 1 have the same effect as writing 2 and 3. Very useful undocumented behavior!
         * If Gravis were smart, they would have been able to claim 48KHz sample rates by allowing
         * less than 14 channels in their SDK! Not sure why they would cap it like that, unless
         * there are undocumented chipset instabilities with running at higher rates.
         *
         * So far only verified on a Gravis Ultrasound MAX.
         *
         * Does anyone out there have a Gravis Ultrasound Classic (original 1992 version) they can
         * test for this behavior?
         *
         * NOTED: Gravis Ultrasound Plug & Play (interwave) cards *do* enforce the 14-channel minimum.
         *        You can write less than 14 channels to this register, but unlike the Classic and Max
         *        cards they will not run faster than 44.1KHz. */
        myGUS.ActiveChannels = myGUS.ActiveChannelsUser;

        // force min channels to 14 - I don't want to subject the poor RP2040 to anything more than 44.1kHz
        if(myGUS.ActiveChannels < 14) myGUS.ActiveChannels = 14;
        if(myGUS.ActiveChannels > 32) myGUS.ActiveChannels = 32;

        myGUS.ActiveMask=0xffffffffU >> (32-myGUS.ActiveChannels);
        // myGUS.basefreq = (uint32_t)(1000000.0/(1.619695497*(float)(myGUS.ActiveChannels)));
        myGUS.basefreq = sample_rates[myGUS.ActiveChannels - 1];

#if LOG_GUS
        LOG_MSG("GUS set to %d channels freq=%luHz", myGUS.ActiveChannels,(unsigned long)myGUS.basefreq);
#endif
        for (i=0;i<myGUS.ActiveChannels;i++) guschan[i]->UpdateWaveRamp();
        break;
    case 0x10:  // Undocumented register used in Fast Tracker 2
        break;
    case 0x41:  // Dma control register
        myGUS.DMAControl &= ~0xFFu; // FIXME: Does writing DMA Control clear the DMA TC IRQ?
        myGUS.DMAControl |= (uint8_t)(myGUS.gRegData>>8);
        // GUS_Update_DMA_Event_transfer();
        if (myGUS.DMAControl & 1) GUS_StartDMA();
        else GUS_StopDMA();
        break;
    case 0x42:  // Gravis DRAM DMA address register
        myGUS.dmaAddr = myGUS.gRegData;
        myGUS.dmaAddrOffset = 0;
        break;
    case 0x43:  // LSB Peek/poke DRAM position
        myGUS.gDramAddr = (0xff0000 & myGUS.gDramAddr) | ((uint32_t)myGUS.gRegData);
        break;
    case 0x44:  // MSW Peek/poke DRAM position
        myGUS.gDramAddr = (0xffff & myGUS.gDramAddr) | ((uint32_t)myGUS.gRegData>>8) << 16;
        break;
    case 0x45:  // Timer control register.  Identical in operation to Adlib's timer
        myGUS.TimerControl = (uint8_t)(myGUS.gRegData>>8);
        myGUS.timers[0].raiseirq=(myGUS.TimerControl & 0x04)>0;
        if (!myGUS.timers[0].raiseirq) myGUS.IRQStatus&=~0x04;
        myGUS.timers[1].raiseirq=(myGUS.TimerControl & 0x08)>0;
        if (!myGUS.timers[1].raiseirq) myGUS.IRQStatus&=~0x08;
        if (!myGUS.timers[0].raiseirq && !myGUS.timers[1].raiseirq) {
            GUS_CheckIRQ();
        }
        break;
    case 0x46:  // Timer 1 control
        myGUS.timers[0].value = (uint8_t)(myGUS.gRegData>>8);
        myGUS.timers[0].delay = ((0x100 - myGUS.timers[0].value) * 80) - 5;
        break;
    case 0x47:  // Timer 2 control
        myGUS.timers[1].value = (uint8_t)(myGUS.gRegData>>8);
        myGUS.timers[1].delay = ((0x100 - myGUS.timers[1].value) * 320) - 5;
        break;
    case 0x49:  // DMA sampling control register
        myGUS.SampControl = (uint8_t)(myGUS.gRegData>>8);
        if (myGUS.DMAControl & 1) GUS_StartDMA();
        else GUS_StopDMA();
        break;
    case 0x4c:  // GUS reset register
        GUSReset();
        break;
    default:
#if LOG_GUS
        LOG_MSG("Unimplemented global register %x -- %x", myGUS.gRegSelect, myGUS.gRegData);
#endif
        break;
    }
    return;
}

__force_inline Bitu read_gus(Bitu port) {
    uint16_t reg16;

//  LOG_MSG("read from gus port %x",port);

    switch(port - GUS_BASE) {
    case 0x206:
#if 0 // no fancy stuff
        if (myGUS.clearTCIfPollingIRQStatus) {
            double t = PIC_FullIndex();

            /* Hack: "Warcraft II" by Blizzard entertainment.
             *
             * If you configure the game to use the Gravis Ultrasound for both music and sound, the GUS support code for digital
             * sound will cause the game to hang if music is playing at the same time within the main menu. The bug is that there
             * is code (in real mode no less) that polls the IRQ status register (2X6) and handles each IRQ event to clear it,
             * however there is no condition to handle clearing the DMA Terminal Count IRQ flag. The routine will not terminate
             * until all bits are cleared, hence, the game hangs after starting a sound effect. Most often, this is visible to
             * the user as causing the game to hang when you click on a button on the main menu.
             *
             * This hack attempts to detect the bug by looking for rapid polling of this register in a short period of time.
             * If detected, we clear the DMA IRQ flag to help the loop terminate so the game continues to run. */
            if (t < (myGUS.lastIRQStatusPollAt + 0.1/*ms*/)) {
                myGUS.lastIRQStatusPollAt = t;
                myGUS.lastIRQStatusPollRapidCount++;
                if (myGUS.clearTCIfPollingIRQStatus && (myGUS.IRQStatus & 0x80) && myGUS.lastIRQStatusPollRapidCount >= 500) {
                    DEBUG_LOG_MSG("GUS: Clearing DMA TC IRQ status, DOS application appears to be stuck");
                    myGUS.lastIRQStatusPollRapidCount = 0;
                    myGUS.lastIRQStatusPollAt = t;
                    myGUS.IRQStatus &= 0x7F;
                    GUS_CheckIRQ();
                }
            }
            else {
                myGUS.lastIRQStatusPollAt = t;
                myGUS.lastIRQStatusPollRapidCount = 0;
            }
        }
#endif // 0 // no fancy stuff

        /* NTS: Contrary to some false impressions, GUS hardware does not report "one at a time", it really is a bitmask.
         *      I had the funny idea you read this register "one at a time" just like reading the IRQ reason bits of the RS-232 port --J.C. */
        return GUS_EffectiveIRQStatus();
    case 0x208:
        uint8_t tmptime;
        tmptime = 0;
        if (myGUS.timers[0].reached) tmptime |= (1 << 6);
        if (myGUS.timers[1].reached) tmptime |= (1 << 5);
        if (tmptime & 0x60) tmptime |= (1 << 7);
        if (myGUS.IRQStatus & 0x04) tmptime|=(1 << 2);
        if (myGUS.IRQStatus & 0x08) tmptime|=(1 << 1);
        return tmptime;
    case 0x20a:
        return adlib_commandreg;
    case 0x20f:
        return ~0ul; // should not happen
    case 0x302:
        return myGUS.gRegSelectData;
    case 0x303:
        return myGUS.gRegSelectData;
    case 0x304:
        reg16 = ExecuteReadRegister() & 0xff;

        // Versions prior to the Interwave will reflect last I/O to 3X2-3X5 when read back from 3X3
        myGUS.gRegSelectData = reg16/* & 0xFF*/;

        return reg16;
    case 0x305:
        reg16 = ExecuteReadRegister() >> 8;

        //  Versions prior to the Interwave will reflect last I/O to 3X2-3X5 when read back from 3X3
        myGUS.gRegSelectData = reg16 & 0xFF;

        return reg16;
    case 0x307:
        if((myGUS.gDramAddr & myGUS.gDramAddrMask) < myGUS.memsize) {
#ifdef PSRAM
            return psram_read8(&psram_spi, myGUS.gDramAddr & myGUS.gDramAddrMask);
#else
            return GUSRam[myGUS.gDramAddr & myGUS.gDramAddrMask];
#endif
        } else {
            return 0;
        }
    case 0x306:
    default:
#if LOG_GUS
        LOG_MSG("Read GUS at port 0x%x", port);
#endif
        break;
    }

    return 0xff;
}


__force_inline void write_gus(Bitu port, Bitu val) {
//  LOG_MSG("Write gus port %x val %x",port,val);

    switch(port - GUS_BASE) {
    case 0x200:
        myGUS.gRegControl = 0;
        myGUS.mixControl = (uint8_t)val;
        myGUS.ChangeIRQDMA = true;
        // return;
        break;
    case 0x202:
        // PICOGUS special port to set audio buffer size
        buffer_size = val + 1;
        break;
    case 0x208:
        adlib_commandreg = (uint8_t)val;
        break;
    case 0x209:
//TODO adlib_commandreg should be 4 for this to work else it should just latch the value
        if (val & 0x80) {
            myGUS.timers[0].reached=false;
            myGUS.timers[1].reached=false;
            GUS_CheckIRQ();
            // return;
            break;
        }
        myGUS.timers[0].masked=(val & 0x40)>0;
        myGUS.timers[1].masked=(val & 0x20)>0;
        if (val & 0x1) {
            if (!myGUS.timers[0].running) {
                PIC_AddEvent(GUS_TimerEvent,myGUS.timers[0].delay,0);
                myGUS.timers[0].running=true;
            }
        } else myGUS.timers[0].running=false;
        if (val & 0x2) {
            if (!myGUS.timers[1].running) {
                PIC_AddEvent(GUS_TimerEvent,myGUS.timers[1].delay,1);
                myGUS.timers[1].running=true;
            }
        } else myGUS.timers[1].running=false;
        break;
//TODO Check if 0x20a register is also available on the gus like on the interwave
    case 0x20b:
        if (!myGUS.ChangeIRQDMA) break;
        myGUS.ChangeIRQDMA=false;

        if (myGUS.gRegControl == 6) {
            /* Jumper register:
             *
             * 7: reserved
             * 6: reserved
             * 5: reserved
             * 4: reserved
             * 3: reserved
             * 2: enable joystick port decode
             * 1: enable midi port address decode
             * 0: reserved */
            /* TODO: */
            LOG_MSG("GUS: DOS application wrote to 2XB register control number 0x06 (Jumper) val=%02xh",(int)val);
        }
        else if (myGUS.gRegControl == 5) {
            /* write a 0 to clear IRQs on power up (?) */
            LOG_MSG("GUS: DOS application wrote to 2XB register control number 0x05 (Clear IRQs) val=%02xh",(int)val);
        }
        else if (myGUS.gRegControl == 0) {
            if (myGUS.mixControl & 0x40) {
                // GUS SDK: IRQ Control Register
                //     Channel 1 GF1 IRQ selector (bits 2-0)
                //       0=reserved, do not use
                //       1=IRQ2
                //       2=IRQ5
                //       3=IRQ3
                //       4=IRQ7
                //       5=IRQ11
                //       6=IRQ12
                //       7=IRQ15
                //     Channel 2 MIDI IRQ selector (bits 5-3)
                //       0=no interrupt
                //       1=IRQ2
                //       2=IRQ5
                //       3=IRQ3
                //       4=IRQ7
                //       5=IRQ11
                //       6=IRQ12
                //       7=IRQ15
                //     Combine both IRQs using channel 1 (bit 6)
                //     Reserved (bit 7)
                //
                //     "If both channels are sharing an IRQ, channel 2's IRQ must be set to 0 and turn on bit 6. A
                //      bus conflict will occur if both latches are programmed with the same IRQ #."
                if (irqtable[val & 0x7] != 0)
                    myGUS.irq1 = irqtable[val & 0x7];

                if (val & 0x40) // "Combine both IRQs"
                    myGUS.irq2 = myGUS.irq1;
                else if (((val >> 3) & 7) != 0)
                    myGUS.irq2 = irqtable[(val >> 3) & 0x7];

                LOG_MSG("GUS IRQ reprogrammed: GF1 IRQ %d, MIDI IRQ %d",(int)myGUS.irq1,(int)myGUS.irq2);

                gus_warn_irq_conflict = (!(val & 0x40) && (val & 7) == ((val >> 3) & 7));
            } else {
                // GUS SDK: DMA Control Register
                //     Channel 1 (bits 2-0)
                //       0=NO DMA
                //       1=DMA1
                //       2=DMA3
                //       3=DMA5
                //       4=DMA6
                //       5=DMA7
                //       6=?
                //       7=?
                //     Channel 2 (bits 5-3)
                //       0=NO DMA
                //       1=DMA1
                //       2=DMA3
                //       3=DMA5
                //       4=DMA6
                //       5=DMA7
                //       6=?
                //       7=?
                //     Combine both DMA channels using channel 1 (bit 6)
                //     Reserved (bit 7)
                //
                //     "If both channels are sharing an DMA, channel 2's DMA must be set to 0 and turn on bit 6. A
                //      bus conflict will occur if both latches are programmed with the same DMA #."

                // NTS: This emulation does not use the second DMA channel... yet... which is why we do not bother
                //      unregistering and reregistering the second DMA channel.

                ///* TODO implement DMA
                // GetDMAChannel(myGUS.dma1)->Register_Callback(0); // FIXME: On DMA conflict this could mean kicking other devices off!

                // NTS: Contrary to an earlier commit, DMA channel 0 is not a valid choice
                if (dmatable[val & 0x7] != 0)
                    myGUS.dma1 = dmatable[val & 0x7];

                if (val & 0x40) // "Combine both DMA channels"
                    myGUS.dma2 = myGUS.dma1;
                else if (dmatable[(val >> 3) & 0x7] != 0)
                    myGUS.dma2 = dmatable[(val >> 3) & 0x7];

                // TODO implement DMA callback
                // GetDMAChannel(myGUS.dma1)->Register_Callback(GUS_DMA_Callback);

                DEBUG_LOG_MSG("GUS DMA reprogrammed: DMA1 %d, DMA2 %d",(int)myGUS.dma1,(int)myGUS.dma2);

                // NTS: The Windows 3.1 Gravis Ultrasound drivers will program the same DMA channel into both without setting the "combining" bit,
                //      even though their own SDK says not to, when Windows starts up. But it then immediately reprograms it normally, so no bus
                //      conflicts actually occur. Strange.
                gus_warn_dma_conflict = (!(val & 0x40) && (val & 7) == ((val >> 3) & 7));
                //*/ // TODO implement DMA
            }
        }
        else {
            LOG_MSG("GUS warning: Port 2XB register control %02xh written (unknown control reg) val=%02xh",(int)myGUS.gRegControl,(int)val);
        }
        break;
    case 0x302:
        myGUS.gCurChannel = val & 31;
        // Versions prior to the Interwave will reflect last I/O to 3X2-3X5 when read back from 3X3
        myGUS.gRegSelectData = (uint8_t)val;

        curchan = guschan[myGUS.gCurChannel];
        break;
    case 0x303:
        myGUS.gRegSelect = myGUS.gRegSelectData = (uint8_t)val;
        myGUS.gRegData = 0;
        break;
    case 0x304:
        // Versions prior to the Interwave will reflect last I/O to 3X2-3X5 when read back from 3X3
        myGUS.gRegSelectData = val;

        myGUS.gRegData = (uint16_t)val;
        break;
    case 0x305:
        // Versions prior to the Interwave will reflect last I/O to 3X2-3X5 when read back from 3X3
        myGUS.gRegSelectData = val;

        myGUS.gRegData = (uint16_t)((0x00ff & myGUS.gRegData) | val << 8);
        ExecuteGlobRegister();
        break;
    case 0x307:
        if ((myGUS.gDramAddr & myGUS.gDramAddrMask) < myGUS.memsize) {
#ifdef PSRAM
            psram_write8(&psram_spi, myGUS.gDramAddr & myGUS.gDramAddrMask, (uint8_t)val);
#else
            GUSRam[myGUS.gDramAddr & myGUS.gDramAddrMask] = (uint8_t)val;
#endif
        }
        break;
    default:
#if LOG_GUS
        LOG_MSG("Write GUS at port 0x%x with %x", port, val);
#endif
        break;
    }
}

//#if 0 // TODO implement DMA
#if 0 // we aren't going to do it this way
static constexpr Bitu GUS_Master_Clock = 617400; /* NOTE: This is 1000000Hz / 1.619695497. Seems to be a common base rate within the hardware. */
static constexpr Bitu GUS_DMA_Event_transfer = 16; /* DMA words (8 or 16-bit) per interval */
static constexpr Bitu GUS_DMA_Events_per_sec = 44100 / 4; /* cheat a little, to improve emulation performance */
static constexpr uint32_t GUS_DMA_Event_interval = 1000000 / GUS_DMA_Events_per_sec;
static constexpr uint32_t GUS_DMA_Event_interval_init = 1000000 / 44100;

void GUS_Update_DMA_Event_transfer() {
    /* NTS: From the GUS SDK, bits 3-4 of DMA Control divide the ISA DMA transfer rate down from "approx 650KHz".
     *      Bits 3-4 are documented as "DMA Rate divisor" */
    GUS_DMA_Event_transfer = GUS_Master_Clock / GUS_DMA_Events_per_sec / (Bitu)(((Bitu)(myGUS.DMAControl >> 3u) & 3u) + 1u);
    GUS_DMA_Event_transfer &= ~1u; /* make sure it's word aligned in case of 16-bit PCM */
    if (GUS_DMA_Event_transfer == 0) GUS_DMA_Event_transfer = 2;
}
#endif
static bool GUS_DMA_Active = false;

void GUS_DMA_Event_Transfer(/*DmaChannel *chan,Bitu dmawords*/) {
    Bitu dmaaddr = (Bitu)(myGUS.dmaAddr) << 4ul; //+ (Bitu)myGUS.dmaAddrOffset;
    // Bitu dmalimit = myGUS.memsize;
    // unsigned int docount = 0;
    unsigned int step = 0;
    // bool dma16xlate;
    // Bitu holdAddr;

    // puts("Starting DMA");

    // FIXME: What does the GUS do if the DMA address goes beyond the end of memory?

    /* is this a DMA transfer where the GUS memory address is to be translated for 16-bit PCM?
     * Note that there is plenty of code out there that transfers 16-bit PCM with 8-bit DMA,
     * and such transfers DO NOT involve the memory address translation.
     *
     * MODE    XLATE?    NOTE
     * 0x00    NO        8-bit PCM, 8-bit DMA (Most DOS programs)
     * 0x04    DEPENDS   8-bit PCM, 16-bit DMA (Star Control II, see comments below)
     * 0x40    NO        16-bit PCM, 8-bit DMA (Windows 3.1, Quake, for 16-bit PCM over 8-bit DMA)
     * 0x44    YES       16-bit PCM, 16-bit DMA (Windows 3.1, Quake, for 16-bit PCM over 16-bit DMA)
     *
     * Mode 0x04 is marked DEPENDS. It DEPENDS on whether the assigned DMA channel is 8-bit (NO) or 16-bit (YES).
     * Mode 0x04 does not appear to be used by any other DOS application or Windows drivers, only by an erratic
     * bug in Star Control II. FIXME: But, what does REAL hardware do? Drag out the old Pentium 100MHz with the
     * GUS classic and test! --J.C.
     *
     * Star Control II has a bug where, if the GUS DMA channel is 8-bit (DMA channel 0-3), it will upload
     * it's samples to GUS RAM, one DMA transfer per sample, and sometimes set bit 2. Setting bit 2 incorrectly
     * tells the GUS it's a 16-bit wide DMA transfer when the DMA channel is 8-bit. But, if the DMA channel is
     * 16-bit (DMA channel 5-7), Star Control II will correctly set bit 2 in all cases and samples will
     * transfer correctly to GUS RAM.
     * */

    /* FIXME: So, if the GUS DMA channel is 8-bit and Star Control II writes mode 0x04 (8-bit PCM, 16-bit DMA), what does the GF1 do?
     *        I'm guessing so far that something happens within the GF1 to transfer as 8-bit anyway, clearly the developers of Star Control II
     *        did not hear any audible sign that an invalid DMA control was being used. Perhaps the hardware engineers of the GF1 figured
     *        out that case and put something in the silicon to ignore the invalid DMA control state. I won't have any answers until
     *        I pull out an old Pentium box with a GUS classic and check. --J.C.
     *
     *        DMA transfers noted by Star Control II that are the reason for this hack (gusdma=1):
     *
     *        LOG:  157098507 DEBUG MISC:GUS DMA: terminal count reached. DMAControl=0x21
     *        LOG:  157098507 DEBUG MISC:GUS DMA transfer 1981 bytes, GUS RAM address 0x0 8-bit DMA 8-bit PCM (ctrl=0x21)
     *        LOG:  157098507 DEBUG MISC:GUS DMA: terminal count reached. DMAControl=0x21
     *        LOG:  157100331 DEBUG MISC:GUS DMA: terminal count reached. DMAControl=0x21
     *        LOG:  157100331 DEBUG MISC:GUS DMA transfer 912 bytes, GUS RAM address 0x7c0 8-bit DMA 8-bit PCM (ctrl=0x21)
     *        LOG:  157100331 DEBUG MISC:GUS DMA: terminal count reached. DMAControl=0x21
     *        LOG:  157100470 DEBUG MISC:GUS DMA: terminal count reached. DMAControl=0x25
     *        LOG:  157100470 DEBUG MISC:GUS DMA transfer 1053 bytes, GUS RAM address 0xb50 16-bit DMA 8-bit PCM (ctrl=0x25)    <-- What?
     *        LOG:  157100470 DEBUG MISC:GUS DMA: terminal count reached. DMAControl=0x25
     *        LOG:  157102251 DEBUG MISC:GUS DMA: terminal count reached. DMAControl=0x21
     *        LOG:  157102251 DEBUG MISC:GUS DMA transfer 1597 bytes, GUS RAM address 0xf80 8-bit DMA 8-bit PCM (ctrl=0x21)
     *        LOG:  157102251 DEBUG MISC:GUS DMA: terminal count reached. DMAControl=0x21
     *        LOG:  157104064 DEBUG MISC:GUS DMA: terminal count reached. DMAControl=0x21
     *        LOG:  157104064 DEBUG MISC:GUS DMA transfer 2413 bytes, GUS RAM address 0x15c0 8-bit DMA 8-bit PCM (ctrl=0x21)
     *        LOG:  157104064 DEBUG MISC:GUS DMA: terminal count reached. DMAControl=0x21
     *
     *        (end list)
     *
     *        Noted: Prior to this hack, the samples played by Star Control II sounded more random and often involved leftover
     *               sample data in GUS RAM, where with this fix, the music now sounds identical to what is played when using
     *               it's Sound Blaster support. */
    // we don't care about this because we are always 8-bit DMA
#if 0
    if (myGUS.dma1 < 4/*8-bit DMA channel*/ && (myGUS.DMAControl & 0x44) == 0x04/*8-bit PCM, 16-bit DMA*/)
        dma16xlate = false; /* Star Control II hack: 8-bit PCM, 8-bit DMA, ignore the bit that says it's 16-bit wide */
    else
        dma16xlate = (myGUS.DMAControl & 0x4) ? true : false;

    if (dma16xlate) {
        // 16-bit wide DMA. The GUS SDK specifically mentions that 16-bit DMA is translated
        // to GUS RAM the same way you translate the play pointer. Eugh. But this allows
        // older demos to work properly even if you set the GUS DMA to a 16-bit channel (5)
        // instead of the usual 8-bit channel (1).
        holdAddr = dmaaddr & 0xc0000L;
        dmaaddr = dmaaddr & 0x1ffffL;
        dmaaddr = dmaaddr << 1;
        dmaaddr = (holdAddr | dmaaddr);
        dmalimit = ((dmaaddr & 0xc0000L) | 0x3FFFFL) + 1;
    }
#endif

#if 0 // ehhhhhhh
    if (dmaaddr < dmalimit)
        docount = (unsigned int)(dmalimit - dmaaddr);

    // docount /= (chan->DMA16+1u);
    if (docount > (chan->currcnt+1u)) docount = chan->currcnt+1u;
    if ((Bitu)docount > dmawords) docount = dmawords;
#endif

    // hack: some programs especially Gravis Ultrasound MIDI playback like to upload by DMA but never clear the DMA TC flag on the DMA controller.
    // TODO let's see how this acts on "real" hardware
    // bool saved_tcount = chan->tcount;
    // chan->tcount = false;

    uint32_t dma_delay;
    // From Interwave programmers guide:
    // 00:0.5 ??s???1.0 ??s
    // 01:6 ??s???7 ??s
    // 10:6 ??s???7 ??s
    // 11:13 ??s???14 ??s
    // From ULTRAWRD: it's 650KHz with a divisor...
    switch ((myGUS.DMAControl >> 3u) & 3u) {
    case 0b00:
        dma_delay = 1;
        break;
    case 0b01:
        dma_delay = 3;
        break;
    case 0b10:
        dma_delay = 6;
        break;
    case 0b11:
        dma_delay = 12;
        break;
    }

    if (true /*docount > 0*/) {
        if ((myGUS.DMAControl & 0x2) == 0) { // transfer direction = write
            // The write to the gus actually happens here!
            //
            step = DMA_Write(
                &dma_config,
                dmaaddr,
                (myGUS.DMAControl & 0x80) /* invert_msb */,
                (myGUS.DMAControl & 0x4) /* is_16bit */,
                dma_delay,
                &GUS_DMA_Active
            );
            uart_print_hex_u32(step);
            // Bitu read=(Bitu)chan->Read((Bitu)docount,&GUSRam[dmaaddr]);
            //Check for 16 or 8bit channel
            // read*=(chan->DMA16+1u);
            /* // this is handled in DMA_Write
            if((myGUS.DMAControl & 0x80) != 0) {
                //Invert the MSB to convert twos compliment form
                Bitu i;
                if((myGUS.DMAControl & 0x40) == 0) {
                    // 8-bit data
                    for(i=dmaaddr;i<(dmaaddr+read);i++) GUSRam[i] ^= 0x80;
                } else {
                    // 16-bit data
                    for(i=dmaaddr+1;i<(dmaaddr+read);i+=2) GUSRam[i] ^= 0x80;
                }
            }
            */
            // step = read;
        } else {
            //Read data out of UltraSound
            // curl up and die, we can't support read
            printf("DMA read isn't supported on PicoGUS... sorry\n");
            assert(false);
            /*
            Bitu wd = (Bitu)chan->Write((Bitu)docount,&GUSRam[dmaaddr]);
            //Check for 16 or 8bit channel
            wd*=(chan->DMA16+1u);

            step = wd;
            */
        }
    }

    //LOG_MSG("GUS DMA transfer %lu bytes, GUS RAM address 0x%lx %u-bit DMA %u-bit PCM (ctrl=0x%02x) tcount=%u",
    //    (unsigned long)step,(unsigned long)dmaaddr,(myGUS.DMAControl & 0x4) ? 16 : 8,(myGUS.DMAControl & 0x40) ? 16 : 8,myGUS.DMAControl,0/*chan->tcount*/);

        /*
    if (step > 0) {
        dmaaddr += (unsigned int)step;

        if (dma16xlate) {
            holdAddr = dmaaddr & 0xc0000L;
            dmaaddr = dmaaddr & 0x3ffffL;
            dmaaddr = dmaaddr >> 1;
            dmaaddr = (holdAddr | dmaaddr);
        }

        myGUS.dmaAddr = dmaaddr >> 4;
        myGUS.dmaAddrOffset = dmaaddr & 0xF;
    }
        */

    // If we're here, tcount has been reached
    //if (chan->tcount) {
        // LOG_MSG("GUS DMA transfer hit Terminal Count, setting DMA TC IRQ pending");
        // printf("step %u\n", step);

        /* Raise the TC irq, and stop DMA */
        myGUS.DMAControl |= 0x100u; /* NTS: DOSBox SVN approach: Use bit 8 for DMA TC IRQ */
        myGUS.IRQStatus |= 0x80;
        // saved_tcount = true;
        if (!(myGUS.DMAControl & 0x20)) {
            puts("DMA end without TC IRQ enabled");
        }
        GUS_CheckIRQ();
        GUS_StopDMA();
    // }

    // chan->tcount = saved_tcount;
}
// #endif // 0 // TODO implement DMA

// #if 0 // TODO implement DMA
uint32_t GUS_DMA_Event(Bitu val) {
    (void)val;//UNUSED
    /* stuff that has no bearing on a real card
    DmaChannel *chan = GetDMAChannel(myGUS.dma1);
    if (chan == NULL) {
        DEBUG_LOG_MSG("GUS DMA event: DMA channel no longer exists, stopping DMA transfer events");
        GUS_DMA_Active = false;
        return;
    }

    if (chan->masked) {
        DEBUG_LOG_MSG("GUS: Stopping DMA transfer interval, DMA masked=%u",chan->masked?1:0);
        GUS_DMA_Active = false;
        return;
    }
    */

    if (!(myGUS.DMAControl & 0x01/*DMA enable*/)) {
        DEBUG_LOG_MSG("GUS DMA event: DMA control 'enable DMA' bit was reset, stopping DMA transfer events");
        GUS_DMA_Active = false;
        return 0;
    }

    /*
    DEBUG_LOG_MSG("GUS DMA event: max %u DMA words. DMA: tc=%u mask=%u cnt=%u",
        (unsigned int)GUS_DMA_Event_transfer,
        chan->tcount?1:0,
        chan->masked?1:0,
        chan->currcnt+1);
    */
    GUS_DMA_Event_Transfer(/*chan,GUS_DMA_Event_transfer*/);

    if (GUS_DMA_Active) {
        puts("HMM NOPE??");
        /* keep going */
        // PIC_AddEvent(GUS_DMA_Event,GUS_DMA_Event_interval, 2);
    }
    return 0;
}
// #endif // 0 // TODO implement DMA

void GUS_StopDMA() {
    ///* TODO implement DMA
    if (GUS_DMA_Active)
        DEBUG_LOG_MSG("GUS: Stopping DMA transfer interval");

    PIC_RemoveEvents(GUS_DMA_Event);
    GUS_DMA_Active = false;
    //*/ // TODO implement DMA
}

__force_inline void GUS_StartDMA() {
    ///* TODO implement DMA
    if (!GUS_DMA_Active) {
        GUS_DMA_Active = true;
        DEBUG_LOG_MSG("GUS: Starting DMA transfer interval");
        // PIC_AddEvent(GUS_DMA_Event, GUS_DMA_Event_interval_init, 2);
        PIC_AddEvent(GUS_DMA_Event, 2, 2);

        /* can't do shit about this
        if (GetDMAChannel(myGUS.dma1)->masked)
            LOG(LOG_MISC,LOG_WARN)("GUS: DMA transfer interval started when channel is masked");

        if (gus_warn_dma_conflict)
            LOG(LOG_MISC,LOG_WARN)(
                "GUS warning: Both DMA channels set to the same channel WITHOUT combining! "
                "This is documented to cause bus conflicts on real hardware");
        */
    }
    //*/ // TODO implement DMA
}

//#if 0 // TODO implement DMA
#if 0 // we aren't going to do it this way
static void GUS_DMA_Callback(/*DmaChannel * chan,*/DMAEvent event) {
    // (void)chan;//UNUSED
    if (event == DMA_UNMASKED) {
        DEBUG_LOG_MSG("GUS: DMA unmasked");
        if (myGUS.DMAControl & 0x01/*DMA enable*/) GUS_StartDMA();
    }
    else if (event == DMA_MASKED) {
        DEBUG_LOG_MSG("GUS: DMA masked. Perhaps it will stop the DMA transfer event.");
    }
}
#endif
//#endif // 0 // TODO implement DMA

int32_t buffer[MIXER_BUFSIZE][2];
extern uint32_t GUS_CallBack(Bitu max_len, int16_t* play_buffer) {
    uint32_t render_samples = buffer_size;
    memset(buffer, 0, render_samples * sizeof(buffer[0]));

    // putchar('g');
    if ((GUS_reset_reg & 0x01/*!master reset*/) == 0x01) {
        for (Bitu i = 0; i < myGUS.ActiveChannels; i++) {
            guschan[i]->generateSamples(buffer[0], render_samples);
        }
    }

    // FIXME: I wonder if the GF1 chip DAC had more than 16 bits precision
    //        to render louder than 100% volume without clipping, and if so,
    //        how many extra bits?
    //
    //        If not, then perhaps clipping and saturation were not a problem
    //        unless the volume was set to maximum?
    //
    //        Time to pull out the GUS MAX and test this theory: what happens
    //        if you play samples that would saturate at maximum volume at 16-bit
    //        precision? Does it audibly clip or is there some headroom like some
    //        sort of 17-bit DAC?
    //
    //        One way to test is to play a sample on one channel while another
    //        channel is set to play a single sample at maximum volume (to see
    //        if it makes the audio grungy like a waveform railed to one side).
    //
    //        Past experience with GUS cards says that at full volume their line
    //        out jacks can be quite loud when connected to a speaker.
    //
    //        While improving this code, a better audio compression function
    //        could be implemented that does proper envelope tracking and volume
    //        control for better results than this.
    //
    //        --J.C.

    Bitu play_i = 0;
    for (Bitu i = 0; i < render_samples; i++) {
        buffer[i][0] >>= (VOL_SHIFT * AutoAmp) >> 9;
        buffer[i][1] >>= (VOL_SHIFT * AutoAmp) >> 9;
        bool dampenedAutoAmp = false;

        if (buffer[i][0] > 32767) {
            buffer[i][0] = 32767;
            if (enable_autoamp) {
                AutoAmp -= 4; /* dampen faster than recovery */
                dampenedAutoAmp = true;
            }
        }
        else if (buffer[i][0] < -32768) {
            buffer[i][0] = -32768;
            if (enable_autoamp) {
                AutoAmp -= 4; /* dampen faster than recovery */
                dampenedAutoAmp = true;
            }
        }

        if (buffer[i][1] > 32767) {
            buffer[i][1] = 32767;
            if (enable_autoamp && !dampenedAutoAmp) {
                AutoAmp -= 4; /* dampen faster than recovery */
                dampenedAutoAmp = true;
            }
        }
        else if (buffer[i][1] < -32768) {
            buffer[i][1] = -32768;
            if (enable_autoamp && !dampenedAutoAmp) {
                AutoAmp -= 4; /* dampen faster than recovery */
                dampenedAutoAmp = true;
            }
        }

        if (AutoAmp < myGUS.masterVolumeMul && !dampenedAutoAmp) {
            AutoAmp++; /* recovery back to 100% normal volume */
        }
        play_buffer[play_i++] = buffer[i][0];
        play_buffer[play_i++] = buffer[i][1];
    }

    // TODO copy samples to play buffer
    // gus_chan->AddSamples_s32(len, buffer[0]);
    CheckVoiceIrq();
    // putchar('o');
    return render_samples;
}

// Generate logarithmic to linear volume conversion tables
static void MakeTables(void) {
    int i;
    double out = (double)(1 << 13);
    for (i=4095;i>=0;i--) {
        vol16bit[i]=(uint16_t)((int16_t)out);
        out/=1.002709201;       /* 0.0235 dB Steps */
        //Original amplification routine in the hardware
        //vol16bit[i] = ((256 + i & 0xff) << VOL_SHIFT) / (1 << (24 - (i >> 8)));
    }
    /* FIX: DOSBox 0.74 had code here that produced a pantable which
     *      had nothing to do with actual panning control variables.
     *      Instead it seemed to generate a 16-element map that started
     *      at 0, jumped sharply to unity and decayed to 0.
     *      The unfortunate result was that stock builds of DOSBox
     *      effectively locked Gravis Ultrasound capable programs
     *      to monural audio.
     *
     *      This fix generates the table properly so that they correspond
     *      to how much we attenuate the LEFT channel for any given
     *      4-bit value of the Panning register (you attenuate the
     *      RIGHT channel by looking at element 0xF - (val&0xF)).
     *
     *      Having made this fix I can finally enjoy old DOS demos
     *      in GUS stereo instead of having everything mushed into
     *      mono. */
    if (gus_fixed_table) {
        for (i=0;i < 16;i++)
            pantable[i] = pantablePDF[i] * 2048u;

        LOG_MSG("GUS: using accurate (fixed) pantable");
    }
    else {
        for (i=0;i < 8;i++)
            pantable[i] = 0;
        for (i=8;i < 15;i++)
            pantable[i]=(uint32_t)(-128.0*(log((double)(15-i)/7.0)/log(2.0))*(double)(1 << RAMP_FRACT));

        /* if the program cranks the pan register all the way, ensure the
         * opposite channel is crushed to silence */
        pantable[15] = 1UL << 30UL;

        LOG_MSG("GUS: using old (naive) pantable");
    }

    LOG_MSG("GUS pantable (attenuation, left to right in dB): hard left -%.3f, -%.3f, -%.3f, -%.3f, -%.3f, -%.3f, -%.3f, center(7) -%.3f, center(8) -%.3f, -%.3f, -%.3f, -%.3f, -%.3f, -%.3f, -%.3f, hard right -%.3f",
        ((double)pantable[0]) / (1 << RAMP_FRACT),
        ((double)pantable[1]) / (1 << RAMP_FRACT),
        ((double)pantable[2]) / (1 << RAMP_FRACT),
        ((double)pantable[3]) / (1 << RAMP_FRACT),
        ((double)pantable[4]) / (1 << RAMP_FRACT),
        ((double)pantable[5]) / (1 << RAMP_FRACT),
        ((double)pantable[6]) / (1 << RAMP_FRACT),
        ((double)pantable[7]) / (1 << RAMP_FRACT),
        ((double)pantable[8]) / (1 << RAMP_FRACT),
        ((double)pantable[9]) / (1 << RAMP_FRACT),
        ((double)pantable[10]) / (1 << RAMP_FRACT),
        ((double)pantable[11]) / (1 << RAMP_FRACT),
        ((double)pantable[12]) / (1 << RAMP_FRACT),
        ((double)pantable[13]) / (1 << RAMP_FRACT),
        ((double)pantable[14]) / (1 << RAMP_FRACT),
        ((double)pantable[15]) / (1 << RAMP_FRACT));
}

class GUS/*:public Module_base*/{
private:
    bool gus_enable;
public:
    GUS(Bitu base_port) {
        int x;

        gus_enable = true;
        memset(&myGUS,0,sizeof(myGUS));
#ifndef PSRAM
        memset(GUSRam,0,GUS_RAM_SIZE);
#endif

        gus_fixed_table = true;

        myGUS.clearTCIfPollingIRQStatus = false;

        myGUS.gUltraMAXControl = 0;
        myGUS.lastIRQStatusPollRapidCount = 0;
        myGUS.lastIRQStatusPollAt = 0;

        myGUS.initUnmaskDMA = false;

        myGUS.force_master_irq_enable = false;

        myGUS.rate=44100;
        myGUS.memsize = GUS_RAM_SIZE;

        LOG_MSG("GUS emulation: %uKB onboard",myGUS.memsize>>10);

        // some demoscene stuff has music that's way too loud if we render at full volume.
        // the GUS mixer emulation won't fix it because it changes the volume at the Mixer
        // level AFTER the code has rendered and clipped samples to 16-bit range.
        myGUS.masterVolume = 0.00;
        myGUS.updateMasterVolume();

        myGUS.portbase = base_port - 0x200u;

       // DmaChannels[myGUS.dma1]->Register_TC_Callback(GUS_DMA_TC_Callback);
    
        MakeTables();
    
        for (uint8_t chan_ct=0; chan_ct<32; chan_ct++) {
            guschan[chan_ct] = new GUSChannels(chan_ct);
        }

        // FIXME: Could we leave the card in reset state until a fake ULTRINIT runs?
        myGUS.gRegData=0x000/*reset*/;
        GUSReset();

        /* TODO support DMA
        if (myGUS.initUnmaskDMA)
            GetDMAChannel(myGUS.dma1)->SetMask(false);
        */
        /* TODO support DMA
        GetDMAChannel(myGUS.dma1)->Register_Callback(GUS_DMA_Callback);
        */

        // Default to GUS MAX 1MB maximum
        myGUS.gDramAddrMask = 0xFFFFFu;

        // if instructed, configure the card as if ULTRINIT had been run
        if (startup_ultrinit) {
            myGUS.gRegData=0x700;
            GUSReset();

            myGUS.gRegData=0x700;
            GUSReset();
        }
    }

    ~GUS() {
        myGUS.gRegData=0x1;
        GUSReset();
        myGUS.gRegData=0x0;
    
        for(Bitu i=0;i<32;i++) {
            delete guschan[i];
        }

        memset(&myGUS,0,sizeof(myGUS));
#ifndef PSRAM
        memset(GUSRam,0,1024*1024);
#endif
    }
};

static GUS* test = NULL;
void GUS_OnReset(Bitu base_port) {
    LOG_MSG("Allocating GUS emulation");
    test = new GUS(base_port);

    critical_section_init(&gus_crit);
}
