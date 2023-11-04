// license:BSD-3-Clause
// copyright-holders:Juergen Buchmueller, Manuel Abadia
/**********************************************
    Philips SAA1099 Sound driver
**********************************************/

#ifndef MAME_SOUND_SAA1099_H
#define MAME_SOUND_SAA1099_H

#pragma once

#include <cstdint>

//**************************************************************************
//  TYPE DEFINITIONS
//**************************************************************************

// ======================> saa1099_device

class saa1099_device
{
public:
	saa1099_device(uint32_t clock);

	void control_w(uint8_t data);
	void data_w(uint8_t data);

protected:
	// device-level overrides
	void device_start();
	void device_clock_changed();

public:
	// sound stream update overrides
	void sound_stream_update(int16_t *outputs, int samples);

private:
	struct saa1099_channel
	{
		saa1099_channel() : amplitude{ 0, 0 }, envelope{ 0, 0 } { }

		uint8_t frequency      = 0;      // frequency (0x00..0xff)
		bool freq_enable  = false;  // frequency enable
		bool noise_enable = false;  // noise enable
		uint8_t octave         = 0;      // octave (0x00..0x07)
		uint16_t amplitude[2];           // amplitude
		uint8_t envelope[2];             // envelope (0x00..0x0f or 0x10 == off)

		/* vars to simulate the square wave */
		inline uint32_t freq() const { return (511 - frequency) << (8 - octave); } // clock / ((511 - frequency) * 2^(8 - octave))
		int counter = 0;
		uint8_t level = 0;
	};

	struct saa1099_noise
	{
		saa1099_noise() { }

		/* vars to simulate the noise generator output */
		int counter = 0;
		int freq = 0;
		uint32_t level = 0xffffffffU;    // noise polynomial shifter
	};

	void envelope_w(int ch);

	uint8_t m_noise_params[2];           // noise generators parameters
	bool m_env_enable[2];           // envelope generators enable
	bool m_env_reverse_right[2];    // envelope reversed for right channel
	uint8_t m_env_mode[2];               // envelope generators mode
	bool m_env_bits[2];             // true = 3 bits resolution
	bool m_env_clock[2];            // envelope clock mode (true external)
	uint8_t m_env_step[2];               // current envelope step
	bool m_all_ch_enable;           // all channels enable
	bool m_sync_state;              // sync all channels
	uint8_t m_selected_reg;              // selected register
	saa1099_channel m_channels[6];  // channels
	saa1099_noise m_noise[2];       // noise generators
};

#endif // MAME_SOUND_SAA1099_H
