//=========================================================
//  square.cpp
//
//  Misc square wave audio device emulation
//  (c) Aaron Giles
//
//  Modified by Ian Scott for PicoGUS: added int32_t output
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

#include "square.h"


//===========================================================================
//
// speaker_generator_t
//
// This class implements a simple PC speaker emulation.
//
//===========================================================================

//
// constructor
//
speaker_generator_t::speaker_generator_t() :
    m_step(0),
    m_pos(0),
    m_enable(0)
{
}

//
// update state based on an incoming event
//
void speaker_generator_t::process_event(uint16_t divisor, bool enable)
{
    // extract enable from raw data
    m_enable = enable;

    // extract the PIT divisor and recompute the step value
    if (divisor == 0)
        m_step = 0, m_pos = FRAC_HALF;
    else
        m_step = uint32_t((uint64_t(PIT_FREQUENCY) << FRAC_BITS) / (OUTPUT_FREQUENCY * divisor));
}

//
// generate the requested number of audio frames
//
void speaker_generator_t::generate_frames(float *dest, uint32_t frames, float gain)
{
    // ignore if we're not enabled
    if (!m_enable)
        return;

    // generate a square wave
    float value = MAX_VOLUME * gain;
    for (uint32_t frame = 0; frame < frames; frame++, dest += 2)
    {
        m_pos += m_step;
        if ((m_pos & FRAC_HALF) != 0)
        {
            dest[0] += value;
            dest[1] += value;
        }
    }
}



//===========================================================================
//
// speaker_t
//
// This class manages speaker sound emulation.
//
//===========================================================================

//
// constructor
//
speaker_t::speaker_t() :
    m_rate(0),
    m_control(0)
{
}

//
// set the external square wave generator rate
//
void speaker_t::set_rate(uint32_t rate)
{
    if (m_rate != rate)
    {
        m_rate = rate;
        m_generator.process_event(((m_control & 1) != 0) ? m_rate : 0, (m_control & 2) != 0);
    }
}

//
// set the control bits
//
void speaker_t::set_control(uint8_t data)
{
    uint8_t diff = data ^ m_control;
    if ((diff & 3) != 0)
    {
        m_control = data;
        m_generator.process_event(((m_control & 1) != 0) ? m_rate : 0, (m_control & 2) != 0);
    }
}



//===========================================================================
//
// tandy_generator_t
//
// This class implements Tandy (NCR8496) sound chip emulation.
//
//===========================================================================

//
// constructor
//
tandy_generator_t::tandy_generator_t() :
    m_last_freq_chan(0),
    m_noise_control(0),
    m_prng(PRNG_INITIAL)
{
}

//
// update state based on an incoming event
//
void tandy_generator_t::process_event(uint8_t data)
{
    int16_t const s_volume_table[16] = { 8191, 6506, 5168, 4105, 3261, 2590, 2057, 1634, 1298, 1031, 819, 651, 517, 411, 326, 0 };

    // get the raw register write
    uint8_t reg = (data >> 4) & 7;
    if ((data & 0x80) == 0)
    {
        if (m_last_freq_chan > 2)
            return;
        reg = m_last_freq_chan * 2;
    }

    // find the attached voice
    uint8_t chan = (reg >> 1) & 3;
    auto &voice = m_voice[chan];

    // update based on the register written
    switch (reg)
    {
        // tone frequency registers
        case 0:
        case 2:
        case 4:
            // remember the last frequency channel
            m_last_freq_chan = chan;

            // write to high or low depending on top bit
            if ((data & 0x80) != 0)
                voice.rawfreq = (voice.rawfreq & 0x3f0) | ((data << 0) & 0x00f);
            else
                voice.rawfreq = (voice.rawfreq & 0x00f) | ((data << 4) & 0x3f0);

            // recompute the voice step
            voice.step = this->step_from_divisor(voice.rawfreq);

            // if the noise is connected to channel 2, update it as well
            if (chan == 2 && (m_noise_control & 3) == 3)
                m_voice[3].step = voice.step;
            break;

        // volume registers
        case 1:
        case 3:
        case 5:
        case 7:
            voice.volume = s_volume_table[data & 0x0f];
            break;

        // noise control
        case 6:
            // mark the last frequency channel invalid
            m_last_freq_chan = 3;

            // PRNG is reset if state of bit 2 changes
            // note that some SN76496 implementation reset on every write to this
            // register, but empirically that ruins the drums in Maniac Mansion
            if ((data ^ m_noise_control) & 0x04)
                m_prng = PRNG_INITIAL;

            // update the register
            m_noise_control = data & 0x07;

            // if set to track voice 2, copy from there; otherwise, compute the
            // fixed frequency from the noise control bits
            if ((m_noise_control & 3) == 3)
                voice.step = m_voice[2].step;
            else
                voice.step = this->step_from_divisor(16 << (m_noise_control & 3));
            break;
    }
}

//
// generate the requested number of audio frames
//
#ifdef SQUARE_FLOAT_OUTPUT
void tandy_generator_t::generate_frames(float *dest, uint32_t frames, float gain)
#else
void tandy_generator_t::generate_frames(int32_t *dest, uint32_t frames)
#endif
{
    // generate square wavs
#ifdef SQUARE_FLOAT_OUTPUT
    float vvolume[4];
    vvolume[0] = float(m_voice[0].volume) * gain * (1.0f / 32768.0f);
    vvolume[1] = float(m_voice[1].volume) * gain * (1.0f / 32768.0f);
    vvolume[2] = float(m_voice[2].volume) * gain * (1.0f / 32768.0f);
    vvolume[3] = float(m_voice[3].volume) * gain * (1.0f / 32768.0f);
#endif
    for (uint32_t frame = 0; frame < frames; frame++)
    {
#ifdef SQUARE_FLOAT_OUTPUT
        float result = 0;
#else
        int32_t result = 0;
#endif

        // channel 0
        m_voice[0].pos += m_voice[0].step;
        if ((m_voice[0].pos & FRAC_HALF) != 0)
#ifdef SQUARE_FLOAT_OUTPUT
            result += vvolume[0];
#else
            result += m_voice[0].volume;
#endif

        // channel 1
        m_voice[1].pos += m_voice[1].step;
        if ((m_voice[1].pos & FRAC_HALF) != 0)
#ifdef SQUARE_FLOAT_OUTPUT
            result += vvolume[1];
#else
            result += m_voice[1].volume;
#endif

        // channel 2
        m_voice[2].pos += m_voice[2].step;
        if ((m_voice[2].pos & FRAC_HALF) != 0)
#ifdef SQUARE_FLOAT_OUTPUT
            result += vvolume[2];
#else
            result += m_voice[2].volume;
#endif

        // noise channel: on rising edge, clock the PRNG
        m_voice[3].pos += m_voice[3].step;
        for ( ; m_voice[3].pos >= FRAC_ONE; m_voice[3].pos -= FRAC_ONE)
        {
            // mode 0 just feeds back low bit into high bit
            if ((m_noise_control & 4) == 0)
                m_prng = (m_prng >> 1) | ((m_prng & 1) << 14);

            // mode 1 is a proper PRNG; feedback from output + bit 4 into bit 14
            else
                m_prng = (m_prng >> 1) | (((m_prng ^ (~m_prng >> 4)) & 1) << 14);
        }

        // PRNG output bit controls the noise contribution
        if ((m_prng & 1) != 0)
#ifdef SQUARE_FLOAT_OUTPUT
            result += vvolume[3];
#else
            result += m_voice[3].volume;
#endif

        // output stereo; note that output is inverted
        *dest++ -= result;
        *dest++ -= result;
    }
}

//
// helper to compute the output sample step from a frequency divisor
//
uint32_t tandy_generator_t::step_from_divisor(uint16_t divisor) const
{
    return uint32_t((uint64_t(INTERNAL_CLOCK) << FRAC_BITS) / (OUTPUT_FREQUENCY * ((divisor != 0) ? divisor : 0x400)));
}



//===========================================================================
//
// tandysound_t
//
// This class manages Tandy sound emulation.
//
//===========================================================================

//
// constructor
//
tandysound_t::tandysound_t()
{
}

//
// write to the data register
//
void tandysound_t::write_register(uint32_t address, uint8_t data)
{
    m_generator.process_event(data);
}



//===========================================================================
//
// saa1099_generator_t
//
// This class implements a SAA1099 sound chip emulation. Two of
// these are present on the CMS/GameBlaster.
//
//===========================================================================

//
// constructor
//
saa1099_generator_t::saa1099_generator_t() :
    m_enable(false)
{
}

//
// update state based on an incoming event
//
void saa1099_generator_t::process_event(uint8_t reg, uint8_t data)
{
    int16_t const s_volume_table[16] = { 0, 0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700, 0x800, 0x900, 0xa00, 0xb00, 0xc00, 0xd00, 0xe00, 0xf00 };

    // get the raw register write
    uint8_t chan, type;

    switch (reg)
    {
        // left/right output volumes for each voice
        case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05:
            chan = reg & 7;
            m_voice[chan].lvolume = s_volume_table[data & 0x0f];
            m_voice[chan].rvolume = s_volume_table[(data >> 4) & 0x0f];
            break;

        // frequency control for each voice
        case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d:
            chan = reg & 7;
            m_voice[chan].frequency = data;
            m_voice[chan].step = this->step_from_divisor(m_voice[chan]);

            // if the noise channels are driven by our frequency, update them as well
            if (chan == 0 && m_noise[0].frequency == 3)
                m_noise[0].step = this->noise_step(m_noise[0], 0);
            if (chan == 3 && m_noise[1].frequency == 3)
                m_noise[1].step = this->noise_step(m_noise[1], 1);
            break;

        // octave control for each voice, packed two voices to a byte
        case 0x10: case 0x11: case 0x12:
            chan = 2 * (reg & 3);
            m_voice[chan].octave = data & 0x0f;
            m_voice[chan].step = this->step_from_divisor(m_voice[chan]);
            if (chan == 0 && m_noise[0].frequency == 3)
                m_noise[0].step = this->noise_step(m_noise[0], 0);
            chan++;
            m_voice[chan].octave = (data >> 4) & 0x0f;
            m_voice[chan].step = this->step_from_divisor(m_voice[chan]);
            if (chan == 3 && m_noise[1].frequency == 3)
                m_noise[1].step = this->noise_step(m_noise[1], 1);
            break;

        // bitmask of tone enables for each voice
        case 0x14:
            m_voice[0].enable = (data >> 0) & 1;
            m_voice[1].enable = (data >> 1) & 1;
            m_voice[2].enable = (data >> 2) & 1;
            m_voice[3].enable = (data >> 3) & 1;
            m_voice[4].enable = (data >> 4) & 1;
            m_voice[5].enable = (data >> 5) & 1;
            break;

        // bitmask of noise enables for each voice
        case 0x15:
            m_voice[0].noise = (data >> 0) & 1;
            m_voice[1].noise = (data >> 1) & 1;
            m_voice[2].noise = (data >> 2) & 1;
            m_voice[3].noise = (data >> 3) & 1;
            m_voice[4].noise = (data >> 4) & 1;
            m_voice[5].noise = (data >> 5) & 1;
            break;

        // noise control register
        case 0x16:
            m_noise[0].frequency = data & 3;
            m_noise[0].step = this->noise_step(m_noise[0], 0);
            m_noise[1].frequency = (data >> 4) & 3;
            m_noise[1].step = this->noise_step(m_noise[1], 1);
            break;

        // envelope control registers
        case 0x18:
        case 0x19:
            chan = reg & 1;
            m_envelope[chan].type = data;
            type = (data >> 1) & 7;
            m_envelope[chan].hold = (type == 0) ? 0 : (type == 1) ? 15 : -1;
            m_envelope[chan].pos = 0;
            break;

        // reset/enable register
        case 0x1c:
            m_enable = data & 1;
            if (data & 2)
            {
                m_voice[0].pos = 0;
                m_voice[1].pos = 0;
                m_voice[2].pos = 0;
                m_voice[3].pos = 0;
                m_voice[4].pos = 0;
                m_voice[5].pos = 0;
                m_noise[0].pos = 0;
                m_noise[1].pos = 0;
            }
            break;
    }
}

//
// helper to add the output of a single voice to the results; templated so
// that codegen can be optimized for different voices' behaviors
//
template<int _Voicenum>
#ifdef SQUARE_FLOAT_OUTPUT
void saa1099_generator_t::add_voice(float &lresult, float &rresult, float lvolume, float rvolume)
#else
void saa1099_generator_t::add_voice(int32_t &lresult, int32_t &rresult, int16_t lvolume, int16_t rvolume)
#endif
{
    auto &voice = m_voice[_Voicenum];

    // handle envelopes
    int outlevel = 0;

    // non-noise case: level is high if tone is enabled and the fractional bit is high
    if (!voice.noise)
        outlevel = voice.enable & (voice.pos >> (FRAC_BITS - 1));

    // noise-only case -- just use the noise source
    else if (!voice.enable)
        outlevel = m_noise[_Voicenum/3].prng & 1;

    // combined case: only outputs if tone is high, and double if both are high
    else
        outlevel = ((voice.pos >> (FRAC_BITS - 1)) & 1) << (m_noise[_Voicenum/3].prng & 1);

    // if the output level is 0, doesn't matter
    if (outlevel-- == 0)
        return;

    // apply outlevel to the volume
#ifdef SQUARE_FLOAT_OUTPUT
    float outscale = float(1 << outlevel);
    lvolume *= outscale;
    rvolume *= outscale;
#else
    lvolume << outlevel;
    rvolume << outlevel;
#endif

    // non-envelope case
    if (_Voicenum % 3 != 2 || (m_envelope[_Voicenum / 3].type & 0x80) == 0)
    {
        lresult += lvolume;
        rresult += rvolume;
    }

    // envelope case
    else
    {
        auto &env = m_envelope[_Voicenum / 3];
        int8_t factor = env.hold;

        // if envelope is still going, get the value
        if (factor < 0)
        {
            // bit 4 is number of bits for envelope control (3 vs 4)
            uint32_t pos = (env.pos >> FRAC_BITS) - ((env.type >> 4) & 1);

            // bits 1-3 are the type:
            //   0: hold 0
            //   1: hold 15
            //   2: decay 15->0 then hold 0
            //   3: decay 15->0 repeatedly
            //   4: triangle 0->15->0 then hold 0
            //   5: triangle 0->15->0 repeatedly
            //   6: attack 0->15 then hold 0
            //   7: attack 0->15 repeatedly
            uint8_t type = (env.type >> 1) & 7;

            // if past the hold time, clamp to 0 for the even-numbered cases
            if (pos >= 32 && (type & 1) == 0)
                env.hold = factor = 0;

            // otherwise, process
            else
            {
                static uint8_t const s_env_shapes[8][32] =
                {
                    {  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                    { 15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15, 15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15 },
                    { 15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                    { 15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 },
                    {  0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15, 15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 },
                    {  0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15, 15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 },
                    {  0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                    {  0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,  0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15 },
                };
                factor = s_env_shapes[type][pos & 31];
            }
        }

        // apply to left
#ifdef SQUARE_FLOAT_OUTPUT
        float ffactor = float(factor) * (1.0f / 16.0f);
        lresult += ffactor * lvolume;
#else
        lresult += (int32_t)lvolume * factor / 16;
#endif

        // bit 0 means right is inverted
        if ((env.type & 0x01) != 0)
            factor ^= 15;
#ifdef SQUARE_FLOAT_OUTPUT
        rresult += ffactor * rvolume;
#else
        rresult += (int32_t)rvolume * factor / 16;
#endif
    }
}

//
// generate the requested number of audio frames
//
#ifdef SQUARE_FLOAT_OUTPUT
void saa1099_generator_t::generate_frames(float *dest, uint32_t frames, float gain)
#else
void saa1099_generator_t::generate_frames(int32_t *dest, uint32_t frames)
#endif
{
    // if not enabled, nothing to do
    if (!m_enable)
        return;

    // precompute volumes
    float vvolume[6][2];
#ifdef SQUARE_FLOAT_OUTPUT
    float vvolume[6][2];
    vvolume[0][0] = float(m_voice[0].lvolume) * gain * (1.0f / 32768.0f);
    vvolume[0][1] = float(m_voice[0].rvolume) * gain * (1.0f / 32768.0f);
    vvolume[1][0] = float(m_voice[1].lvolume) * gain * (1.0f / 32768.0f);
    vvolume[1][1] = float(m_voice[1].rvolume) * gain * (1.0f / 32768.0f);
    vvolume[2][0] = float(m_voice[2].lvolume) * gain * (1.0f / 32768.0f);
    vvolume[2][1] = float(m_voice[2].rvolume) * gain * (1.0f / 32768.0f);
    vvolume[3][0] = float(m_voice[3].lvolume) * gain * (1.0f / 32768.0f);
    vvolume[3][1] = float(m_voice[3].rvolume) * gain * (1.0f / 32768.0f);
    vvolume[4][0] = float(m_voice[4].lvolume) * gain * (1.0f / 32768.0f);
    vvolume[4][1] = float(m_voice[4].rvolume) * gain * (1.0f / 32768.0f);
    vvolume[5][0] = float(m_voice[5].lvolume) * gain * (1.0f / 32768.0f);
    vvolume[5][1] = float(m_voice[5].rvolume) * gain * (1.0f / 32768.0f);
#endif

    // generate square wavs
    bool env0_clock = ((m_envelope[0].type & 0xa0) == 0x80);
    bool env1_clock = ((m_envelope[1].type & 0xa0) == 0x80);
    for (uint32_t frame = 0; frame < frames; frame++)
    {
#ifdef SQUARE_FLOAT_OUTPUT
        float lresult = 0;
        float rresult = 0;
#else
        int32_t lresult = 0;
        int32_t rresult = 0;
#endif

        // channel 0
        m_voice[0].pos += m_voice[0].step;
#ifdef SQUARE_FLOAT_OUTPUT
        this->add_voice<0>(lresult, rresult, vvolume[0][0], vvolume[0][1]);
#else
        this->add_voice<0>(lresult, rresult, m_voice[0].lvolume, m_voice[0].rvolume);
#endif

        // channel 1
        m_voice[1].pos += m_voice[1].step;
#ifdef SQUARE_FLOAT_OUTPUT
        this->add_voice<1>(lresult, rresult, vvolume[1][0], vvolume[1][1]);
#else
        this->add_voice<1>(lresult, rresult, m_voice[1].lvolume, m_voice[1].rvolume);
#endif

        // step envelope 0 if enabled
        if (env0_clock)
            m_envelope[0].pos += m_voice[1].step;

        // channel 2
        m_voice[2].pos += m_voice[2].step;
#ifdef SQUARE_FLOAT_OUTPUT
        this->add_voice<2>(lresult, rresult, vvolume[2][0], vvolume[2][1]);
#else
        this->add_voice<2>(lresult, rresult, m_voice[2].lvolume, m_voice[2].rvolume);
#endif

        // channel 3
        m_voice[3].pos += m_voice[3].step;
#ifdef SQUARE_FLOAT_OUTPUT
        this->add_voice<3>(lresult, rresult, vvolume[3][0], vvolume[3][1]);
#else
        this->add_voice<3>(lresult, rresult, m_voice[3].lvolume, m_voice[3].rvolume);
#endif

        // channel 4
        m_voice[4].pos += m_voice[4].step;
#ifdef SQUARE_FLOAT_OUTPUT
        this->add_voice<4>(lresult, rresult, vvolume[4][0], vvolume[4][1]);
#else
        this->add_voice<4>(lresult, rresult, m_voice[4].lvolume, m_voice[4].rvolume);
#endif

        // step envelope 1 if enabled
        if (env1_clock)
            m_envelope[1].pos += m_voice[4].step;

        // channel 5
        m_voice[5].pos += m_voice[5].step;
#ifdef SQUARE_FLOAT_OUTPUT
        this->add_voice<5>(lresult, rresult, vvolume[5][0], vvolume[5][1]);
#else
        this->add_voice<5>(lresult, rresult, m_voice[5].lvolume, m_voice[5].rvolume);
#endif

        // output stereo
        *dest++ += lresult;
        *dest++ += rresult;

        // noise generator 0
        m_noise[0].pos += m_noise[0].step;
        for ( ; m_noise[0].pos >= FRAC_ONE; m_noise[0].pos -= FRAC_ONE)
            m_noise[0].prng = (m_noise[0].prng << 1) | (((m_noise[0].prng >> 17) ^ (m_noise[0].prng >> 10)) & 1);

        // noise generator 1
        m_noise[1].pos += m_noise[1].step;
        for ( ; m_noise[1].pos >= FRAC_ONE; m_noise[1].pos -= FRAC_ONE)
            m_noise[1].prng = (m_noise[1].prng << 1) | (((m_noise[1].prng >> 17) ^ (m_noise[1].prng >> 10)) & 1);
    }
}

//
// helper to compute the output sample step from a voice's frequency and octave
//
uint32_t saa1099_generator_t::step_from_divisor(voice_t &voice)
{
    return uint32_t((uint64_t(INTERNAL_CLOCK/2) << FRAC_BITS) / (OUTPUT_FREQUENCY * ((511 - voice.frequency) << (8 - voice.octave))));
}

//
// helper to compute the output sample step from a a noise generator's control bits
//
uint32_t saa1099_generator_t::noise_step(noise_t &noise, int gen)
{
    // looks like noise is clocked 2x as fast, based on datasheet
    if (noise.frequency != 3)
        return uint32_t((uint64_t(INTERNAL_CLOCK/2) << FRAC_BITS) / (OUTPUT_FREQUENCY * (128 << noise.frequency)));
    else
        return m_voice[gen * 3].step * 2;
}



//===========================================================================
//
// cms_t
//
// This class manages Creative Music System (CMS) emulation.
//
//===========================================================================

//
// constructor
//
cms_t::cms_t() :
    m_register{ 0 }
{
}

//
// handle unimplemented reads in our address space
//
uint8_t cms_t::read_unimp(uint32_t address)
{
    return 0xff;
}

//
// handle unimplemented writes in our address space
//
void cms_t::write_unimp(uint32_t address, uint8_t data)
{
    m_register[address & 15] = data;
}

//
// handle data write to either SAA1099 chip
//
void cms_t::write_data(uint32_t address, uint8_t data)
{
    int which = (address >> 1) & 1;
    m_generator[which].process_event(m_register[(address & 15) ^ 1], data);
}

//
// handle address write to either SAA1099 chip
//
void cms_t::write_addr(uint32_t address, uint8_t data)
{
    m_register[address & 15] = data;
}

//
// handle reading the detection register (not sure what it really maps to)
//
uint8_t cms_t::read_detect(uint32_t address)
{
    // CMS test port(?); used in detection of CMS, should return value written to 227h
    //
    // INDY3 CMS detection:
    //   write 227 = AAh
    //   read 22A; if not = AAh, see if SB1.0
    //   write 227 = 55h
    //   read 22A; if not = 55h, see if SB1.0
    //   ok, this is a CMD
    //
    return m_register[7];
}
