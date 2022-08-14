/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Copyright (C) 2020-2022  The DOSBox Staging Team
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "dosboxcompat.h"
#ifdef CIRCLE
#include <circle/logger.h>
#endif

#include <array>
#include <iomanip>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

/*
#include "control.h"
#include "dma.h"
#include "hardware.h"
#include "mixer.h"
#include "pic.h"
#include "setup.h"
#include "shell.h"
#include "string_utils.h"
*/
/*
#include "soft_limiter.h"
*/

// TODO timers
// #include "gustimer.h"

#define LOG_GUS 0 // set to 1 for detailed logging

// Global Constants
// ----------------

// AdLib emulation state constant
constexpr uint8_t ADLIB_CMD_DEFAULT = 85u;

// Buffer and memory constants
//constexpr int BUFFER_FRAMES = 48;
constexpr int BUFFER_FRAMES = 1024;
//constexpr uint32_t RAM_SIZE = 1024 * 1024;        // 1 MiB
constexpr uint32_t RAM_SIZE = 1024 * 64;        // 1 MiB

// DMA transfer size and rate constants
constexpr uint32_t BYTES_PER_DMA_XFER = 8 * 1024;         // 8 KiB per transfer
constexpr uint32_t ISA_BUS_THROUGHPUT = 32 * 1024 * 1024; // 32 MiB/s
constexpr uint16_t DMA_TRANSFERS_PER_S = ISA_BUS_THROUGHPUT / BYTES_PER_DMA_XFER;
constexpr double MS_PER_DMA_XFER = 1000.0 / DMA_TRANSFERS_PER_S;

// Voice-channel and state related constants
constexpr uint8_t MAX_VOICES = 32u;
constexpr uint8_t MIN_VOICES = 14u;
constexpr uint8_t VOICE_DEFAULT_STATE = 3u;

// DMA and IRQ extents and quantities
constexpr uint8_t MIN_DMA_ADDRESS = 0u;
constexpr uint8_t MAX_DMA_ADDRESS = 7u;
constexpr uint8_t MIN_IRQ_ADDRESS = 0u;
constexpr uint8_t MAX_IRQ_ADDRESS = 15u;
constexpr uint8_t DMA_IRQ_ADDRESSES = 8u; // number of IRQ and DMA channels
constexpr uint16_t DMA_TC_STATUS_BITMASK = 0b100000000; // Status in 9th bit

// Pan position constants
constexpr uint8_t PAN_DEFAULT_POSITION = 7u;
constexpr uint8_t PAN_POSITIONS = 16u;  // 0: -45-deg, 7: centre, 15: +45-deg

// Timer delay constants
constexpr uint32_t TIMER_1_DEFAULT_DELAY = 80;
constexpr uint32_t TIMER_2_DEFAULT_DELAY = 320;

// Volume scaling and dampening constants
constexpr auto DELTA_DB = 0.002709201;     // 0.0235 dB increments
constexpr int16_t VOLUME_INC_SCALAR = 512; // Volume index increment scalar
constexpr uint16_t VOLUME_LEVELS = 4096u;

// Interwave addressing constant
constexpr int16_t WAVE_WIDTH = 1 << 9; // Wave interpolation width (9 bits)

// IO address quantities
constexpr uint8_t READ_HANDLERS = 8u;
constexpr uint8_t WRITE_HANDLERS = 9u;

// A group of parameters defining the Gus's voice IRQ control that's also shared
// (as a reference) into each instantiated voice.
struct VoiceIrq {
	uint32_t vol_state = 0u;
	uint32_t wave_state = 0u;
	uint8_t status = 0u;
};

// A group of parameters used in the Voice class to track the Wave and Volume
// controls.
struct VoiceCtrl {
	uint32_t &irq_state;
	int32_t start = 0;
	int32_t end = 0;
	int32_t pos = 0;
	int32_t inc = 0;
	uint16_t rate = 0;
	uint8_t state = VOICE_DEFAULT_STATE;
};

// Collection types involving constant quantities
using address_array_t = std::array<uint8_t, DMA_IRQ_ADDRESSES>;
using pan_scalars_array_t = std::array<AudioFrame, PAN_POSITIONS>;
using ram_array_t = std::array<uint8_t, RAM_SIZE>;
/* using read_io_array_t = std::array<IO_ReadHandleObject, READ_HANDLERS>; */
using vol_scalars_array_t = std::array<float, VOLUME_LEVELS>;
/* using write_io_array_t = std::array<IO_WriteHandleObject, WRITE_HANDLERS>; */

// A Voice is used by the Gus class and instantiates 32 of these.
// Each voice represents a single "mono" render_buffer of audio having its own
// characteristics defined by the running program, such as:
//   - being 8bit or 16bit
//   - having a "position" along a left-right axis (panned)
//   - having its volume reduced by some amount (native-level down to 0)
//   - having start, stop, loop, and loop-backward controls
//   - informing the GUS DSP as to when an IRQ is needed to keep it playing
//
class Voice {
public:
	Voice(uint8_t num, VoiceIrq &irq) noexcept;
	inline void GenerateSamples(std::vector<int32_t> &render_buffer,
	                     const ram_array_t &ram,
	                     const vol_scalars_array_t &vol_scalars,
	                     const pan_scalars_array_t &pan_scalars,
	                     uint16_t requested_frames);

	uint8_t ReadVolState() const noexcept;
	uint8_t ReadWaveState() const noexcept;
	void ResetCtrls() noexcept;
	void WritePanPot(uint8_t pos) noexcept;
	void WriteVolRate(uint16_t rate, int playback_rate) noexcept;
	void WriteWaveRate(uint16_t rate, int playback_rate) noexcept;
	bool UpdateVolState(uint8_t state) noexcept;
	bool UpdateWaveState(uint8_t state) noexcept;

	VoiceCtrl vol_ctrl;
	VoiceCtrl wave_ctrl;

	uint32_t generated_8bit_ms = 0u;
	uint32_t generated_16bit_ms = 0u;

private:
	Voice() = delete;
	Voice(const Voice &) = delete;            // prevent copying
	Voice &operator=(const Voice &) = delete; // prevent assignment
	bool CheckWaveRolloverCondition() noexcept;
	bool Is16Bit() const noexcept;
	float GetVolScalar(const vol_scalars_array_t &vol_scalars);
	inline int16_t GetSample(const ram_array_t &ram) noexcept;
	int32_t PopWavePos() noexcept;
	float PopVolScalar(const vol_scalars_array_t &vol_scalars);
	inline int16_t Read8BitSample(const ram_array_t &ram, int32_t addr) const noexcept;
	inline int16_t Read16BitSample(const ram_array_t &ram, int32_t addr) const noexcept;
	uint8_t ReadCtrlState(const VoiceCtrl &ctrl) const noexcept;
	void IncrementCtrlPos(VoiceCtrl &ctrl, bool skip_loop) noexcept;
	bool UpdateCtrlState(VoiceCtrl &ctrl, uint8_t state) noexcept;

	// Control states
	enum CTRL : uint8_t {
		RESET = 0x01,
		STOPPED = 0x02,
		DISABLED = RESET | STOPPED,
		BIT16 = 0x04,
		LOOP = 0x08,
		BIDIRECTIONAL = 0x10,
		RAISEIRQ = 0x20,
		DECREASING = 0x40,
	};

	uint32_t irq_mask = 0u;
	uint8_t &shared_irq_status;
	uint8_t pan_position = PAN_DEFAULT_POSITION;
};

static void GUS_TimerEvent(Bitu t);
/* static void GUS_DMA_Event(uint32_t val); */

using voice_array_t = std::array<std::unique_ptr<Voice>, MAX_VOICES>;

typedef void IRQCallback(bool raise, void *pParam);

// The Gravis UltraSound GF1 DSP (classic)
// This class:
//   - Registers, receives, and responds to port address inputs, which are used
//     by the emulated software to configure and control the GUS card.
//   - Reads or provides audio samples via direct memory access (DMA)
//   - Provides shared resources to all of the Voices, such as the volume
//     reducing table, constant-power panning table, and IRQ states.
//   - Accumulates the audio from each active voice into a floating point
//     vector (the render_buffer), without resampling.
//   - Populates an autoexec line (ULTRASND=...) with its port, irq, and dma
//     addresses.
//
class Gus {
public:
#ifdef CIRCLE
	Gus(uint16_t port, IRQCallback* irq_callback, void* irq_param, GusTimer &gus_timer, CLogger &logger);
#else
	Gus(uint16_t port, IRQCallback* irq_callback, void* irq_param);
#endif
	virtual ~Gus();
	bool CheckTimer(size_t t);
	void PrintStats();
	// WriteToPort and ReadFromPort are private in DOSBox, but now they are the primary interfaces so need to be exposed
	void WriteToPort(io_port_t port, io_val_t value, io_width_t width);
	uint16_t ReadFromPort(io_port_t port, io_width_t width);

	struct Timer {
		uint32_t delay = 0;
		uint8_t value = 0xff;
		bool has_expired = true;
		bool is_counting_down = false;
		bool is_masked = false;
		bool should_raise_irq = false;
	};
	Timer timer_one = {TIMER_1_DEFAULT_DELAY};
	Timer timer_two = {TIMER_2_DEFAULT_DELAY};
#if 0 // no DMA yet
	bool PerformDmaTransfer();
#endif

private:
	Gus() = delete;
	Gus(const Gus &) = delete;            // prevent copying
	Gus &operator=(const Gus &) = delete; // prevent assignment

	void ActivateVoices(uint8_t requested_voices);
public:
	void AudioCallback(uint16_t requested_frames, int16_t* play_buffer);
private:
	void BeginPlayback();
	void CheckIrq();
	void CheckVoiceIrq();
#if 0 // no DMA yet
	uint32_t GetDmaOffset() noexcept;
	void UpdateDmaAddr(uint32_t offset) noexcept;
	void DmaCallback(DmaChannel *chan, DMAEvent event);
	void StartDmaTransfers();
	bool IsDmaPcm16Bit() noexcept;
	bool IsDmaXfer16Bit() noexcept;
#endif
	uint16_t ReadFromRegister();
	void PopulatePanScalars() noexcept;
	void PopulateVolScalars() noexcept;
	void PrepareForPlayback() noexcept;

	/* void RegisterIoHandlers(); */
	void Reset(uint8_t state);
	void SetLevelCallback(const AudioFrame &levels);
	void StopPlayback();
#if 0 // no DMA yet
	void UpdateDmaAddress(uint8_t new_address);
#endif
	void UpdateWaveMsw(int32_t &addr) const noexcept;
	void UpdateWaveLsw(int32_t &addr) const noexcept;

	void WriteToRegister();

	// Collections
	vol_scalars_array_t vol_scalars = {{}};
	std::vector<int32_t> render_buffer = {};
	// std::vector<int16_t> play_buffer = {};
	pan_scalars_array_t pan_scalars = {{}};
	ram_array_t ram = {{0u}};
	/* read_io_array_t read_handlers = {};   // std::functions */
	/* write_io_array_t write_handlers = {}; // std::functions */
#if 0 // no DMA yet
	const address_array_t dma_addresses = {
	        {MIN_DMA_ADDRESS, 1, 3, 5, 6, MAX_IRQ_ADDRESS, 0, 0}};
#endif
	const address_array_t irq_addresses = {
	        {MIN_IRQ_ADDRESS, 2, 5, 3, 7, 11, 12, MAX_IRQ_ADDRESS}};
	voice_array_t voices = {{nullptr}};

	// Struct and pointer members
	VoiceIrq voice_irq = {};
	//SoftLimiter soft_limiter;
	Voice *target_voice = nullptr;
#if 0 // no DMA yet
	DmaChannel *dma_channel = nullptr;
#endif
	//mixer_channel_t audio_channel = nullptr;
	uint8_t adlib_command_reg = ADLIB_CMD_DEFAULT;

	// Port address
	io_port_t port_base = 0u;

	// Voice states
	uint32_t active_voice_mask = 0u;
	uint16_t voice_index = 0u;
public:
	uint8_t active_voices = 0u;
private:
	uint8_t prev_logged_voices = 0u;

	// Register and playback rate
	uint32_t dram_addr = 0u;
public:
	int playback_rate = 0;
private:
	uint16_t register_data = 0u;
public:
	uint8_t selected_register = 0u;

private:
	// Control states
	uint8_t mix_ctrl = 0x0b; // latches enabled, LINEs disabled
	uint8_t sample_ctrl = 0u;
	uint8_t timer_ctrl = 0u;

	// DMA states
	uint16_t dma_addr = 0u;
	uint8_t dma_addr_nibble = 0u;
	// dma_ctrl would normally be a uint8_t as real hardware uses 8 bits,
	// but we store the DMA terminal count status in the 9th bit
	uint16_t dma_ctrl = 0u;
	uint8_t dma1 = 0u; // playback DMA
	uint8_t dma2 = 0u; // recording DMA

	// IRQ states
	uint8_t irq1 = 0u; // playback IRQ
	uint8_t irq2 = 0u; // MIDI IRQ
	uint8_t irq_status = 0u;
	uint8_t prev_interrupt = 0u;

	bool dac_enabled = false;
	bool irq_enabled = false;
	bool is_running = false;
	bool should_change_irq_dma = false;

        IRQCallback* irq_callback;
        void* irq_param;

public:
        // TODO timers
	// GusTimer &m_GusTimer;
#ifdef CIRCLE
private:
	CLogger &m_Logger;
#endif
};

inline uint16_t Gus::ReadFromPort(const io_port_t port, io_width_t width)
{
	/* LOG_MSG("GUS: Read from port %x", port); */
	switch (port - port_base) {
	case 0x206: return irq_status;
	case 0x208:
		uint8_t time;
		time = 0u;
		if (timer_one.has_expired)
			time |= (1 << 6);
		if (timer_two.has_expired)
			time |= 1 << 5;
		if (time & 0x60)
			time |= 1 << 7;
		if (irq_status & 0x04)
			time |= 1 << 2;
		if (irq_status & 0x08)
			time |= 1 << 1;
		return time;
	case 0x20a: return adlib_command_reg;
	case 0x302: return static_cast<uint8_t>(voice_index);
	case 0x303: return selected_register;
	case 0x304:
		if (width == io_width_t::word)
			return ReadFromRegister() & 0xffff;
		else
			return ReadFromRegister() & 0xff;
	case 0x305: return ReadFromRegister() >> 8;
	case 0x307:
		return dram_addr < ram.size() ? ram.at(dram_addr) : 0;
	default:
#if LOG_GUS
		LOG_MSG("GUS: Read at port %#x", port);
#endif
		break;
	}
	return 0xff;
}

using namespace std::placeholders;

/* uint8_t adlib_commandreg = ADLIB_CMD_DEFAULT; */
