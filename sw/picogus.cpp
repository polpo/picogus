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

enum board_type { PICO_BASED = 0, PICOGUS_2 = 1 } BOARD_TYPE;

typedef enum {
    GUS_MODE = 0,
    ADLIB_MODE = 1, // deprecated
    MPU_MODE = 2,
    TANDY_MODE = 3,
    CMS_MODE = 4,
    SB_MODE = 5,
    JOYSTICK_ONLY_MODE = 0x0f
} card_mode_t;

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
#include "opl.h"
static uint16_t basePort;
static uint16_t sb_port_test;

void play_adlib(void);
extern "C" int OPL_Pico_Init(unsigned int);
extern "C" unsigned int OPL_Pico_PortRead(opl_port_t);
#include "cmd_buffers.h"
cms_buffer_t opl_buffer = { {0}, 0, 0 };

extern void sbdsp_write(uint8_t address, uint8_t value);
extern uint8_t sbdsp_read(uint8_t address);
extern void sbdsp_init();
extern void sbdsp_process();
static uint16_t adlib_basePort;
static bool adlib_wait;
#endif

#ifdef SOUND_GUS
#include "gus-x.cpp"

#include "isa_dma.h"
dma_inst_t dma_config;
static uint16_t basePort;
static uint16_t gus_port_test;
void play_gus(void);
#endif


#ifdef SOUND_MPU
#include "mpu401/export.h"
static uint16_t basePort;
void play_mpu(void);
#endif

#ifdef SOUND_TANDY
#include "square/square.h"
static uint16_t basePort;
void play_tandy(void);

#include "cmd_buffers.h"
tandy_buffer_t tandy_buffer = { {0}, 0, 0 };
#endif

#ifdef SOUND_CMS
static uint16_t basePort;
void play_cms(void);
static uint8_t cms_detect = 0xFF;

#include "cmd_buffers.h"
cms_buffer_t cms_buffer = { {0}, 0, 0 };
#endif

#ifdef USB_JOYSTICK
static uint16_t joyPort;
#ifdef USB_JOYSTICK_ONLY
void play_usb(void);
#endif
#include "joy_hid/joy.h"
extern "C" joystate_struct_t joystate_struct;
uint8_t joystate_bin;
#include "hardware/pwm.h"
#endif

// PicoGUS control and data ports
// 1D0 chosen as the base port as nothing is listed in Ralf Brown's Port List (http://www.cs.cmu.edu/~ralf/files.html)
#define CONTROL_PORT 0x1D0
#define DATA_PORT_LOW  0x1D1
#define DATA_PORT_HIGH 0x1D2
#define PICOGUS_PROTOCOL_VER 2
static bool control_active = false;
static uint8_t sel_reg = 0;
static uint16_t cur_data = 0;
static uint32_t cur_read = 0;
static bool queueSaveSettings = false;
static bool queueReboot = false;

Settings settings;

#define IOW_PIO_SM 0
#define IOR_PIO_SM 1

const char* firmware_string = PICO_PROGRAM_NAME " v" PICO_PROGRAM_VERSION_STRING;

uint16_t basePort_tmp;
uint16_t multifw_tmp;

__force_inline void select_picogus(uint8_t value) {
    // printf("select picogus %x\n", value);
    sel_reg = value;
    switch (sel_reg) {
    case 0x00: // Magic string
    case 0x01: // Protocol version
        break;
    case 0x02: // Firmware string
        cur_read = 0;
        break;
    case 0x03: // Mode (GUS, OPL, MPU, etc...)
        break;
    case 0x04: // Base port
    case 0x05: // Adlib Base port
        basePort_tmp = 0;
        break;
    case 0x0f: // enable joystick
        break;
#ifdef SOUND_GUS
    case 0x10: // Audio buffer size
    case 0x11: // DMA interval
    case 0x12: // Force 44k
        break;
#endif
    case 0x20: // Wavetable mixer volume
        break;
#ifdef SOUND_MPU
    case 0x21: // MPU init
        break;
#endif
#ifdef SOUND_SB
    case 0x30: // Adlib speed sensitive fix
        break;
#endif
    case 0xE0: // Select firmware boot mode register
    case 0xE1: // Select save settings register
    case 0xE2: // Select reboot register
        break;
    case 0xF0: // Hardware version
        break;
    case 0xFF: // Firmware write mode
        pico_firmware_start();
        break;
    default:
        control_active = false;
        break;
    }
}

__force_inline void write_picogus_low(uint8_t value) {
    switch (sel_reg) {
    case 0x04: // Base port
    case 0x05: // Adlib Base port
        basePort_tmp = value;
        break;
    }
}

__force_inline void write_picogus_high(uint8_t value) {
    switch (sel_reg) {
    case 0x04: // Base port
#if defined(SOUND_GUS) || defined(SOUND_SB) || defined(SOUND_MPU) || defined(SOUND_TANDY) || defined(SOUND_CMS)
        basePort = (value << 8) | basePort_tmp;
#endif
#ifdef SOUND_GUS
        settings.GUS.basePort = basePort;
        gus_port_test = basePort >> 4 | 0x10;
        // GUS_SetPort(basePort);
#endif
#ifdef SOUND_SB
        settings.SB.basePort = basePort;
        sb_port_test = basePort >> 4;
#endif
#ifdef SOUND_MPU
        settings.MPU.basePort = basePort;
#endif
#ifdef SOUND_TANDY
        settings.Tandy.basePort = basePort;
#endif
#ifdef SOUND_CMS
        settings.CMS.basePort = basePort;
#endif
        break;
    case 0x05: // Adlib Base port
#ifdef SOUND_SB
        adlib_basePort = (value << 8) | basePort_tmp;
        settings.SB.oplBasePort = adlib_basePort;
#endif
        break;
    case 0x0f: // enable joystick
#ifdef USB_JOYSTICK
        joyPort = value ? 0x201u : 0xffff;
        settings.Joy.basePort = joyPort;
#endif
        break;
#ifdef SOUND_GUS
    case 0x10: // Audio buffer size
        // Value is sent by pgusinit as the size - 1, so we need to add 1 back to it
        GUS_SetAudioBuffer(value + 1);
        settings.GUS.audioBuffer = value + 1;
        break;
    case 0x11: // DMA interval
        GUS_SetDMAInterval(value);
        settings.GUS.dmaInterval = value;
        break;
    case 0x12: // Force 44k output
        GUS_SetFixed44k(value);
        settings.GUS.force44k = value;
        break;
#endif
    case 0x20: // Wavetable mixer volume
        if (BOARD_TYPE == PICOGUS_2) {
            m62429->setVolume(M62429_BOTH, value);
            settings.Global.waveTableVolume = value;
        }
        break;
#ifdef SOUND_MPU
    case 0x21: // MIDI emulation flags
        MPU401_Init(value & 0x1 /* delaysysex */, value & 0x2 /* fakeallnotesoff */);
        settings.MPU.delaySysex = value & 0x1;
        settings.MPU.fakeAllNotesOff = value & 0x2;
        break;
#endif
#ifdef SOUND_SB
    case 0x30: // Adlib speed sensitive fix
        adlib_wait = value;
        break;
#endif
    // For multifw
    case 0xE0:
        // set firmware num, perm flag and reboot
        settings.startupMode = value;
        printf("requesting startup mode: %u\n", value);
        break;
    case 0xE1:
        queueSaveSettings = true;
        break;
    case 0xE2:
        watchdog_hw->scratch[3] = settings.startupMode;
        printf("rebooting into mode: %u\n", settings.startupMode);
        watchdog_reboot(0, 0, 0);
    case 0xff: // Firmware write
        pico_firmware_write(value);
        break;
    }
}

__force_inline uint8_t read_picogus_low(void) {
    switch (sel_reg) {
    case 0x04: // Base port
#if defined(SOUND_GUS) || defined(SOUND_SB) || defined(SOUND_MPU) || defined(SOUND_TANDY) || defined(SOUND_CMS)
        return basePort & 0xff;
#else
        return 0xff;
#endif
        break;
    case 0x05: // Adlib Base port
#if defined(SOUND_SB)
        return adlib_basePort & 0xff;
#else
        return 0xff;
#endif
    default:
        return 0x0;
    }
}

__force_inline uint8_t read_picogus_high(void) {
    uint8_t ret;
    switch (sel_reg) {
    case 0x00:  // PicoGUS magic string
        return 0xdd;
        break;
    case 0x01:  // PicoGUS protocol version
        return PICOGUS_PROTOCOL_VER;
        break;
    case 0x02: // Firmware string
        ret = firmware_string[cur_read++];
        if (ret == 0) { // Null terminated
            cur_read = 0;
        }
        return ret;
        break;
    case 0x03: // Mode (GUS, OPL, MPU, etc...)
#if defined(SOUND_GUS)
        return GUS_MODE;
#elif defined(SOUND_MPU)
        return MPU_MODE;
#elif defined(SOUND_TANDY)
        return TANDY_MODE;
#elif defined(SOUND_CMS)
        return CMS_MODE;
#elif defined(SOUND_SB)
        return SB_MODE;
#elif defined(USB_JOYSTICK_ONLY)
        return JOYSTICK_ONLY_MODE;
#else
        return 0xff;
#endif
        break;
    case 0x04: // Base port
#if defined(SOUND_GUS) || defined(SOUND_SB) || defined(SOUND_MPU) || defined(SOUND_TANDY) || defined(SOUND_CMS)
        return basePort >> 8;
#else
        return 0xff;
#endif
        break;
    case 0x05: // Adlib Base port
#if defined(SOUND_SB)
        return adlib_basePort >> 8;
#else
        return 0xff;
#endif
        break;
    case 0x0f: // enable joystick
#ifdef USB_JOYSTICK
        return joyPort == 0x201u;
#else
        return 0;
#endif
    case 0x20: // Wavetable mixer volume
        if (BOARD_TYPE == PICOGUS_2) {
            return m62429->getVolume(0);
        } else {
            return 0;
        }
        break;
    case 0xF0: // Hardware version
        return BOARD_TYPE;
    case 0xff:
        // Get status of firmware write
        return pico_firmware_getStatus();
        break;
    default:
        return 0xff;
        break;
    }
}


void processSettings(void) {
#ifdef SOUND_SB
    basePort = settings.SB.basePort;
    sb_port_test = basePort >> 4;
    adlib_basePort = settings.SB.oplBasePort;
    adlib_wait = settings.SB.oplSpeedSensitive;
#endif
#ifdef SOUND_GUS
    basePort = settings.GUS.basePort;
    gus_port_test = basePort >> 4 | 0x10;
#endif
#ifdef SOUND_MPU
    basePort = settings.MPU.basePort;
#endif
#ifdef SOUND_CMS
    basePort = settings.CMS.basePort;
#endif
#ifdef SOUND_TANDY
    basePort = settings.Tandy.basePort;
#endif
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
        port -= basePort;
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
        switch (port - basePort) {
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
    } else if (port == adlib_basePort) {
        // Fast write
        pio_sm_put(pio0, IOW_PIO_SM, IO_END);
        opl_buffer.cmds[opl_buffer.head++] = {
            OPL_REGISTER_PORT,
            (uint8_t)(iow_read & 0xFF)
        };
        // Fast write - return early as we've already written 0x0u to the PIO
        return;
    } else if (port == adlib_basePort + 1) {
        pio_sm_put(pio0, IOW_PIO_SM, IO_WAIT);
        if (adlib_wait) {
            busy_wait_us(1); // busy wait for speed sensitive games
        }
        opl_buffer.cmds[opl_buffer.head++] = {
            OPL_DATA_PORT,
            (uint8_t)(iow_read & 0xFF)
        };
    } else // if follows down below
#endif // SOUND_SB
#ifdef SOUND_MPU
    switch (port - basePort) {
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
    if (port == basePort) {
        pio_sm_put(pio0, IOW_PIO_SM, IO_END);
        tandy_buffer.cmds[tandy_buffer.head++] = iow_read & 0xFF;
        return;
    } else // if follows down below
#endif // SOUND_TANDY
#ifdef SOUND_CMS
    switch (port - basePort) {
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
    if (port == joyPort) {
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
#endif
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
        uint32_t value = read_gus(port - basePort) & 0xff;
        // OR with 0x0000ff00 is required to set pindirs in the PIO
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | value);
        // printf("GUS IOR: port: %x value: %x\n", port, value);
        // gpio_xor_mask(LED_PIN);
    } else // if follows down below
#elif defined(SOUND_SB)
    if ((port >> 4) == sb_port_test) {
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        if (port - basePort == 0x8) {
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
    } else if (port == adlib_basePort) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        // wait for OPL buffer to process
        while (opl_buffer.tail != opl_buffer.head) {
            tight_loop_contents();
        }
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | OPL_Pico_PortRead(OPL_REGISTER_PORT));
    } else // if follows down below
#elif defined(SOUND_MPU)
    if (port == basePort) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        uint32_t value = MPU401_ReadData();
        // printf("MPU IOR: port: %x value: %x\n", port, value);
        // OR with 0x0000ff00 is required to set pindirs in the PIO
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | value);
    } else if (port == basePort + 1) {
        // Tell PIO to wait for data
        pio_sm_put(pio0, IOR_PIO_SM, IO_WAIT);
        uint32_t value = MPU401_ReadStatus();
        // printf("MPU IOR: port: %x value: %x\n", port, value);
        // OR with 0x0000ff00 is required to set pindirs in the PIO
        pio_sm_put(pio0, IOR_PIO_SM, IOR_SET_VALUE | value);
    } else // if follows down below
#elif defined(SOUND_CMS)
    switch (port - basePort) {
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
    if (port == joyPort) {
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
#endif
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
    hw_clear_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_EN_BITS);
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
    processSettings();

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
        m62429->setVolume(M62429_BOTH, settings.Global.waveTableVolume);
#endif // SOUND_MPU
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
    puts("Creating OPL");
    OPL_Pico_Init(basePort);
    multicore_launch_core1(&play_adlib);
#endif // SOUND_SB

#ifdef SOUND_GUS
    puts("Creating GUS");
    GUS_OnReset();
    GUS_SetAudioBuffer(settings.GUS.audioBuffer);
    GUS_SetDMAInterval(settings.GUS.dmaInterval);
    GUS_SetFixed44k(settings.GUS.force44k);
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
#ifdef USB_JOYSTICK_ONLY
    multicore_launch_core1(&play_usb);
#endif // USB_JOYSTICK_ONLY
#endif // USB_JOYSTICK

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

    for (;;) {
#ifndef USE_IRQ
        // if (!pio_sm_is_rx_fifo_empty(pio0, IOW_PIO_SM)) {
        if (iow_has_data()) {
            handle_iow();
            // gpio_xor_mask(LED_PIN);
        }

        // if (!pio_sm_is_rx_fifo_empty(pio0, IOR_PIO_SM)) {
        if (ior_has_data()) {
            handle_ior();
            // gpio_xor_mask(LED_PIN);
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
