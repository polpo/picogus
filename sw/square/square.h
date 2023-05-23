//=========================================================
//  square.h
//
//  Misc square wave audio device emulation
//  (c) Aaron Giles
//=========================================================
//
//  This code is derived from the DREAMM source code, and is released under
//  the BSD 3-clause license.
//
//  Copyright (c) 2023, Aaron Giles
//
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted provided that the following conditions are met:
//
//  1. Redistributions of source code must retain the above copyright notice, this
//     list of conditions and the following disclaimer.
//
//  2. Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//  3. Neither the name of the copyright holder nor the names of its
//     contributors may be used to endorse or promote products derived from
//     this software without specific prior written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
//  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
//  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
//  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
//  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
//  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
//  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
//  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
//  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
//  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//=========================================================

#pragma once

#include <stdint.h>

#ifndef ASG_AUDIO_SQUARE_H
#define ASG_AUDIO_SQUARE_H

static constexpr uint32_t OUTPUT_FREQUENCY = 44100;


//===========================================================================
//
// speaker_generator_t
//
// This class implements a simple PC speaker emulation.
//
//===========================================================================

class speaker_generator_t
{
    static constexpr uint32_t PIT_FREQUENCY = 14318180 / 12;

    //
    // square wave stepping constants
    //
    static constexpr uint8_t FRAC_BITS = 31;
    static constexpr uint32_t FRAC_ONE = 1 << FRAC_BITS;
    static constexpr uint32_t FRAC_HALF = FRAC_ONE >> 1;

    //
    // volume constants
    //
    static constexpr float MAX_VOLUME = 0.25f;

public:
    //
    // construction/destruction
    //
    speaker_generator_t();

    //
    // event processing
    //
    void process_event(uint16_t divisor, bool enable);

    //
    // output
    //
    void generate_frames(float *dest, uint32_t frames, float gain = 1.0f);

private:
    //
    // internal state
    //
    uint32_t m_step;
    uint32_t m_pos;
    uint8_t m_enable;
};



//===========================================================================
//
// speaker_t
//
// This class manages speaker sound emulation.
//
//===========================================================================

class speaker_t
{
public:
    //
    // construction/destruction
    //
    speaker_t();

    //
    // state changes
    //
    void set_rate(uint32_t rate);
    void set_control(uint8_t control);

    //
    // direct generator access
    //
    speaker_generator_t &generator() { return m_generator; }

private:
    //
    // internal state
    //
    speaker_generator_t m_generator;
    uint32_t m_rate;
    uint8_t m_control;
};



//===========================================================================
//
// tandy_generator_t
//
// This class implements Tandy (NCR8496) sound chip emulation.
//
//===========================================================================

class tandy_generator_t
{
    //
    // square wave stepping constants
    //
    static constexpr uint8_t FRAC_BITS = 28;
    static constexpr uint32_t FRAC_ONE = 1 << FRAC_BITS;
    static constexpr uint32_t FRAC_HALF = FRAC_ONE >> 1;

    //
    // chip-level constants
    //
    static constexpr uint32_t INTERNAL_CLOCK = 3579545 / 32;
    static constexpr uint32_t PRNG_INITIAL = 0x4000;

    //
    // voice-level state
    //
    struct voice_t
    {
        int16_t volume = 0;
        uint16_t rawfreq = 0;
        uint32_t step = 0;
        uint32_t pos = 0;
    };

public:
    //
    // construction/destruction
    //
    tandy_generator_t();

    //
    // event processing
    //
    void process_event(uint8_t data);

    //
    // output
    //
#ifdef SQUARE_FLOAT_OUTPUT
    void generate_frames(float *dest, uint32_t frames, float gain = 1.0f);
#else
    void generate_frames(int32_t *dest, uint32_t frames);
#endif

private:
    //
    // internal helpers
    //
    uint32_t step_from_divisor(uint16_t divisor) const;

    //
    // internal state
    //
    uint8_t m_last_freq_chan;
    uint8_t m_noise_control;
    uint16_t m_prng;
    voice_t m_voice[4];
};



//===========================================================================
//
// tandysound_t
//
// This class manages Tandy/PCjr (SN76486) sound emulation.
//
//===========================================================================

class tandysound_t
{
public:
    //
    // construction/destruction
    //
    tandysound_t();

    //
    // direct generator access
    //
    tandy_generator_t &generator() { return m_generator; }

    //
    // SN76486 I/O handlers
    //
    void write_register(uint32_t address, uint8_t data); // I/O port 0xc0

private:
    //
    // internal state
    //
    tandy_generator_t m_generator;
};



//===========================================================================
//
// saa1099_generator_t
//
// This class implements a SAA1099 sound chip emulation. Two of
// these are present on the CMS/GameBlaster.
//
//===========================================================================

class saa1099_generator_t
{
    //
    // square wave stepping constants
    //
    static constexpr uint8_t FRAC_BITS = 24;
    static constexpr uint32_t FRAC_ONE = 1 << FRAC_BITS;
    static constexpr uint32_t FRAC_HALF = FRAC_ONE >> 1;

    //
    // chip-level constants
    //
    static constexpr uint32_t INTERNAL_CLOCK = 14318181 / 2;
    static constexpr uint32_t PRNG_INITIAL = 0xfffff;

    //
    // voice-level state
    //
    struct voice_t
    {
        int16_t lvolume = 0;
        int16_t rvolume = 0;
        uint8_t octave = 0;
        uint8_t frequency = 0;
        uint8_t enable = 0;
        uint8_t noise = 0;
        uint32_t step = 0;
        uint32_t pos = 0;
    };

    //
    // noise generator state
    //
    struct noise_t
    {
        uint8_t frequency = 0;
        uint32_t step = 0;
        uint32_t pos = 0;
        uint32_t prng = PRNG_INITIAL;
    };

    //
    // envelope state
    //
    struct envelope_t
    {
        uint8_t type = 0;
        int8_t hold = -1;
        uint32_t pos = 0;
    };

public:
    //
    // construction/destruction
    //
    saa1099_generator_t();

    //
    // event generation
    //
    void process_event(uint8_t regnum, uint8_t data);

    //
    // output
    //
#ifdef SQUARE_FLOAT_OUTPUT
    void generate_frames(float *dest, uint32_t frames, float gain = 1.0f);
#else
    void generate_frames(int32_t *dest, uint32_t frames);
#endif

private:
    //
    // internal helpers
    //
    uint32_t step_from_divisor(voice_t &voice);
    uint32_t noise_step(noise_t &noise, int gen);
#ifdef SQUARE_FLOAT_OUTPUT
    template<int _Voicenum> void add_voice(float &lresult, float &rresult, float lvolume, float rvolume);
#else
    template<int _Voicenum> void add_voice(int32_t &lresult, int32_t &rresult, int16_t lvolume, int16_t rvolume);
#endif

    //
    // internal state
    //
    bool m_enable;
    voice_t m_voice[8];
    noise_t m_noise[2];
    envelope_t m_envelope[2];
};



//===========================================================================
//
// cms_t
//
// This class manages Creative Music System (CMS) emulation.
//
//===========================================================================

class cms_t
{
public:
    //
    // construction/destruction
    //
    cms_t();

    //
    // direct generator access
    //
    saa1099_generator_t &generator(int index) { return m_generator[index]; }

    //
    // CMS I/O handlers
    //
    uint8_t read_unimp(uint32_t address);             // I/O ports 0x220-229, 22b-22f
    void write_unimp(uint32_t address, uint8_t data); // I/O ports 0x224-22f
    void write_data(uint32_t address, uint8_t data);  // I/O ports 0x220/222
    void write_addr(uint32_t address, uint8_t data);  // I/O ports 0x221/223
    uint8_t read_detect(uint32_t address);            // I/O port  0x22a

private:
    //
    // internal state
    //
    uint8_t m_register[16];
    saa1099_generator_t m_generator[2];
};

#endif
