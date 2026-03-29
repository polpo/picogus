#!/usr/bin/env python3
"""Decode ISA bus transactions from binary capture files.

Parses binary output from the PicoGUS ISA bus analyzer firmware.
Supports RLE-compressed ring buffer format.
"""

import struct
import sys
import argparse

# Binary format constants (must match isa_analyzer.cpp)
BINARY_MARKER_MAGIC = 0x1DE1DE1D
BINARY_MARKER_START = 0x42494E53  # "BINS"
BINARY_MARKER_END = 0x42494E45    # "BINE"
RLE_MAGIC = 0xFF000000
RLE_COUNT_MASK = 0x00FFFFFF


def read_binary_data(data):
    """Extract binary transaction data between markers."""
    marker_start = struct.pack('<II', BINARY_MARKER_MAGIC, BINARY_MARKER_START)
    marker_end = struct.pack('<II', BINARY_MARKER_MAGIC, BINARY_MARKER_END)

    start_idx = data.find(marker_start)
    if start_idx == -1:
        return None

    end_idx = data.find(marker_end, start_idx + 8)
    if end_idx == -1:
        end_idx = len(data)

    return data[start_idx + 8:end_idx]


def iter_transactions(binary_data):
    """Iterate over transactions, yielding (value, count) tuples.

    Uses value-first RLE encoding: value appears first, then optional
    trailer with repeat count.
    """
    i = 0
    pending_value = None
    pending_count = 0

    while i + 4 <= len(binary_data):
        word, = struct.unpack('<I', binary_data[i:i+4])
        i += 4

        if (word & 0xFF000000) == RLE_MAGIC:
            if pending_value is not None:
                pending_count += (word & RLE_COUNT_MASK)
        else:
            if pending_value is not None:
                yield pending_value, pending_count
            pending_value = word
            pending_count = 1

    if pending_value is not None:
        yield pending_value, pending_count


class ISATransactionDecoder:
    """Stateful decoder for ISA bus transactions."""

    # Well-known ISA port ranges
    PORT_RANGES = {
        (0x200, 0x207): "Game Port",
        (0x210, 0x217): "Expansion",
        (0x220, 0x22F): "SB",
        (0x240, 0x24F): "SB (alt) / GUS",
        (0x278, 0x27F): "LPT2",
        (0x2E8, 0x2EF): "COM4",
        (0x2F8, 0x2FF): "COM2",
        (0x300, 0x301): "MPU-401",
        (0x330, 0x331): "MPU-401 (alt)",
        (0x340, 0x34F): "GUS (hi)",
        (0x378, 0x37F): "LPT1",
        (0x388, 0x38B): "AdLib/OPL",
        (0x3BC, 0x3BF): "LPT3",
        (0x3E8, 0x3EF): "COM3",
        (0x3F8, 0x3FF): "COM1",
    }

    # Sound Blaster register decode (offset from base)
    SB_REGISTERS = {
        0x0: "FM Addr L",
        0x1: "FM Data L",
        0x2: "FM Addr R",
        0x3: "FM Data R",
        0x4: "Mixer Addr",
        0x5: "Mixer Data",
        0x6: "DSP Reset",
        0x8: "FM Addr",
        0x9: "FM Data",
        0xA: "DSP Read",
        0xC: "DSP Write/Status",
        0xE: "DSP Read Status",
        0xF: "DSP 16-bit IRQ Ack",
    }

    # DSP commands
    DSP_COMMANDS = {
        0x10: "Direct DAC 8-bit",
        0x14: "DMA DAC 8-bit",
        0x1C: "Auto-Init DMA DAC 8-bit",
        0x20: "Direct ADC 8-bit",
        0x24: "DMA ADC 8-bit",
        0x2C: "Auto-Init DMA ADC 8-bit",
        0x40: "Set Time Constant",
        0x41: "Set Output Sample Rate",
        0x42: "Set Input Sample Rate",
        0x48: "Set DMA Block Size",
        0x80: "Silence DAC",
        0xB0: "16-bit DMA (generic)",
        0xB2: "16-bit DMA autoinit (generic)",
        0xB4: "16-bit DMA FIFO (generic)",
        0xB6: "16-bit DMA autoinit FIFO (generic)",
        0xC0: "8-bit DMA (generic)",
        0xC2: "8-bit DMA autoinit (generic)",
        0xC4: "8-bit DMA FIFO (generic)",
        0xC6: "8-bit DMA autoinit FIFO (generic)",
        0xD0: "Halt DMA 8-bit",
        0xD1: "Speaker On",
        0xD3: "Speaker Off",
        0xD4: "Continue DMA 8-bit",
        0xD5: "Halt DMA 16-bit",
        0xD6: "Continue DMA 16-bit",
        0xD8: "Speaker Status",
        0xD9: "Exit autoinit DMA 16-bit",
        0xDA: "Exit autoinit DMA 8-bit",
        0xE1: "DSP Version",
        0xE3: "DSP Copyright",
        0xE4: "Write Test Register",
        0xE8: "Read Test Register",
        0xF2: "IRQ Request 8-bit",
        0xF3: "IRQ Request 16-bit",
    }

    # OPL register ranges
    OPL_REGISTERS = {
        (0x01, 0x01): "Waveform Select Enable",
        (0x02, 0x02): "Timer 1",
        (0x03, 0x03): "Timer 2",
        (0x04, 0x04): "Timer Control",
        (0x08, 0x08): "CSW / Note-Sel",
        (0x20, 0x35): "Tremolo/Vibrato/Sustain/KSR/Mult",
        (0x40, 0x55): "Key Scale Level / Output Level",
        (0x60, 0x75): "Attack Rate / Decay Rate",
        (0x80, 0x95): "Sustain Level / Release Rate",
        (0xA0, 0xA8): "Frequency (low 8 bits)",
        (0xB0, 0xB8): "Key On / Block / Frequency (high)",
        (0xBD, 0xBD): "Percussion Mode",
        (0xC0, 0xC8): "Feedback / Connection",
        (0xE0, 0xF5): "Waveform Select",
    }

    # Mixer registers (SB16)
    MIXER_REGISTERS = {
        0x00: "Reset",
        0x04: "Voice Volume",
        0x0A: "Mic Volume",
        0x22: "Master Volume",
        0x26: "FM Volume",
        0x28: "CD Volume",
        0x2E: "Line Volume",
        0x30: "Master Volume L",
        0x31: "Master Volume R",
        0x32: "Voice Volume L",
        0x33: "Voice Volume R",
        0x34: "FM Volume L",
        0x35: "FM Volume R",
        0x36: "CD Volume L",
        0x37: "CD Volume R",
        0x38: "Line Volume L",
        0x39: "Line Volume R",
        0x3B: "PC Speaker Volume",
        0x3C: "Output Switches",
        0x3D: "Input Switches L",
        0x3E: "Input Switches R",
        0x41: "Output Gain L",
        0x42: "Output Gain R",
        0x44: "Treble L",
        0x45: "Treble R",
        0x46: "Bass L",
        0x47: "Bass R",
        0x80: "IRQ Select",
        0x81: "DMA Select",
        0x82: "IRQ Status",
    }

    def __init__(self, sb_base=0x220):
        self.sb_base = sb_base
        self.opl_addr = None        # Last OPL address written (port 0x388/0x38A)
        self.opl_addr_hi = None     # Last OPL high-bank address (port 0x38A)
        self.mixer_addr = None      # Last mixer address written
        self.dsp_cmd = None         # Current DSP command being processed
        self.dsp_param_count = 0    # Number of parameter bytes expected
        self.dsp_params = []        # Collected parameter bytes
        self.dma_count = 0          # Consecutive DMA transfers

    def identify_port(self, addr):
        """Return a human-readable name for a port address."""
        for (lo, hi), name in self.PORT_RANGES.items():
            if lo <= addr <= hi:
                return name
        return None

    def decode(self, value):
        """Decode a single transaction value (32-bit stored event)."""
        is_ior = (value >> 31) & 1
        is_dma = (value >> 30) & 1
        addr = (value >> 8) & 0x3FF
        data = value & 0xFF

        direction = "IOR" if is_ior else "IOW"

        if is_dma:
            return f"DMA {'IN' if is_ior else 'OUT'}: 0x{data:02X}"

        port_name = self.identify_port(addr)
        base_str = f"{direction} 0x{addr:03X}: 0x{data:02X}"
        if port_name:
            base_str += f"  [{port_name}]"

        extra = self._decode_context(is_ior, addr, data)
        if extra:
            return f"{base_str}  # {extra}"
        return base_str

    def _decode_context(self, is_ior, addr, data):
        """Decode in context of previous operations."""
        # Sound Blaster register decode
        sb_offset = addr - self.sb_base
        if 0 <= sb_offset <= 0xF:
            return self._decode_sb(is_ior, sb_offset, data)

        # AdLib/OPL register decode
        if addr in (0x388, 0x389, 0x38A, 0x38B):
            return self._decode_opl(is_ior, addr, data)

        # MPU-401
        if addr == 0x330:
            if is_ior:
                return "MPU-401 Data Read"
            else:
                return f"MPU-401 Data Write: 0x{data:02X}"
        if addr == 0x331:
            if is_ior:
                ack = "ACK" if data == 0xFE else f"Status 0x{data:02X}"
                return f"MPU-401 Status: {ack}"
            else:
                cmds = {0x3F: "UART Mode", 0xFF: "Reset"}
                cmd_name = cmds.get(data, f"Command 0x{data:02X}")
                return f"MPU-401 Command: {cmd_name}"

        return None

    def _decode_sb(self, is_ior, offset, data):
        """Decode Sound Blaster register access."""
        reg_name = self.SB_REGISTERS.get(offset)
        if not reg_name:
            return None

        # Mixer address write
        if offset == 0x4 and not is_ior:
            self.mixer_addr = data
            mixer_name = self.MIXER_REGISTERS.get(data, f"Reg 0x{data:02X}")
            return f"Mixer Addr -> {mixer_name}"

        # Mixer data
        if offset == 0x5:
            if self.mixer_addr is not None:
                mixer_name = self.MIXER_REGISTERS.get(self.mixer_addr, f"Reg 0x{self.mixer_addr:02X}")
                return f"Mixer [{mixer_name}] = 0x{data:02X}"
            return reg_name

        # DSP Reset
        if offset == 0x6 and not is_ior:
            return f"DSP Reset {'assert' if data else 'deassert'}"

        # DSP Write (command/data)
        if offset == 0xC and not is_ior:
            return self._decode_dsp_write(data)

        # DSP Read
        if offset == 0xA and is_ior:
            return f"DSP Read: 0x{data:02X}"

        # DSP Read Status (check if data ready)
        if offset == 0xE and is_ior:
            ready = "ready" if data & 0x80 else "not ready"
            return f"DSP Read Status: {ready}"

        # DSP Write Status (check if ready for write)
        if offset == 0xC and is_ior:
            ready = "busy" if data & 0x80 else "ready"
            return f"DSP Write Status: {ready}"

        return reg_name

    def _decode_dsp_write(self, data):
        """Decode DSP command/parameter write."""
        if self.dsp_cmd is not None and self.dsp_param_count > 0:
            # This is a parameter byte
            self.dsp_params.append(data)
            self.dsp_param_count -= 1
            cmd_name = self.DSP_COMMANDS.get(self.dsp_cmd, f"Cmd 0x{self.dsp_cmd:02X}")
            param_str = f"param[{len(self.dsp_params)-1}] = 0x{data:02X}"
            if self.dsp_param_count == 0:
                self.dsp_cmd = None
            return f"DSP {cmd_name}: {param_str}"

        # New command
        cmd_name = self.DSP_COMMANDS.get(data, f"Unknown 0x{data:02X}")
        self.dsp_cmd = data
        self.dsp_params = []

        # Determine expected parameter count
        if data == 0x40:    # Set Time Constant
            self.dsp_param_count = 1
        elif data in (0x41, 0x42):  # Set Sample Rate
            self.dsp_param_count = 2
        elif data == 0x48:  # Set DMA Block Size
            self.dsp_param_count = 2
        elif data == 0x10:  # Direct DAC
            self.dsp_param_count = 1
        elif data == 0x80:  # Silence DAC
            self.dsp_param_count = 2
        elif data in (0x14, 0x24):  # DMA DAC/ADC 8-bit
            self.dsp_param_count = 2
        elif data & 0xF0 in (0xB0, 0xC0) and data not in self.DSP_COMMANDS:
            # SB16 generic DMA commands have 2 parameter bytes
            self.dsp_param_count = 2
        elif data in (0xB0, 0xB2, 0xB4, 0xB6, 0xC0, 0xC2, 0xC4, 0xC6):
            self.dsp_param_count = 2
        elif data == 0xE4:  # Write Test Register
            self.dsp_param_count = 1
        else:
            self.dsp_cmd = None
            self.dsp_param_count = 0

        return f"DSP Command: {cmd_name}"

    def _decode_opl(self, is_ior, addr, data):
        """Decode OPL register access."""
        if addr == 0x388:
            if is_ior:
                timer_flags = []
                if data & 0x80: timer_flags.append("IRQ")
                if data & 0x40: timer_flags.append("T1")
                if data & 0x20: timer_flags.append("T2")
                flags_str = " | ".join(timer_flags) if timer_flags else "clear"
                return f"OPL Status: {flags_str}"
            else:
                self.opl_addr = data
                return f"OPL Addr -> 0x{data:02X}"
        elif addr == 0x389:
            if not is_ior and self.opl_addr is not None:
                reg_name = self._opl_register_name(self.opl_addr)
                return f"OPL [0x{self.opl_addr:02X} {reg_name}] = 0x{data:02X}"
            return "OPL Data"
        elif addr == 0x38A:
            if not is_ior:
                self.opl_addr_hi = data
                return f"OPL Addr (hi) -> 0x{data:02X}"
        elif addr == 0x38B:
            if not is_ior and self.opl_addr_hi is not None:
                reg_name = self._opl_register_name(self.opl_addr_hi)
                return f"OPL [hi:0x{self.opl_addr_hi:02X} {reg_name}] = 0x{data:02X}"
            return "OPL Data (hi)"
        return None

    def _opl_register_name(self, reg):
        """Get human-readable OPL register name."""
        for (lo, hi), name in self.OPL_REGISTERS.items():
            if lo <= reg <= hi:
                return name
        return "Unknown"


def is_dma_transfer(value):
    """Check if this transaction is a DMA transfer."""
    return (value >> 30) & 1


def main():
    parser = argparse.ArgumentParser(
        description='Decode ISA bus transactions from binary capture files')
    parser.add_argument('file', help='Binary capture file')
    parser.add_argument('--expand', action='store_true',
                        help='Expand all repeated transactions')
    parser.add_argument('--collapse-dma', action='store_true',
                        help='Collapse consecutive DMA transfers')
    parser.add_argument('--sb-base', type=lambda x: int(x, 0), default=0x220,
                        help='Sound Blaster base port (default: 0x220)')
    parser.add_argument('--port-filter',
                        help='Only show ports in range (e.g., 0x220-0x22F)')
    args = parser.parse_args()

    with open(args.file, 'rb') as f:
        data = f.read()

    binary_data = read_binary_data(data)
    if binary_data is None:
        print("Error: No binary data markers found in file", file=sys.stderr)
        sys.exit(1)

    # Parse port filter
    filter_lo = None
    filter_hi = None
    if args.port_filter:
        parts = args.port_filter.split('-')
        filter_lo = int(parts[0], 0)
        filter_hi = int(parts[1], 0) if len(parts) > 1 else filter_lo

    decoder = ISATransactionDecoder(sb_base=args.sb_base)

    last_value = None
    last_decoded = None
    merged_count = 0
    total_transactions = 0
    shown_transactions = 0

    def flush_merged():
        nonlocal merged_count, shown_transactions
        if merged_count > 0:
            shown_transactions += merged_count
            if merged_count == 1:
                print(f"{last_value:08X} -> {last_decoded}")
            else:
                print(f"{last_value:08X} x {merged_count} -> {last_decoded}")
            merged_count = 0

    for value, count in iter_transactions(binary_data):
        total_transactions += count

        # Apply port filter
        if filter_lo is not None:
            is_dma = (value >> 30) & 1
            if not is_dma:
                addr = (value >> 8) & 0x3FF
                if addr < filter_lo or addr > filter_hi:
                    continue

        # Determine if we should expand
        expand_this = args.expand or (is_dma_transfer(value) and not args.collapse_dma)

        if expand_this:
            flush_merged()
            for _ in range(count):
                decoded = decoder.decode(value)
                print(f"{value:08X} -> {decoded}")
                shown_transactions += 1
            last_value = None
        else:
            decoded = decoder.decode(value)

            if value == last_value:
                merged_count += count
            else:
                flush_merged()
                last_value = value
                last_decoded = decoded
                merged_count = count

    flush_merged()

    print(f"\n--- {shown_transactions} of {total_transactions} transactions shown ---",
          file=sys.stderr)


if __name__ == "__main__":
    main()
