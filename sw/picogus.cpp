/*
 *  Copyright (C) 2022-2024  Ian Scott
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

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/adc.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "hardware/regs/vreg_and_chip_reset.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"

#include "system/pico_reflash.h"
#include "system/flash_settings.h"

// For multifw
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"

#include "../common/picogus.h"

board_type_t BOARD_TYPE;

#ifdef PSRAM
#include "psram_spi.h"
psram_spi_inst_t psram_spi;
psram_spi_inst_t* async_spi_inst;
#endif
#include "isa_io.pio.h"

#ifdef ASYNC_UART
#include "stdio_async_uart.h"
#endif 
// UART_TX_PIN is defined in isa_io.pio.h
#define UART_RX_PIN (-1)
#define UART_ID     uart0
#define BAUD_RATE   230400

uint LED_PIN;

#include "M62429/M62429.h"
M62429* m62429;



#ifdef SOUND_SB
#include "sbdsp/sbdsp.h"
static uint16_t sb_port_test;
#endif
#ifdef SOUND_OPL
#include "opl.h"
void play_adlib(void);
#if OPL_CMD_BUFFER
#include "include/cmd_buffers.h"
cms_buffer_t opl_cmd_buffer = { {0}, 0, 0 };
#else
extern "C" void OPL_Pico_WriteRegister(unsigned int reg_num, unsigned int value);
static uint8_t opl_addr;
#endif // OPL_CMD_BUFFER
#if AUDIO_CALLBACK_CORE0
extern void audio_sample_handler(void);
#endif // AUDIO_CALLBACK_CORE0
#endif // SOUND_OPL

#ifdef CDROM
static uint16_t cdrom_port_test;
extern "C" void MKE_WRITE(uint16_t address, uint8_t value);
extern "C" uint8_t MKE_READ(uint16_t address);
extern "C" void mke_init();

#include "cdrom/cdrom.h"
#include "cdrom/cdrom_image_manager.h"
cdrom_t cdrom;

static uint32_t cur_read_idx;
#endif

#ifdef SOUND_GUS
#include "gus/gus-x.cpp"
#include "isa/isa_dma.h"
dma_inst_t dma_config;
static uint16_t gus_port_test;
void play_gus(void);
#endif


#ifdef SOUND_MPU
#include "mpu401/export.h"
#ifdef MPU_ONLY
void play_mpu(void);
#endif
#endif

#if SOUND_TANDY || SOUND_CMS
#include "include/cmd_buffers.h"
#include "square/square.h"
void play_psg(void);
#endif
#if SOUND_TANDY
tandy_buffer_t tandy_buffer = { {0}, 0, 0 };
#endif
#if SOUND_CMS
static uint8_t cms_detect = 0xFF;
cms_buffer_t cms_buffer = { {0}, 0, 0 };
#endif


#ifdef NE2000
extern "C" {
#include "ne2000/ne2000.h"
}
void play_ne2000(void);
#endif


#ifdef USB_JOYSTICK
#include "usb_hid/joy.h"
extern "C" joystate_struct_t joystate_struct;
uint8_t joystate_bin;
#include "hardware/pwm.h"
#endif
#ifdef USB_MOUSE
#include "mouse/8250uart.h"
#include "mouse/sermouse.h"
#endif
#ifdef USB_ONLY
void play_usb(void);
#endif

#if SOUND_GUS || SOUND_SB || SOUND_OPL || CDROM || SOUND_TANDY || SOUND_CMS
#include "audio/volctrl.h"
#endif


// PicoGUS control and data ports
static bool control_active = false;
static uint8_t sel_reg = 0;
static uint32_t cur_read = 0;
static uint32_t cur_write = 0;
static bool queueSaveSettings = false;
static bool queueReboot = false;

Settings settings;
void processSettings(void);

#define IOW_PIO_SM 0
#define IOR_PIO_SM 1

const char* firmware_string = PICO_PROGRAM_NAME " v" PICO_PROGRAM_VERSION_STRING;

static uint8_t basePort_low;
static uint8_t  mouseSensitivity_low;

__force_inline void select_picogus(uint8_t value) {
    // printf("select picogus %x\n", value);
    sel_reg = value;
    switch (sel_reg) {
    case CMD_MAGIC: // Magic string
    case CMD_PROTOCOL: // Protocol version
        break;
    case CMD_FWSTRING: // Firmware string
        cur_read = 0;
        break;
    case CMD_BOOTMODE: // Mode (GUS, OPL, MPU, etc...)
        break;
    case CMD_GUSPORT: // GUS Base port
    case CMD_OPLPORT: // Adlib Base port
    case CMD_SBPORT: // SB Base port
    case CMD_MPUPORT: // MPU Base port
    case CMD_TANDYPORT: // Tandy Base port
    case CMD_CMSPORT: // CMS Base port
        basePort_low = 0;
        break;
    case CMD_JOYEN: // enable joystick
        break;
    case CMD_GUSBUF: // Audio buffer size
    case CMD_GUSDMA: // DMA interval
    case CMD_GUS44K: // Force 44k
        break;
    case CMD_WTVOL: // Wavetable mixer volume
        break;
    case CMD_MPUDELAY: // MPU sysex delay
    case CMD_MPUFAKE: // MPU fake all notes off
        break;
    case CMD_OPLWAIT: // Adlib speed sensitive fix
        break;
    case CMD_MOUSEPORT:
        basePort_low = 0;
        break;
    case CMD_MOUSEPROTO:
    case CMD_MOUSERATE:
    case CMD_MOUSESEN:
        break;
    case CMD_NE2KPORT:
        basePort_low = 0;
        break;
    case CMD_WIFISSID:
        memset(settings.WiFi.ssid, 0, sizeof(settings.WiFi.ssid));
        cur_write = 0;
        break;
    case CMD_WIFIPASS:
        memset(settings.WiFi.password, 0, sizeof(settings.WiFi.password));
        cur_write = 0;
        break;
    case CMD_WIFIAPPLY:
    case CMD_WIFISTAT:
        break;
    case CMD_CDPORT: // CMS Base port
        basePort_low = 0;
        break;
    case CMD_CDLIST:
#ifdef CDROM
        if (cdrom.image_status == CD_STATUS_IDLE) {
            cdrom.image_status = CD_STATUS_BUSY;
            cdrom.image_command = CD_COMMAND_IMAGE_LIST;
            // puts("cdimages start");
        }
        // puts("cdimages read");
        cur_read = 0;
        cur_read_idx = 0;
#endif
        break;
    case CMD_CDSTATUS:
    case CMD_CDLOAD:
    case CMD_CDAUTOADV:
    case CMD_MAINVOL:
    case CMD_OPLVOL:
    case CMD_SBVOL:
    case CMD_CDVOL:
    case CMD_GUSVOL:
    case CMD_PSGVOL:
        break;
    case CMD_CDNAME:
        cur_write = 0;
    case CMD_CDERROR:
        cur_read = 0;
        break;
    case CMD_SAVE: // Select save settings register
    case CMD_REBOOT: // Select reboot register
    case CMD_DEFAULTS: // Select reset to defaults register
        break;
    case CMD_HWTYPE: // Hardware version
        break;
    case CMD_FLASH: // Firmware write mode
        pico_firmware_start();
        break;
    default:
        control_active = false;
        break;
    }
}

__force_inline void write_picogus_low(uint8_t value) {
    switch (sel_reg) {
    case CMD_GUSPORT: // GUS Base port
    case CMD_OPLPORT: // Adlib Base port
    case CMD_SBPORT: // SB Base port
    case CMD_MPUPORT: // MPU Base port
    case CMD_TANDYPORT: // Tandy Base port
    case CMD_CMSPORT: // CMS Base port
    case CMD_MOUSEPORT:  // USB Mouse port (0 - disabled)
    case CMD_CDPORT:  // USB Mouse port (0 - disabled)
        basePort_low = value;
        break;
    case CMD_MOUSESEN:  // USB Mouse Sensitivity (8.8 fixedpoint)
        mouseSensitivity_low = value;
        break;
    }
}

__force_inline void write_picogus_high(uint8_t value) {
    switch (sel_reg) {
    case CMD_GUSPORT: // GUS Base port
        settings.GUS.basePort = (value || basePort_low) ? ((value << 8) | basePort_low) : 0xFFFF;
#ifdef SOUND_GUS
        gus_port_test = settings.GUS.basePort >> 4 | 0x10;
#endif
        break;
    case CMD_OPLPORT: // Adlib Base port
        settings.SB.oplBasePort = (value || basePort_low) ? ((value << 8) | basePort_low) : 0xFFFF;
        break;
    case CMD_SBPORT: // SB Base port
        settings.SB.basePort = (value || basePort_low) ? ((value << 8) | basePort_low) : 0xFFFF;
#ifdef SOUND_SB
        sb_port_test = settings.SB.basePort >> 4;
#endif
        break;
    case CMD_MPUPORT: // MPU Base port
        settings.MPU.basePort = (value || basePort_low) ? ((value << 8) | basePort_low) : 0xFFFF;
        break;
    case CMD_TANDYPORT: // Tandy Base port
        settings.Tandy.basePort = (value || basePort_low) ? ((value << 8) | basePort_low) : 0xFFFF;
        break;
    case CMD_CMSPORT: // CMS Base port
        settings.CMS.basePort = (value || basePort_low) ? ((value << 8) | basePort_low) : 0xFFFF;
        break;
    case CMD_JOYEN: // enable joystick
        settings.Joy.basePort = value ? 0x201u : 0xffff;
        break;
    case CMD_GUSBUF: // GUS audio buffer size
        // Value is sent by pgusinit as the size - 1, so we need to add 1 back to it
        settings.GUS.audioBuffer = value + 1;
#ifdef SOUND_GUS
        GUS_SetAudioBuffer(settings.GUS.audioBuffer);
#endif
        break;
    case CMD_GUSDMA: // GUS DMA interval
        settings.GUS.dmaInterval = value;
#ifdef SOUND_GUS
        GUS_SetDMAInterval(settings.GUS.dmaInterval);
#endif
        break;
    case CMD_GUS44K: // Force 44k output
        settings.GUS.force44k = value;
#ifdef SOUND_GUS
        GUS_SetFixed44k(settings.GUS.force44k);
#endif
        break;
    case CMD_WTVOL: // Wavetable mixer volume
        settings.Global.waveTableVolume = value;
        if (BOARD_TYPE == PICOGUS_2) {
            m62429->setVolume(M62429_BOTH, settings.Global.waveTableVolume);
        }
        break;
    case CMD_MPUDELAY: // MPU SYSEX delay
        settings.MPU.delaySysex = value;
#ifdef SOUND_MPU
        MPU401_Init(settings.MPU.delaySysex, settings.MPU.fakeAllNotesOff);
#endif
        break;
    case CMD_MPUFAKE: // MPU fake all notes off
        settings.MPU.fakeAllNotesOff = value;
#ifdef SOUND_MPU
        MPU401_Init(settings.MPU.delaySysex, settings.MPU.fakeAllNotesOff);
#endif
        break;
    case CMD_OPLWAIT: // Adlib speed sensitive fix
        settings.SB.oplSpeedSensitive = value;
        break;
    case CMD_MOUSEPORT:  // USB Mouse port (0 - disabled)
        settings.Mouse.basePort = (value || basePort_low) ? ((value << 8) | basePort_low) : 0xFFFF;
        break;
    case CMD_MOUSEPROTO:  // USB Mouse protocol
        settings.Mouse.protocol = value;
#ifdef USB_MOUSE
        sermouse_set_protocol(settings.Mouse.protocol);
#endif
        break;
    case CMD_MOUSERATE:  // USB Mouse Report Rate
        settings.Mouse.reportRate = value;
#ifdef USB_MOUSE
        sermouse_set_report_rate_hz(settings.Mouse.reportRate);
#endif
        break;
    case CMD_MOUSESEN:  // USB Mouse Sensitivity (8.8 fixedpoint)
        settings.Mouse.sensitivity = (value << 8) | (mouseSensitivity_low & 0xFF);
#ifdef USB_MOUSE
        sermouse_set_sensitivity(settings.Mouse.sensitivity);
#endif
        break;
    case CMD_NE2KPORT: // NE2000 Base port
        settings.NE2K.basePort = (value || basePort_low) ? ((value << 8) | basePort_low) : 0xFFFF;
        break;
    case CMD_WIFISSID:
        settings.WiFi.ssid[cur_write++] = value;
        break;
    case CMD_WIFIPASS:
        settings.WiFi.password[cur_write++] = value;
        /* printf("%s\n", settings.WiFi.password); */
        break;
    case CMD_WIFIAPPLY:
        printf("Applying wifi settings: %s %s\n", settings.WiFi.ssid, settings.WiFi.password);
#ifdef PICOW
        PG_Wifi_Connect(settings.WiFi.ssid, settings.WiFi.password);
#endif
        break;
    case CMD_WIFISTAT:
#ifdef PICOW
        multicore_fifo_push_blocking(FIFO_WIFI_STATUS);
#endif
        break;
    case CMD_CDPORT: // CD Base port
        settings.CD.basePort = (value || basePort_low) ? ((value << 8) | basePort_low) : 0xFFFF;
#ifdef CDROM
        cdrom_port_test = settings.CD.basePort >> 4;
#endif
        break;
#ifdef CDROM
    case CMD_CDLOAD: // Load CD image
        cdrom.image_data = value;
        cdrom.image_status = CD_STATUS_BUSY;
        cdrom.image_command = CD_COMMAND_IMAGE_LOAD_INDEX;
        break;
    case CMD_CDNAME:
        if (!cur_write) {
            memset(cdrom.image_path, 0, sizeof(cdrom.image_path));
        }
        cdrom.image_path[cur_write++] = value;
        if (!value) {
            cur_write = 0;
            cdrom.image_status = CD_STATUS_BUSY;
            cdrom.image_command = CD_COMMAND_IMAGE_LOAD;
        }
        break;
#endif
    case CMD_CDAUTOADV: // enable auto advance of CD image on USB reinsert
        settings.CD.autoAdvance = value;
#ifdef CDROM
        cdman_set_autoadvance(settings.CD.autoAdvance);
#endif
        break;

    case CMD_MAINVOL: // Set the volume for CD Audio
        settings.Volume.mainVol = value;
#if SOUND_GUS || SOUND_SB || SOUND_OPL || CDROM || SOUND_TANDY || SOUND_CMS
        set_volume(CMD_MAINVOL);
#endif
        break;
    case CMD_OPLVOL: // Set the volume for Adlib
        settings.Volume.oplVol = value;
#ifdef SOUND_OPL
        set_volume(CMD_OPLVOL);
#endif
        break;
    case CMD_SBVOL: // Set the volume for Sound Blaster
        settings.Volume.sbVol = value;
#ifdef SOUND_SB
        set_volume(CMD_SBVOL);
#endif
        break;
        case CMD_CDVOL: // Set the volume for CD Audio
        settings.Volume.cdVol = value;
#ifdef CDROM
        set_volume(CMD_CDVOL);
#endif
        break;
        case CMD_GUSVOL: // Set the volume for GUS
        settings.Volume.gusVol = value;
#ifdef SOUND_GUS
        set_volume(CMD_GUSVOL);
#endif
        break;
        case CMD_PSGVOL: // Set the volume for PSG
        settings.Volume.psgVol = value;
#if SOUND_TANDY || SOUND_CMS
        set_volume(CMD_PSGVOL);
#endif
        break;

    // For multifw
    case CMD_BOOTMODE:
        settings.startupMode = value;
        printf("requesting startup mode: %u\n", value);
        break;
    case CMD_SAVE:
        queueSaveSettings = true;
        break;
    case CMD_REBOOT:
        watchdog_hw->scratch[3] = settings.startupMode;
        printf("rebooting into mode: %u\n", settings.startupMode);
        watchdog_reboot(0, 0, 0);
        break;
    case CMD_DEFAULTS:
        getDefaultSettings(&settings);
        processSettings();
        break;
    case CMD_FLASH: // Firmware write
        pico_firmware_write(value);
        break;
    }
}

__force_inline uint8_t read_picogus_low(void) {
    switch (sel_reg) {
    case CMD_GUSPORT: // GUS Base port
        return settings.GUS.basePort == 0xFFFF ? 0 : (settings.GUS.basePort & 0xFF);
    case CMD_OPLPORT: // Adlib Base port
        return settings.SB.oplBasePort == 0xFFFF ? 0 : (settings.SB.oplBasePort & 0xFF);
    case CMD_SBPORT: // SB Base port
        return settings.SB.basePort == 0xFFFF ? 0 : (settings.SB.basePort & 0xFF);
    case CMD_MPUPORT: // MPU Base port
        return settings.MPU.basePort == 0xFFFF ? 0 : (settings.MPU.basePort & 0xFF);
    case CMD_TANDYPORT: // Tandy Base port
        return settings.Tandy.basePort == 0xFFFF ? 0 : (settings.Tandy.basePort & 0xFF);
    case CMD_CMSPORT: // CMS Base port
        return settings.CMS.basePort == 0xFFFF ? 0 : (settings.CMS.basePort & 0xFF);
    case CMD_MOUSEPORT:  // USB Mouse port (0 - disabled)
        return settings.Mouse.basePort == 0xFFFF ? 0 : (settings.Mouse.basePort & 0xFF);
    case CMD_MOUSESEN:  // USB Mouse Sensitivity (8.8 fixedpoint)
        return settings.Mouse.sensitivity & 0xFF;
    case CMD_NE2KPORT:  // NE2000 Base port (0 - disabled)
        return settings.NE2K.basePort == 0xFFFF ? 0 : (settings.NE2K.basePort & 0xFF);
    case CMD_CDPORT: // SB Base port
        return settings.CD.basePort == 0xFFFF ? 0 : (settings.CD.basePort & 0xFF);
    default:
        return 0x0;
    }
}

__force_inline uint8_t read_picogus_high(void) {
    uint8_t ret;
    switch (sel_reg) {
    case CMD_MAGIC:  // PicoGUS magic string
        return 0xdd;
    case CMD_PROTOCOL:  // PicoGUS protocol version
        return PICOGUS_PROTOCOL_VER;
    case CMD_FWSTRING: // Firmware string
        ret = firmware_string[cur_read++];
        if (ret == 0) { // Null terminated
            cur_read = 0;
        }
        return ret;
    case CMD_BOOTMODE: // Mode (GUS, OPL, MPU, etc...)
        return settings.startupMode;
    case CMD_GUSPORT: // GUS Base port
        return settings.GUS.basePort == 0xFFFF ? 0 : (settings.GUS.basePort >> 8);
    case CMD_OPLPORT: // Adlib Base port
        return settings.SB.oplBasePort == 0xFFFF ? 0 : (settings.SB.oplBasePort >> 8);
    case CMD_SBPORT: // SB Base port
        return settings.SB.basePort == 0xFFFF ? 0 : (settings.SB.basePort >> 8);
    case CMD_MPUPORT: // MPU Base port
        return settings.MPU.basePort == 0xFFFF ? 0 : (settings.MPU.basePort >> 8);
    case CMD_TANDYPORT: // Tandy Base port
        return settings.Tandy.basePort == 0xFFFF ? 0 : (settings.Tandy.basePort >> 8);
    case CMD_CMSPORT: // CMS Base port
        return settings.CMS.basePort == 0xFFFF ? 0 : (settings.CMS.basePort >> 8);
    case CMD_JOYEN: // enable joystick
        return settings.Joy.basePort == 0x201u;
    case CMD_GUSBUF: // GUS audio buffer size
        return settings.GUS.audioBuffer - 1;
    case CMD_GUSDMA: // GUS DMA interval
        return settings.GUS.dmaInterval;
    case CMD_GUS44K: // Force 44k output
        return settings.GUS.force44k;
    case CMD_WTVOL: // Wavetable mixer volume
        return (BOARD_TYPE == PICOGUS_2) ? m62429->getVolume(0) : 0;
    case CMD_MPUDELAY: // SYSEX delay
        return settings.MPU.delaySysex;
    case CMD_MPUFAKE: // MPU fake all notes off
        return settings.MPU.fakeAllNotesOff;
    case CMD_OPLWAIT: // Adlib speed sensitive fix
        return settings.SB.oplSpeedSensitive;
    case CMD_MOUSEPORT:  // USB Mouse port (0 - disabled)
        return settings.Mouse.basePort == 0xFFFF ? 0 : (settings.Mouse.basePort >> 8);
    case CMD_MOUSEPROTO:  // USB Mouse protocol
        return settings.Mouse.protocol;
    case CMD_MOUSERATE:  // USB Mouse Report Rate
        return settings.Mouse.reportRate;
    case CMD_MOUSESEN:  // USB Mouse Sensitivity (8.8 fixedpoint)
        return settings.Mouse.sensitivity >> 8;
    case CMD_NE2KPORT: // NE2000 Base port
        return settings.NE2K.basePort == 0xFFFF ? 0 : (settings.NE2K.basePort >> 8);
    /*
    case CMD_WIFISSID:
        break;
    case CMD_WIFIPASS:
        break;
    */
    case CMD_WIFISTAT:
#ifdef PICOW
        return PG_Wifi_ReadStatusStr();
#else
        return 0;
#endif
        break;
    case CMD_CDPORT: // CD Base port
        return settings.CD.basePort == 0xFFFF ? 0 : (settings.CD.basePort >> 8);
#ifdef CDROM
    case CMD_CDSTATUS:
        // printf("cdstatus %x\n", cdrom.image_status);
        return cdrom.image_status;
    case CMD_CDLIST:
        if (cur_read_idx == cdrom.image_count) { // If end of the images
            cur_read_idx = cur_read = 0;
            cdrom.image_status = CD_STATUS_IDLE;
            cdman_list_images_free(cdrom.image_list, cdrom.image_count);
            return 0x04; // EOT
        }
        ret = cdrom.image_list[cur_read_idx][cur_read++];
        putchar(ret);
        if (ret == 0) { // Null terminated
            ++cur_read_idx;
            cur_read = 0;
        }
        return ret;
    case CMD_CDLOAD: // Load CD image
        return cdman_current_image_index();
    case CMD_CDNAME: // Firmware string
        ret = cdrom.image_path[cur_read++];
        if (ret == 0) { // Null terminated
            cur_read = 0;
        }
        return ret;
    case CMD_CDERROR: // Error string
        ret = cdrom.error_str[cur_read++];
        if (ret == 0) { // Null terminated
            cur_read = 0;
        }
        return ret;
#endif
    case CMD_CDAUTOADV: // enable joystick
        return settings.CD.autoAdvance;
    case CMD_MAINVOL: // CD audio volume
        return settings.Volume.mainVol;
    case CMD_OPLVOL: // Adlib volume
        return settings.Volume.oplVol;
    case CMD_SBVOL: // Sound Blaster volume
        return settings.Volume.sbVol;
    case CMD_CDVOL: // CD audio volume
        return settings.Volume.cdVol;
    case CMD_GUSVOL: // GUS volume
        return settings.Volume.gusVol;
    case CMD_PSGVOL: // PSG volume
        return settings.Volume.psgVol;
    case CMD_HWTYPE: // Hardware version
        return BOARD_TYPE;
    case CMD_FLASH:
        // Get status of firmware write
        return pico_firmware_getStatus();
    default:
        return 0xff;
    }
}


void processSettings(void) {
#if defined(SOUND_GUS)
    settings.startupMode = GUS_MODE;
    set_volume(CMD_GUSVOL);
#elif (SOUND_TANDY || SOUND_CMS)
    settings.startupMode = PSG_MODE;
    set_volume(CMD_PSGVOL);
#elif defined(SOUND_SB)
    settings.startupMode = SB_MODE;
    set_volume(CMD_SBVOL);
    set_volume(CMD_OPLVOL);
#elif defined(SOUND_OPL)
    settings.startupMode = ADLIB_MODE;
    set_volume(CMD_OPLVOL);
#elif defined(MPU_ONLY)
    settings.startupMode = MPU_MODE;
#elif defined(USB_ONLY)
    settings.startupMode = USB_MODE;
#elif defined(NE2000)
    settings.startupMode = NE2000_MODE;
#else
    settings.startupMode = INVALID_MODE;
#endif
#ifdef SOUND_SB
    sb_port_test = settings.SB.basePort >> 4;
#endif
#ifdef SOUND_GUS
    gus_port_test = settings.GUS.basePort >> 4 | 0x10;
    GUS_SetFixed44k(settings.GUS.force44k);
    GUS_SetAudioBuffer(settings.GUS.audioBuffer);
    GUS_SetDMAInterval(settings.GUS.dmaInterval);
#endif
#ifdef USB_MOUSE
    sermouse_set_protocol(settings.Mouse.protocol);
    sermouse_set_report_rate_hz(settings.Mouse.reportRate);
    sermouse_set_sensitivity(settings.Mouse.sensitivity);
#endif
#ifdef CDROM
    cdrom_port_test = settings.CD.basePort >> 4;
    printf("cdrom base port: %x\n", settings.CD.basePort);
    cdman_set_autoadvance(settings.CD.autoAdvance);
#endif
    if (BOARD_TYPE == PICOGUS_2) {
        m62429->setVolume(M62429_BOTH, settings.Global.waveTableVolume);
    }
}


static constexpr uint32_t IO_WAIT = 0xffffffffu;
static constexpr uint32_t IO_END = 0x0u;
// OR with 0x0000ff00 is required to set pindirs in the PIO
static constexpr uint32_t IOR_SET_VALUE = 0x0000ff00u;

__force_inline void handle_iow(void) {
    uint32_t iow_read = pio_sm_get(pio0, IOW_PIO_SM); //>> 16;
    // printf("%x", iow_read);
    uint16_t port = (iow_read >> 8) & 0x3FF;
    // printf("IOW: %x %x\n", port, iow_read & 0xFF);
#ifdef SOUND_GUS
    if ((port >> 4 | 0x10) == gus_port_test) {
        port -= settings.GUS.basePort;
        switch (port) {
        case 0x8:
        case 0xb:
        case 0x102:
        case 0x103:
        case 0x104:
            // Fast write, don't set iochrdy by writing 0
            pio_sm_put(pio0, IOW_PIO_SM, IO_END);
            write_gus(port, iow_read & 0xFF);
            // Fast write - return early as we've already written 0x0u to the PIO
            return;
            break;
        default:
            // gpio_xor_mask(LED_PIN);
            // Slow write, set iochrdy by writing non-0
            pio_sm_put(pio0, IOW_PIO_SM, IO_WAIT);
            write_gus(port, iow_read & 0xFF);
            gpio_xor_mask(LED_PIN);
            break;
        }
        // printf("GUS IOW: port: %x value: %x\n", port, value);
        // puts("IOW");
    } else // if follows down below
#endif // SOUND_GUS
#ifdef SOUND_SB
    if ((port >> 4) == sb_port_test) {
        switch (port - settings.SB.basePort) {
        // OPL ports
        case 0x8:
            // Fast write
            pio_sm_put(pio0, IOW_PIO_SM, IO_END);
            // pio_sm_put(pio0, IOW_PIO_SM, IO_WAIT);
#if OPL_CMD_BUFFER
            opl_cmd_buffer.cmds[opl_cmd_buffer.head].addr = (uint16_t)(iow_read & 0xFF);
#else
            opl_addr = (iow_read & 0xff);
#endif
            // Fast write - return early as we've already written 0x0u to the PIO
            return;
            break;
        case 0x9:
            pio_sm_put(pio0, IOW_PIO_SM, IO_WAIT);
#if OPL_CMD_BUFFER
            opl_cmd_buffer.cmds[opl_cmd_buffer.head++].data = (uint8_t)(iow_read & 0xFF);
#else
            OPL_Pico_WriteRegister(opl_addr, iow_read & 0xff);
#endif
            break;
        // DSP ports
        default:
            pio_sm_put(pio0, IOW_PIO_SM, IO_WAIT);                        
            sbdsp_process();
            sbdsp_write(port & 0xF,iow_read & 0xFF);       
            sbdsp_process();                                         
            break;
        } 
    } else // if follows down below
#endif // SOUND_SB
#ifdef CDROM
    if ((port >> 4) == cdrom_port_test) {      
        pio_sm_put(pio0, IOW_PIO_SM, IO_WAIT);
        // putchar('w');
        MKE_WRITE(port, iow_read & 0xFF);
    } else // if follows down below
#endif
#if defined(SOUND_OPL)
    if ((port & 0x3fe) == settings.SB.oplBasePort) {
        if ((port & 1) == 0) {
            // Fast write
            pio_sm_put(pio0, IOW_PIO_SM, IO_END);
#if OPL_CMD_BUFFER
            opl_cmd_buffer.cmds[opl_cmd_buffer.head].addr = (uint16_t)(iow_read & 0xFF);
#else
            opl_addr = (iow_read & 0xff);
#endif
            // Fast write - return early as we've already written 0x0u to the PIO
            return;
        } else {
            pio_sm_put(pio0, IOW_PIO_SM, IO_WAIT);
            if (settings.SB.oplSpeedSensitive) {
                busy_wait_us(1); // busy wait for speed sensitive games
            }
#if OPL_CMD_BUFFER
            opl_cmd_buffer.cmds[opl_cmd_buffer.head++].data = (uint8_t)(iow_read & 0xFF);
#else
            OPL_Pico_WriteRegister(opl_addr, iow_read & 0xff);
#endif
        }
    } else // if follows down below
#endif // SOUND_OPL
#ifdef SOUND_TANDY
    if (port == settings.Tandy.basePort) {
        pio_sm_put(pio0, IOW_PIO_SM, IO_END);
        tandy_buffer.cmds[tandy_buffer.head++] = iow_read & 0xFF;
        return;
    } else // if follows down below
#endif // SOUND_TANDY
#ifdef USB_JOYSTICK
    if (port == settings.Joy.basePort) {
        pio_sm_put(pio0, IOW_PIO_SM, IO_END);
        // Set times in # of cycles (affected by clkdiv) for each PWM slice to count up and wrap back to 0
        // TODO better calibrate this
        // GUS w/ gravis gamestick -
        //   sbdiag: 322/267 16/18 572/520 
        //   gravutil 34/29 2/2 61/56
        //   checkit 556/451 23/25 984/902
        // Noname w/ gravis gamestick
        //   sbdiag: 256/214 7/6 495/435
        //   gravutil 28/23 1/1 53/47
        //   checkit 355/284 6/5 675/585
        pwm_set_wrap(0, 2000 + ((uint16_t)joystate_struct.joy1_x << 6));
        pwm_set_wrap(1, 2000 + ((uint16_t)joystate_struct.joy1_y << 6));
        pwm_set_wrap(2, 2000 + ((uint16_t)joystate_struct.joy2_x << 6));
        pwm_set_wrap(3, 2000 + ((uint16_t)joystate_struct.joy2_y << 6));
        // Convince PWM to run as one-shot by immediately setting wrap to 0. This will take effect once the wrap
        // times set above hit, so after wrapping the counter value will stay at 0 instead of counting again
        pwm_set_wrap(0, 0);
        pwm_set_wrap(1, 0);
        pwm_set_wrap(2, 0);
        pwm_set_wrap(3, 0);
        return;
    } else // if follows down below
#endif // USB_JOYSTICK
#ifdef USB_MOUSE
    if ((port & ~7) == settings.Mouse.basePort) {
        pio_sm_put(pio0, IOW_PIO_SM, IO_WAIT);      // leave some time for UART logic emualtion
        uartemu_write(port & 7, iow_read & 0xFF);
    } else // if follows down below
#endif // USB_MOUSE
#ifdef NE2000
    if((port & ~0x1F) == settings.NE2K.basePort) {
        pio_sm_put(pio0, IOW_PIO_SM, IO_WAIT);
        PG_NE2000_Write(port & 0x1f, iow_read & 0xFF);        
    } else // if follows down below
#endif
#ifdef SOUND_CMS
    if ((port & 0x3f0) == settings.CMS.basePort) {
        pio_sm_put(pio0, IOW_PIO_SM, IO_END);
        switch (port & 0xf) {
        // SAA data/address ports
        case 0x0:
        case 0x1:
        case 0x2:
        case 0x3:
            cms_buffer.cmds[cms_buffer.head++] = {
                port,
                (uint8_t)(iow_read & 0xFF)
            };
            break;
        // CMS autodetect ports
        case 0x6:
        case 0x7:
            cms_detect = iow_read & 0xFF;
            break;
        }
        return;
    } else
#endif // SOUND_CMS
#ifdef SOUND_MPU
    if ((port & 0x3fe) == settings.MPU.basePort) {
        switch (port & 0xf) {
        case 0:
            pio_sm_put(pio0, IOW_PIO_SM, IO_WAIT);
            // printf("MPU IOW: port: %x value: %x\n", port, iow_read & 0xFF);
            MPU401_WriteData(iow_read & 0xFF, true);
            gpio_xor_mask(LED_PIN);
            break;
        case 1:
            pio_sm_put(pio0, IOW_PIO_SM, IO_WAIT);
            MPU401_WriteCommand(iow_read & 0xFF, true);
            // printf("MPU IOW: port: %x value: %x\n", port, iow_read & 0xFF);
            // __dsb();
            break;
        }
    } else
#endif // SOUND_MPU
    // PicoGUS control
    if (port == CONTROL_PORT) {
        pio_sm_put(pio0, IOW_PIO_SM, IO_WAIT);
        // printf("iow control port: %x %d\n", iow_read & 0xff, control_active);
        if ((iow_read & 0xFF) == 0xCC) {
            // printf("activate ");
            control_active = true;
        } else if (control_active) {
            select_picogus(iow_read & 0xFF);
        }
    } else if (port == DATA_PORT_LOW) {
        pio_sm_put(pio0, IOW_PIO_SM, IO_END);
        if (control_active) {
            write_picogus_low(iow_read & 0xFF);
        }
        // Fast write - return early as we've already written 0x0u to the PIO
        return;
    } else if (port == DATA_PORT_HIGH) {
        // printf("iow data port: %x\n", iow_read & 0xff);
        pio_sm_put(pio0, IOW_PIO_SM, IO_WAIT);
        if (control_active) {
            write_picogus_high(iow_read & 0xFF);
        }
    }
    // Fallthrough if no match, or for slow write, reset PIO
    pio_sm_put(pio0, IOW_PIO_SM, IO_END);
    if (queueSaveSettings) {
        saveSettings(&settings);
        queueSaveSettings = false;
    }
}

__force_inline void handle_ior(void) {
    uint8_t x;
    uint16_t port = pio_sm_get(pio0, IOR_PIO_SM) & 0x3FF;
#if defined(SOUND_GUS)
    if ((port >> 4 | 0x10) == gus_port_test) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | read_gus(port - settings.GUS.basePort));
        // gpio_xor_mask(LED_PIN);
    } else // if follows down below
#endif
#if defined(SOUND_SB)
    if ((port >> 4) == sb_port_test) {
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        switch (port - settings.SB.basePort) {
        case 0x8:
#if OPL_CMD_BUFFER
            // wait for OPL buffer to process
            while (opl_cmd_buffer.tail != opl_cmd_buffer.head) {
                tight_loop_contents();
            }
#endif
            pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | OPL_Pico_PortRead(OPL_REGISTER_PORT));
            break;
        default:
            sbdsp_process();
            pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | sbdsp_read(port & 0xF));        
            sbdsp_process();
            break;
        }
    } else // if follows down below
#endif
#if defined(CDROM)
    if ((port >> 4) == cdrom_port_test) {
    // if ((port >> 4) == 0x23) {
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | MKE_READ(port));
        // putchar('r');
    } else // if follows down below
#endif
#if defined(SOUND_OPL)
    if (port == settings.SB.oplBasePort) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
#if OPL_CMD_BUFFER
        // wait for OPL buffer to process
        while (opl_cmd_buffer.tail != opl_cmd_buffer.head) {
            tight_loop_contents();
        }
#endif
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | OPL_Pico_PortRead(OPL_REGISTER_PORT));
    } else // if follows down below
#endif
#if defined(SOUND_MPU)
    if (port == settings.MPU.basePort) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        // printf("MPU IOR: port: %x value: %x\n", port, value);
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | MPU401_ReadData());
    } else if (port == settings.MPU.basePort + 1) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        // printf("MPU IOR: port: %x value: %x\n", port, value);
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | MPU401_ReadStatus());
    } else // if follows down below
#endif
#ifdef NE2000
    if((port & ~0x1F) == settings.NE2K.basePort) {
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | PG_NE2000_Read(port & 0x1f));
        return;
    }
#endif
#ifdef USB_JOYSTICK
    if (port == settings.Joy.basePort) {
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        uint8_t value =
            // Proportional bits: 1 if counter is still counting, 0 otherwise
            (bool)pwm_get_counter(0) |
            ((bool)pwm_get_counter(1) << 1) |
            ((bool)pwm_get_counter(2) << 2) |
            ((bool)pwm_get_counter(3) << 3) |
            joystate_struct.button_mask;
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | value);
    } else // if follows down below
#endif // USB_JOYSTICK
#ifdef USB_MOUSE
    if ((port & ~7) == settings.Mouse.basePort) {
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | uartemu_read(port & 7));
        return;
    } else // if follows down below
#endif // USB_MOUSE
#if defined(SOUND_CMS)
    if ((port & 0x3f0) == settings.CMS.basePort) {
        switch (port & 0xf) {
        // CMS autodetect ports
        case 0x4:
            pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
            pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | 0x7F);
            return;
        case 0xa:
        case 0xb:
            pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
            pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | cms_detect);
            return;
        default:
            pio_sm_put(pio0, IOR_PIO_SM, IO_END);
            return;
        }
    } else
#endif // SOUND_CMS
    if (port == CONTROL_PORT) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | sel_reg);
    } else if (port == DATA_PORT_LOW) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | read_picogus_low());
    } else if (port == DATA_PORT_HIGH) {
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | read_picogus_high());
    } else {
        // Reset PIO
        pio_sm_put(pio0, IOR_PIO_SM, IO_END);
    }
}

#ifdef USE_IRQ
void io_isr(void) {
    // Prioritize handling of ior because we need to react faster for IOCHRDY
    if (__builtin_expect(!!(pio0->ints1 & (1 << IOR_PIO_SM)), true)) {
        handle_ior();
    } else {
        handle_iow();
    }
}
#endif

void err_blink(void) {
    for (;;) {
        //gpio_xor_mask(LED_PIN);//need to abscrat  led functions out to handle chipdown vs  picow
        busy_wait_ms(100);
    }
}

constexpr uint32_t rp2_clock = RP2_CLOCK_SPEED;
constexpr float psram_clkdiv = (float)rp2_clock / 200000.0;
constexpr float pwm_clkdiv = (float)rp2_clock / 22727.27;
constexpr float iow_clkdiv = (float)rp2_clock / 183000.0;

constexpr uint32_t iow_rxempty = 1u << (PIO_FSTAT_RXEMPTY_LSB + IOW_PIO_SM);
__force_inline bool iow_has_data() {
    return !(pio0->fstat & iow_rxempty);
}

constexpr uint32_t ior_rxempty = 1u << (PIO_FSTAT_RXEMPTY_LSB + IOR_PIO_SM);
__force_inline bool ior_has_data() {
    return !(pio0->fstat & ior_rxempty);
}

#if AUDIO_CALLBACK_CORE0
static constexpr uint32_t clocks_per_sample_minus_one = (SYS_CLK_HZ / 44100) - 1;
static constexpr uint pwm_slice_num = 4; // slices 0-3 are taken by USB joystick support
#endif

#include "hardware/structs/xip_ctrl.h"
int main()
{
    busy_wait_ms(250);
#ifdef ASYNC_UART
    stdio_async_uart_init_full(UART_ID, BAUD_RATE, UART_TX_PIN, UART_RX_PIN);
#else
    stdio_init_all();
#endif
    puts(firmware_string);
    io_rw_32 *reset_reason = (io_rw_32 *) (VREG_AND_CHIP_RESET_BASE + VREG_AND_CHIP_RESET_CHIP_RESET_OFFSET);
    if (*reset_reason & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_POR_BITS) {
        puts("I was reset due to power on reset or brownout detection.");
    } else if (*reset_reason & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_RUN_BITS) {
        puts("I was reset due to the RUN pin (either manually or due to ISA RESET signal)");
    } else if(*reset_reason & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_PSM_RESTART_BITS) {
        puts("I was reset due the debug port");
    }

    // Load settings from flash
    loadSettings(&settings, true /* migrate */);
    hw_clear_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_EN_BITS);

    // Determine board type. GPIO 29 is grounded on PicoGUS v2.0, and on a Pico, it's VSYS/3 (~1.666V)
    // GPIO 25 must be high to read GPIO 29 on the Pico W
    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
    gpio_put(25, 1);
    adc_init();
    adc_gpio_init(29);
    // Select ADC input 3 (GPIO29)
    adc_select_input(3);
    // Read several times to let ADC stabilize
    adc_read(); adc_read(); adc_read(); adc_read(); adc_read();
    uint16_t result = adc_read();
    printf("ADC value: 0x%03x... ", result);
    gpio_put(25, 0);
    gpio_deinit(25);

    if (result > 0x100) {
        puts("Running on Pico-based board (PicoGUS v1.1+, PicoGUS Femto)");
        // On Pico-based board (PicoGUS v1.1+, PicoGUS Femto)
#ifndef PICOW        
        LED_PIN = 1 << PICO_DEFAULT_LED_PIN;
        gpio_init(PICO_DEFAULT_LED_PIN);
        gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif
        BOARD_TYPE = PICO_BASED;
    } else {
        // On chipdown board (PicoGUS v2.0)
        puts("Running on PicoGUS v2.0");
        LED_PIN = 1 << 23;
        gpio_init(23);
        gpio_set_dir(23, GPIO_OUT);
        BOARD_TYPE = PICOGUS_2;
    }
    gpio_set_mask(LED_PIN);

    if (BOARD_TYPE == PICOGUS_2) {
        // Create new interface to M62429 digital volume control
        m62429 = new M62429();
        // Data pin = GPIO24, data pin = GPIO25
#ifdef M62429_PIO
        m62429->begin(24, 25, pio1, -1);
#else
        m62429->begin(24, 25);
        // Initial volume is set in processSettings()
#endif // M62429_PIO
    }

    gpio_init(IRQ_PIN);
    gpio_set_dir(IRQ_PIN, GPIO_OUT);
    gpio_set_drive_strength(IRQ_PIN, GPIO_DRIVE_STRENGTH_12MA);

#ifdef SOUND_MPU
    puts("Initing MIDI UART...");
    uart_init(UART_ID, 31250);
    uart_set_translate_crlf(UART_ID, false);
    uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);
    if (BOARD_TYPE == PICO_BASED) {
        // Original hardware drives MIDI directly from RP2040. Newer HW uses an open drain inverter
        // PicoGUS v1.2 also uses an open drain but we can't detect that particular board
        gpio_set_drive_strength(UART_TX_PIN, GPIO_DRIVE_STRENGTH_12MA);
    }
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
#endif // SOUND_MPU

#ifdef PSRAM_CORE0
#ifdef PSRAM
    puts("Initing PSRAM...");
    // Try different PSRAM strategies
    if (BOARD_TYPE == PICOGUS_2) {
        psram_spi = psram_spi_init_clkdiv(pio1, -1, psram_clkdiv /* clkdiv */, false /* fudge */);
#if TEST_PSRAM
        // Only bother to test every 97th address
        if (test_psram(&psram_spi, 97) == 1) {
            printf("Default PSRAM strategy of no fudge not working, switching to fudge\n");
            psram_spi_uninit(psram_spi, false /* fudge */);
            psram_spi = psram_spi_init_clkdiv(pio1, -1, psram_clkdiv /* clkdiv */, true /* fudge */);
            if (test_psram(&psram_spi, 97) == 1) { 
                printf("Error: No PSRAM strategies found to work!\n");
                err_blink();
            }
        }
#endif // TEST_PSRAM
    } else {
        psram_spi = psram_spi_init_clkdiv(pio1, -1, psram_clkdiv /* clkdiv */, true /* fudge */);
#if TEST_PSRAM
        if (test_psram(&psram_spi, 97) == 1) {
            psram_spi_uninit(psram_spi, true /* fudge */);
            psram_spi = psram_spi_init_clkdiv(pio1, -1, psram_clkdiv /* clkdiv */, false /* fudge */);
            if (test_psram(&psram_spi, 97) == 1) { 
                printf("Error: No PSRAM strategies found to work!\n");
                err_blink();
            }
        }
#endif // TEST_PSRAM
    }
#endif // PSRAM
#endif // PSRAM_CORE0


#ifdef SOUND_SB
    puts("Initializing SoundBlaster DSP");
    // sbdsp_init();
#endif // SOUND_SB
#ifdef SOUND_OPL
    puts("Creating OPL");
    OPL_Pico_Init(0);
    multicore_launch_core1(&play_adlib);
#endif

#ifdef CDROM
    cdrom_global_init();
    mke_init();
#endif

#ifdef SOUND_GUS
    puts("Creating GUS");
    GUS_OnReset();
    multicore_launch_core1(&play_gus);
#endif // SOUND_GUS

#ifdef SOUND_MPU
#ifdef MPU_ONLY
    multicore_launch_core1(&play_mpu);
#endif // MPU_ONLY
#endif // SOUND_MPU

#if (SOUND_TANDY || SOUND_CMS)
    puts("Creating psgsound");
    multicore_launch_core1(&play_psg);
#endif // (SOUND_TANDY || SOUND_CMS)

#ifdef NE2000
extern void PIC_ActivateIRQ(void);
extern void PIC_DeActivateIRQ(void);

    puts("Creating NE2000");    
    multicore_launch_core1(&play_ne2000);
#endif

#ifdef USB_JOYSTICK
    // Init joystick as centered with no buttons pressed
    joystate_struct = {127, 127, 127, 127, 0xf};
    puts("Config joystick PWM");
    pwm_config pwm_c = pwm_get_default_config();
    // TODO better calibrate this
    pwm_config_set_clkdiv(&pwm_c, pwm_clkdiv);
    // Start the PWM off constantly looping at 0
    pwm_config_set_wrap(&pwm_c, 0);
    pwm_init(0, &pwm_c, true);
    pwm_init(1, &pwm_c, true);
    pwm_init(2, &pwm_c, true);
    pwm_init(3, &pwm_c, true);
#endif // USB_JOYSTICK
#ifdef USB_MOUSE
    puts("Config USB Mouse emulation");
    uartemu_init(0);
    sermouse_init(settings.Mouse.protocol, settings.Mouse.reportRate, settings.Mouse.sensitivity);
    sermouse_attach_uart();
#endif // USB_MOUSE
#ifdef USB_ONLY
    multicore_launch_core1(&play_usb);
#endif // USBONLY

    for(int i=AD0_PIN; i<(AD0_PIN + 10); ++i) {
        gpio_disable_pulls(i);
    }
    gpio_disable_pulls(IOW_PIN);
    gpio_disable_pulls(IOR_PIN);
    gpio_pull_down(IOCHRDY_PIN);
    gpio_set_dir(IOCHRDY_PIN, GPIO_OUT);

    puts("Enabling bus transceivers...");
    // waggle ADS to set BUSOE latch
    gpio_init(ADS_PIN);
    gpio_set_dir(ADS_PIN, GPIO_OUT);
    gpio_put(ADS_PIN, 1);
    busy_wait_ms(10);
    gpio_put(ADS_PIN, 0);

    puts("Starting ISA bus PIO...");
    // gpio_set_drive_strength(ADS_PIN, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(ADS_PIN, GPIO_SLEW_RATE_FAST);

    uint iow_offset = pio_add_program(pio0, &iow_program);
    pio_sm_claim(pio0, IOW_PIO_SM);
    printf("iow sm: %u\n", IOW_PIO_SM);

    uint ior_offset = pio_add_program(pio0, &ior_program);
    pio_sm_claim(pio0, IOR_PIO_SM);
    printf("ior sm: %u\n", IOR_PIO_SM);

    iow_program_init(pio0, IOW_PIO_SM, iow_offset, iow_clkdiv);
    ior_program_init(pio0, IOR_PIO_SM, ior_offset);

#ifdef USE_IRQ
    puts("Enabling IRQ on ISA IOR/IOW events");
    irq_set_enabled(PIO0_IRQ_1, false);
    pio_set_irq1_source_enabled(pio0, pio_get_rx_fifo_not_empty_interrupt_source(IOW_PIO_SM), true);
    pio_set_irq1_source_enabled(pio0, pio_get_rx_fifo_not_empty_interrupt_source(IOR_PIO_SM), true);
    irq_set_priority(PIO0_IRQ_1, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_exclusive_handler(PIO0_IRQ_1, io_isr);
    irq_set_enabled(PIO0_IRQ_1, true);
#endif

    gpio_xor_mask(LED_PIN);

#if AUDIO_CALLBACK_CORE0
    pwm_c = pwm_get_default_config();
    pwm_config_set_wrap(&pwm_c, clocks_per_sample_minus_one);
    pwm_init(pwm_slice_num, &pwm_c, false);
    pwm_set_irq_enabled(pwm_slice_num, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, audio_sample_handler);
    irq_set_priority(PWM_IRQ_WRAP, PICO_LOWEST_IRQ_PRIORITY);
    irq_set_enabled(PWM_IRQ_WRAP, true);
    pwm_set_enabled(pwm_slice_num, true);
#endif

    processSettings();

    for (;;) {
#ifndef USE_IRQ
        if (iow_has_data()) {
            handle_iow();
        }

        if (ior_has_data()) {
            handle_ior();
        }
#endif
#ifdef POLLING_DMA
        process_dma();
#endif
    }
}
