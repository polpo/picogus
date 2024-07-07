#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/adc.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "hardware/regs/vreg_and_chip_reset.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"

#include "pico_reflash.h"
#include "flash_settings.h"

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
#define BAUD_RATE   115200

uint LED_PIN;

#include "M62429/M62429.h"
M62429* m62429;

#ifdef SOUND_SB
static uint16_t sb_port_test;
extern void sbdsp_write(uint8_t address, uint8_t value);
extern uint8_t sbdsp_read(uint8_t address);
extern void sbdsp_init();
extern void sbdsp_process();
#endif
#ifdef SOUND_OPL
#include "opl.h"
void play_adlib(void);
extern "C" int OPL_Pico_Init(unsigned int);
extern "C" unsigned int OPL_Pico_PortRead(opl_port_t);
#include "cmd_buffers.h"
cms_buffer_t opl_buffer = { {0}, 0, 0 };
#endif

#ifdef SOUND_GUS
#include "gus-x.cpp"

#include "isa_dma.h"
dma_inst_t dma_config;
static uint16_t gus_port_test;
void play_gus(void);
#endif


#ifdef SOUND_MPU
#include "mpu401/export.h"
void play_mpu(void);
#endif

#ifdef SOUND_TANDY
#include "square/square.h"
void play_tandy(void);

#include "cmd_buffers.h"
tandy_buffer_t tandy_buffer = { {0}, 0, 0 };
#endif

#ifdef SOUND_CMS
void play_cms(void);
static uint8_t cms_detect = 0xFF;

#include "cmd_buffers.h"
cms_buffer_t cms_buffer = { {0}, 0, 0 };
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

// PicoGUS control and data ports
static bool control_active = false;
static uint8_t sel_reg = 0;
static uint16_t cur_data = 0;
static uint32_t cur_read = 0;
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
    case MODE_MAGIC: // Magic string
    case MODE_PROTOCOL: // Protocol version
        break;
    case MODE_FWSTRING: // Firmware string
        cur_read = 0;
        break;
    case MODE_BOOTMODE: // Mode (GUS, OPL, MPU, etc...)
        break;
    case MODE_GUSPORT: // GUS Base port
    case MODE_OPLPORT: // Adlib Base port
    case MODE_SBPORT: // SB Base port
    case MODE_MPUPORT: // MPU Base port
    case MODE_TANDYPORT: // Tandy Base port
    case MODE_CMSPORT: // CMS Base port
        basePort_low = 0;
        break;
    case MODE_JOYEN: // enable joystick
        break;
    case MODE_GUSBUF: // Audio buffer size
    case MODE_GUSDMA: // DMA interval
    case MODE_GUS44K: // Force 44k
        break;
    case MODE_WTVOL: // Wavetable mixer volume
        break;
    case MODE_MPUDELAY: // MPU sysex delay
    case MODE_MPUFAKE: // MPU fake all notes off
        break;
    case MODE_OPLWAIT: // Adlib speed sensitive fix
        break;
    case MODE_MOUSEPORT:
        basePort_low = 0;
        break;
    case MODE_MOUSEPROTO:
    case MODE_MOUSERATE:
    case MODE_MOUSESEN:
        break;
    case MODE_SAVE: // Select save settings register
    case MODE_REBOOT: // Select reboot register
    case MODE_DEFAULTS: // Select reset to defaults register
        break;
    case MODE_HWTYPE: // Hardware version
        break;
    case MODE_FLASH: // Firmware write mode
        pico_firmware_start();
        break;
    default:
        control_active = false;
        break;
    }
}

__force_inline void write_picogus_low(uint8_t value) {
    switch (sel_reg) {
    case MODE_GUSPORT: // GUS Base port
    case MODE_OPLPORT: // Adlib Base port
    case MODE_SBPORT: // SB Base port
    case MODE_MPUPORT: // MPU Base port
    case MODE_TANDYPORT: // Tandy Base port
    case MODE_CMSPORT: // CMS Base port
    case MODE_MOUSEPORT:  // USB Mouse port (0 - disabled)
        basePort_low = value;
        break;
    case MODE_MOUSESEN:  // USB Mouse Sensitivity (8.8 fixedpoint)
        mouseSensitivity_low = value;
        break;
    }
}

__force_inline void write_picogus_high(uint8_t value) {
    switch (sel_reg) {
    case MODE_GUSPORT: // GUS Base port
        settings.GUS.basePort = (value && basePort_low) ? ((value << 8) | (basePort_low & 0xFF)) : 0xFFFF;
#ifdef SOUND_GUS
        gus_port_test = settings.GUS.basePort >> 4 | 0x10;
#endif
        break;
    case MODE_OPLPORT: // Adlib Base port
        settings.SB.oplBasePort = (value && basePort_low) ? ((value << 8) | (basePort_low & 0xFF)) : 0xFFFF;
        break;
    case MODE_SBPORT: // SB Base port
        settings.SB.basePort = (value && basePort_low) ? ((value << 8) | (basePort_low & 0xFF)) : 0xFFFF;
#ifdef SOUND_SB
        sb_port_test = settings.SB.basePort >> 4;
#endif
        break;
    case MODE_MPUPORT: // MPU Base port
        settings.MPU.basePort = (value && basePort_low) ? ((value << 8) | (basePort_low & 0xFF)) : 0xFFFF;
        break;
    case MODE_TANDYPORT: // Tandy Base port
        settings.Tandy.basePort = (value && basePort_low) ? ((value << 8) | (basePort_low & 0xFF)) : 0xFFFF;
        break;
    case MODE_CMSPORT: // CMS Base port
        settings.CMS.basePort = (value && basePort_low) ? ((value << 8) | (basePort_low & 0xFF)) : 0xFFFF;
        break;
    case MODE_JOYEN: // enable joystick
        settings.Joy.basePort = value ? 0x201u : 0xffff;
        break;
    case MODE_GUSBUF: // GUS audio buffer size
        // Value is sent by pgusinit as the size - 1, so we need to add 1 back to it
        settings.GUS.audioBuffer = value + 1;
#ifdef SOUND_GUS
        GUS_SetAudioBuffer(settings.GUS.audioBuffer);
#endif
        break;
    case MODE_GUSDMA: // GUS DMA interval
        settings.GUS.dmaInterval = value;
#ifdef SOUND_GUS
        GUS_SetDMAInterval(settings.GUS.dmaInterval);
#endif
        break;
    case MODE_GUS44K: // Force 44k output
        settings.GUS.force44k = value;
#ifdef SOUND_GUS
        GUS_SetFixed44k(settings.GUS.force44k);
#endif
        break;
    case MODE_WTVOL: // Wavetable mixer volume
        settings.Global.waveTableVolume = value;
        if (BOARD_TYPE == PICOGUS_2) {
            m62429->setVolume(M62429_BOTH, settings.Global.waveTableVolume);
        }
        break;
    case MODE_MPUDELAY: // MPU SYSEX delay
        settings.MPU.delaySysex = value;
#ifdef SOUND_MPU
        MPU401_Init(settings.MPU.delaySysex, settings.MPU.fakeAllNotesOff);
#endif
        break;
    case MODE_MPUFAKE: // MPU fake all notes off
        settings.MPU.fakeAllNotesOff = value;
#ifdef SOUND_MPU
        MPU401_Init(settings.MPU.delaySysex, settings.MPU.fakeAllNotesOff);
#endif
        break;
    case MODE_OPLWAIT: // Adlib speed sensitive fix
        settings.SB.oplSpeedSensitive = value;
        break;
    case MODE_MOUSEPORT:  // USB Mouse port (0 - disabled)
        settings.Mouse.basePort = (value && basePort_low) ? ((value << 8) | (basePort_low & 0xFF)) : 0xFFFF;
        break;
    case MODE_MOUSEPROTO:  // USB Mouse protocol
        settings.Mouse.protocol = value;
#ifdef USB_MOUSE
        sermouse_set_protocol(settings.Mouse.protocol);
#endif
        break;
    case MODE_MOUSERATE:  // USB Mouse Report Rate
        settings.Mouse.reportRate = value;
#ifdef USB_MOUSE
        sermouse_set_report_rate_hz(settings.Mouse.reportRate);
#endif
        break;
    case MODE_MOUSESEN:  // USB Mouse Sensitivity (8.8 fixedpoint)
        settings.Mouse.sensitivity = (value << 8) | (mouseSensitivity_low & 0xFF);
#ifdef USB_MOUSE
        sermouse_set_sensitivity(settings.Mouse.sensitivity);
#endif
        break;
    // For multifw
    case MODE_BOOTMODE:
        settings.startupMode = value;
        printf("requesting startup mode: %u\n", value);
        break;
    case MODE_SAVE:
        queueSaveSettings = true;
        break;
    case MODE_REBOOT:
        watchdog_hw->scratch[3] = settings.startupMode;
        printf("rebooting into mode: %u\n", settings.startupMode);
        watchdog_reboot(0, 0, 0);
        break;
    case MODE_DEFAULTS:
        getDefaultSettings(&settings);
        processSettings();
        break;
    case MODE_FLASH: // Firmware write
        pico_firmware_write(value);
        break;
    }
}

__force_inline uint8_t read_picogus_low(void) {
    switch (sel_reg) {
    case MODE_GUSPORT: // GUS Base port
        return settings.GUS.basePort == 0xFFFF ? 0 : (settings.GUS.basePort & 0xFF);
    case MODE_OPLPORT: // Adlib Base port
        return settings.SB.oplBasePort == 0xFFFF ? 0 : (settings.SB.oplBasePort & 0xFF);
    case MODE_SBPORT: // SB Base port
        return settings.SB.basePort == 0xFFFF ? 0 : (settings.SB.basePort & 0xFF);
    case MODE_MPUPORT: // MPU Base port
        return settings.MPU.basePort == 0xFFFF ? 0 : (settings.MPU.basePort & 0xFF);
    case MODE_TANDYPORT: // Tandy Base port
        return settings.Tandy.basePort == 0xFFFF ? 0 : (settings.Tandy.basePort & 0xFF);
    case MODE_CMSPORT: // CMS Base port
        return settings.CMS.basePort == 0xFFFF ? 0 : (settings.CMS.basePort & 0xFF);
    case MODE_MOUSEPORT:  // USB Mouse port (0 - disabled)
        return settings.Mouse.basePort == 0xFFFF ? 0 : (settings.Mouse.basePort & 0xFF);
    case MODE_MOUSESEN:  // USB Mouse Sensitivity (8.8 fixedpoint)
        return settings.Mouse.sensitivity & 0xFF;
    default:
        return 0x0;
    }
}

__force_inline uint8_t read_picogus_high(void) {
    uint8_t ret;
    switch (sel_reg) {
    case MODE_MAGIC:  // PicoGUS magic string
        return 0xdd;
    case MODE_PROTOCOL:  // PicoGUS protocol version
        return PICOGUS_PROTOCOL_VER;
    case MODE_FWSTRING: // Firmware string
        ret = firmware_string[cur_read++];
        if (ret == 0) { // Null terminated
            cur_read = 0;
        }
        return ret;
    case MODE_BOOTMODE: // Mode (GUS, OPL, MPU, etc...)
        return settings.startupMode;
    case MODE_GUSPORT: // GUS Base port
        return settings.GUS.basePort >> 8;
    case MODE_OPLPORT: // Adlib Base port
        return settings.SB.oplBasePort >> 8;
    case MODE_SBPORT: // SB Base port
        return settings.SB.basePort >> 8;
    case MODE_MPUPORT: // MPU Base port
        return settings.MPU.basePort >> 8;
    case MODE_TANDYPORT: // Tandy Base port
        return settings.Tandy.basePort >> 8;
    case MODE_CMSPORT: // CMS Base port
        return settings.CMS.basePort >> 8;
    case MODE_JOYEN: // enable joystick
        return settings.Joy.basePort == 0x201u;
    case MODE_GUSBUF: // GUS audio buffer size
        return settings.GUS.audioBuffer - 1;
    case MODE_GUSDMA: // GUS DMA interval
        return settings.GUS.dmaInterval;
    case MODE_GUS44K: // Force 44k output
        return settings.GUS.force44k;
    case MODE_WTVOL: // Wavetable mixer volume
        return (BOARD_TYPE == PICOGUS_2) ? m62429->getVolume(0) : 0;
    case MODE_MPUDELAY: // SYSEX delay
        return settings.MPU.delaySysex;
    case MODE_MPUFAKE: // MPU fake all notes off
        return settings.MPU.fakeAllNotesOff;
    case MODE_OPLWAIT: // Adlib speed sensitive fix
        return settings.SB.oplSpeedSensitive;
    case MODE_MOUSEPORT:  // USB Mouse port (0 - disabled)
        return settings.Mouse.basePort == 0xFFFF ? 0 : (settings.Mouse.basePort >> 8);
    case MODE_MOUSEPROTO:  // USB Mouse protocol
        return settings.Mouse.protocol;
    case MODE_MOUSERATE:  // USB Mouse Report Rate
        return settings.Mouse.reportRate;
    case MODE_MOUSESEN:  // USB Mouse Sensitivity (8.8 fixedpoint)
        return settings.Mouse.sensitivity >> 8;
    case MODE_HWTYPE: // Hardware version
        return BOARD_TYPE;
    case MODE_FLASH:
        // Get status of firmware write
        return pico_firmware_getStatus();
    default:
        return 0xff;
    }
}


void processSettings(void) {
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
            opl_buffer.cmds[opl_buffer.head++] = {
                OPL_REGISTER_PORT,
                (uint8_t)(iow_read & 0xFF)
            };
            // Fast write - return early as we've already written 0x0u to the PIO
            return;
            break;
        case 0x9:
            pio_sm_put(pio0, IOW_PIO_SM, IO_WAIT);
            opl_buffer.cmds[opl_buffer.head++] = {
                OPL_DATA_PORT,
                (uint8_t)(iow_read & 0xFF)
            };
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
#if defined(SOUND_OPL)
    if (port == settings.SB.oplBasePort) {
        // Fast write
        pio_sm_put(pio0, IOW_PIO_SM, IO_END);
        opl_buffer.cmds[opl_buffer.head++] = {
            OPL_REGISTER_PORT,
            (uint8_t)(iow_read & 0xFF)
        };
        // Fast write - return early as we've already written 0x0u to the PIO
        return;
    } else if (port == settings.SB.oplBasePort + 1) {
        pio_sm_put(pio0, IOW_PIO_SM, IO_WAIT);
        if (settings.SB.oplSpeedSensitive) {
            busy_wait_us(1); // busy wait for speed sensitive games
        }
        opl_buffer.cmds[opl_buffer.head++] = {
            OPL_DATA_PORT,
            (uint8_t)(iow_read & 0xFF)
        };
    } else // if follows down below
#endif // SOUND_OPL
#ifdef SOUND_MPU
    switch (port - settings.MPU.basePort) {
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
#endif // SOUND_MPU
#ifdef SOUND_TANDY
    if (port == settings.Tandy.basePort) {
        pio_sm_put(pio0, IOW_PIO_SM, IO_END);
        tandy_buffer.cmds[tandy_buffer.head++] = iow_read & 0xFF;
        return;
    } else // if follows down below
#endif // SOUND_TANDY
#ifdef SOUND_CMS
    switch (port - settings.CMS.basePort) {
    // SAA data/address ports
    case 0x0:
    case 0x1:
    case 0x2:
    case 0x3:
        pio_sm_put(pio0, IOW_PIO_SM, IO_END);
        cms_buffer.cmds[cms_buffer.head++] = {
            port,
            (uint8_t)(iow_read & 0xFF)
        };
        return;
        break;
    // CMS autodetect ports
    case 0x6:
    case 0x7:
        pio_sm_put(pio0, IOW_PIO_SM, IO_END);
        cms_detect = iow_read & 0xFF;
        return;
        break;
    }
#endif // SOUND_CMS
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
        uint32_t value = read_gus(port - settings.GUS.basePort) & 0xff;
        // OR with 0x0000ff00 is required to set pindirs in the PIO
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | value);
        // printf("GUS IOR: port: %x value: %x\n", port, value);
        // gpio_xor_mask(LED_PIN);
    } else // if follows down below
#endif
#if defined(SOUND_SB)
    if ((port >> 4) == sb_port_test) {
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        if (port - settings.SB.basePort == 0x8) {
            // wait for OPL buffer to process
            while (opl_buffer.tail != opl_buffer.head) {
                tight_loop_contents();
            }
            pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | OPL_Pico_PortRead(OPL_REGISTER_PORT));
        } else {
            sbdsp_process();
            pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | sbdsp_read(port & 0xF));        
            sbdsp_process();
        }
    } else // if follows down below
#endif
#if defined(SOUND_OPL)
    if (port == settings.SB.oplBasePort) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        // wait for OPL buffer to process
        while (opl_buffer.tail != opl_buffer.head) {
            tight_loop_contents();
        }
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | OPL_Pico_PortRead(OPL_REGISTER_PORT));
    } else // if follows down below
#endif
#if defined(SOUND_MPU)
    if (port == settings.MPU.basePort) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        uint32_t value = MPU401_ReadData();
        // printf("MPU IOR: port: %x value: %x\n", port, value);
        // OR with 0x0000ff00 is required to set pindirs in the PIO
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | value);
    } else if (port == settings.MPU.basePort + 1) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        uint32_t value = MPU401_ReadStatus();
        // printf("MPU IOR: port: %x value: %x\n", port, value);
        // OR with 0x0000ff00 is required to set pindirs in the PIO
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | value);
    } else // if follows down below
#endif
#if defined(SOUND_CMS)
    switch (port - settings.CMS.basePort) {
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
    }
#endif // SOUND_CMS
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
        uint8_t value = uartemu_read(port & 7);
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | value);
        return;
    } else // if follows down below
#endif // USB_MOUSE
    if (port == CONTROL_PORT) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        uint32_t value = sel_reg;
        // OR with 0x0000ff00 is required to set pindirs in the PIO
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | value);
    } else if (port == DATA_PORT_LOW) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        uint32_t value = read_picogus_low();
        // OR with 0x0000ff00 is required to set pindirs in the PIO
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | value);
    } else if (port == DATA_PORT_HIGH) {
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        uint32_t value = read_picogus_high();
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | value);
    } else {
        // Reset PIO
        pio_sm_put(pio0, IOR_PIO_SM, IO_END);
    }
}

#ifdef USE_IRQ
void iow_isr(void) {
    /* //printf("ints %x\n", pio0->ints0); */
    handle_iow();
    // pio_interrupt_clear(pio0, pio_intr_sm0_rxnempty_lsb);
    irq_clear(PIO0_IRQ_0);
}
void ior_isr(void) {
    handle_ior();
    // pio_interrupt_clear(pio0, PIO_INTR_SM0_RXNEMPTY_LSB);
    irq_clear(PIO0_IRQ_1);
}
#endif

void err_blink(void) {
    for (;;) {
        gpio_xor_mask(LED_PIN);
        busy_wait_ms(100);
    }
}

#ifndef USE_ALARM
#include "pico_pic.h"
#endif

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

#include "hardware/structs/xip_ctrl.h"
int main()
{
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
    loadSettings(&settings);
#if defined(SOUND_GUS)
    settings.startupMode = GUS_MODE;
#elif defined(SOUND_MPU)
    settings.startupMode = MPU_MODE;
#elif defined(SOUND_TANDY)
    settings.startupMode = TANDY_MODE;
#elif defined(SOUND_CMS)
    settings.startupMode = CMS_MODE;
#elif defined(SOUND_SB)
    settings.startupMode = SB_MODE;
#elif defined(SOUND_OPL)
    settings.startupMode = ADLIB_MODE;
#elif defined(USB_ONLY)
    settings.startupMode = USB_MODE;
#else
    settings.startupMode = INVALID_MODE;
#endif
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
    printf("Waiting for board to stabilize... ");
    busy_wait_ms(250);
    // Overclock!
    printf("Overclocking... ");
    vreg_set_voltage(VREG_VOLTAGE_1_25);
    // vreg_set_voltage(VREG_VOLTAGE_1_15);
    busy_wait_ms(250);
    set_sys_clock_khz(rp2_clock, true);
    busy_wait_ms(250);
    gpio_xor_mask(LED_PIN);
#ifdef ASYNC_UART
    uart_init(UART_ID, 0);
#else
    stdio_init_all();
#endif
    puts("Done. Continuing!");

    // Set clk_peri to use the XOSC
    // clock_configure(clk_peri,
    //                 0,
    //                 CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_XOSC_CLKSRC,
    //                 12 * MHZ,
    //                 12 * MHZ);
    // clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
    //         12 * MHZ, 12 * MHZ);

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
    sbdsp_init();
#endif // SOUND_SB
#ifdef SOUND_OPL
    puts("Creating OPL");
    OPL_Pico_Init(0);
    multicore_launch_core1(&play_adlib);
#endif

#ifdef SOUND_GUS
    puts("Creating GUS");
    GUS_OnReset();
    multicore_launch_core1(&play_gus);
#endif // SOUND_GUS

#ifdef SOUND_MPU
    multicore_launch_core1(&play_mpu);
#endif // SOUND_MPU

#ifdef SOUND_TANDY
    puts("Creating tandysound");
    multicore_launch_core1(&play_tandy);
#endif // SOUND_TANDY

#ifdef SOUND_CMS
    puts("Creating CMS");
    multicore_launch_core1(&play_cms);
#endif // SOUND_CMS

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
    // iow irq
    irq_set_enabled(PIO0_IRQ_0, false);
    pio_set_irq0_source_enabled(pio0, pis_sm0_rx_fifo_not_empty, true);
    irq_set_priority(PIO0_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_exclusive_handler(PIO0_IRQ_0, iow_isr);
    irq_set_enabled(PIO0_IRQ_0, true);
    // ior irq
    irq_set_enabled(PIO0_IRQ_1, false);
    pio_set_irq1_source_enabled(pio0, pis_sm1_rx_fifo_not_empty, true);
    irq_set_priority(PIO0_IRQ_1, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_exclusive_handler(PIO0_IRQ_1, ior_isr);
    irq_set_enabled(PIO0_IRQ_1, true);
#endif

    gpio_xor_mask(LED_PIN);

#ifndef USE_ALARM
    PIC_Init();
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
#ifndef USE_ALARM
        PIC_HandleEvents();
#endif
#ifdef POLLING_DMA
        process_dma();
#endif
    }
}
