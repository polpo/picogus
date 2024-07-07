#pragma once

// names, mode numbers, port numbers, etc., that are common between the PicoGUS firmware and pgusinit.exe

// 1D0 chosen as the base port as nothing is listed in Ralf Brown's Port List (http://www.cs.cmu.edu/~ralf/files.html)
#define CONTROL_PORT 0x1D0
#define DATA_PORT_LOW  0x1D1
#define DATA_PORT_HIGH 0x1D2
#define PICOGUS_PROTOCOL_VER 3

typedef enum {
    PICO_FIRMWARE_IDLE = 0,
    PICO_FIRMWARE_WRITING = 1,
    PICO_FIRMWARE_BUSY = 2,
    PICO_FIRMWARE_DONE = 0xFE,
    PICO_FIRMWARE_ERROR = 0xFF
} pico_firmware_status_t;

typedef enum { PICO_BASED = 0, PICOGUS_2 = 1 } board_type_t;

typedef enum {
    INVALID_MODE = 0,
    GUS_MODE     = 1,
    ADLIB_MODE   = 2,
    MPU_MODE     = 3,
    TANDY_MODE   = 4,
    CMS_MODE     = 5,
    SB_MODE      = 6,
    USB_MODE     = 7
} card_mode_t;

static const char *modenames[8] = {
    "INVALID",
    "GUS",
    "ADLIB",
    "MPU",
    "TANDY",
    "CMS",
    "SB",
    "USB"
};

#define MODE_MAGIC      0x00 // Magic string
#define MODE_PROTOCOL   0x01 // Protocol version
#define MODE_FWSTRING   0x02 // Firmware string
#define MODE_BOOTMODE   0x03 // Mode (GUS, OPL, MPU, etc...)
#define MODE_GUSPORT    0x04 // GUS Base port
#define MODE_OPLPORT    0x05 // Adlib Base port
#define MODE_SBPORT     0x06 // SB Base port
#define MODE_MPUPORT    0x07 // MPU Base port
#define MODE_TANDYPORT  0x08 // Tandy Base port
#define MODE_CMSPORT    0x09 // CMS Base port
#define MODE_JOYEN      0x0f // enable joystick
#define MODE_GUSBUF     0x10 // Audio buffer size
#define MODE_GUSDMA     0x11 // DMA interval
#define MODE_GUS44K     0x12 // Force 44k
#define MODE_WTVOL      0x20 // Wavetable mixer volume
#define MODE_MPUDELAY   0x21 // MPU sysex delay
#define MODE_MPUFAKE    0x22 // MPU fake all notes off
#define MODE_OPLWAIT    0x30 // Adlib speed sensitive fix
#define MODE_MOUSEPORT  0x40 // Mouse Base port
#define MODE_MOUSEPROTO 0x41 // Mouse protocol
#define MODE_MOUSERATE  0x42 // Mouse report rate
#define MODE_MOUSESEN   0x43 // Mouse sensitivity
#define MODE_DEFAULTS   0xE0 // Select reset to defaults register
#define MODE_SAVE       0xE1 // Select save settings register
#define MODE_REBOOT     0xE2 // Select reboot register
#define MODE_HWTYPE     0xF0 // Hardware type 
#define MODE_FLASH      0xFF // Firmware write mode
