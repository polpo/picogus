#pragma once

// names, mode numbers, port numbers, etc., that are common between the PicoGUS firmware and pgusinit.exe

// 1D0 chosen as the base port as nothing is listed in Ralf Brown's Port List (http://www.cs.cmu.edu/~ralf/files.html)
#define CONTROL_PORT 0x1D0
#define DATA_PORT_LOW  0x1D1
#define DATA_PORT_HIGH 0x1D2
#define PICOGUS_PROTOCOL_VER 4

typedef enum {
    PICO_FIRMWARE_IDLE = 0,
    PICO_FIRMWARE_WRITING = 1,
    PICO_FIRMWARE_BUSY = 2,
    PICO_FIRMWARE_DONE = 0xFE,
    PICO_FIRMWARE_ERROR = 0xFF
} pico_firmware_status_t;

typedef enum {
    CD_STATUS_ERROR = -1,
    CD_STATUS_IDLE,
    CD_STATUS_BUSY,
    CD_STATUS_READY,
} cdrom_image_status_t;

typedef enum { PICO_BASED = 0, PICOGUS_2 = 1 } board_type_t;

typedef enum {
    INVALID_MODE = 0,
    GUS_MODE     = 1,
    ADLIB_MODE   = 2,
    MPU_MODE     = 3,
    PSG_MODE     = 4,
    SB_MODE      = 5,
    USB_MODE     = 6,
    NE2000_MODE  = 7
} card_mode_t;

static const char *modenames[8] = {
    "INVALID",
    "GUS",
    "ADLIB",
    "MPU",
    "PSG",
    "SB",
    "USB",
    "NE2000"
};

#define CMD_MAGIC      0x00 // Magic string
#define CMD_PROTOCOL   0x01 // Protocol version
#define CMD_FWSTRING   0x02 // Firmware string
#define CMD_BOOTMODE   0x03 // Mode (GUS, OPL, MPU, etc...)
#define CMD_GUSPORT    0x04 // GUS Base port
#define CMD_OPLPORT    0x05 // Adlib Base port
#define CMD_SBPORT     0x06 // SB Base port
#define CMD_MPUPORT    0x07 // MPU Base port
#define CMD_TANDYPORT  0x08 // Tandy Base port
#define CMD_CMSPORT    0x09 // CMS Base port
#define CMD_JOYEN      0x0f // enable joystick

#define CMD_GUSBUF     0x10 // Audio buffer size
#define CMD_GUSDMA     0x11 // DMA interval
#define CMD_GUS44K     0x12 // Force 44k

#define CMD_WTVOL      0x20 // Wavetable mixer volume
#define CMD_MPUDELAY   0x21 // MPU sysex delay
#define CMD_MPUFAKE    0x22 // MPU fake all notes off

#define CMD_OPLWAIT    0x30 // Adlib speed sensitive fix

#define CMD_MOUSEPORT  0x40 // Mouse Base port
#define CMD_MOUSEPROTO 0x41 // Mouse protocol
#define CMD_MOUSERATE  0x42 // Mouse report rate
#define CMD_MOUSESEN   0x43 // Mouse sensitivity

#define CMD_NE2KPORT   0x50 // NE2000 Base port
#define CMD_WIFISSID   0x51 // WiFi SSID
#define CMD_WIFIPASS   0x52 // WiFi password
#define CMD_WIFIAPPLY  0x53 // apply WiFi settings
#define CMD_WIFISTAT   0x54 // WiFi status

#define CMD_CDPORT     0x60 // CD base port
#define CMD_CDSTATUS   0x61 // Get CD image command status
#define CMD_CDERROR    0x62 // Get CD image error
#define CMD_CDLIST     0x63 // List CD images
#define CMD_CDLOAD     0x64 // Load CD image or get loaded image index
#define CMD_CDNAME     0x65 // Get name of loaded CD image
#define CMD_CDAUTOADV  0x66 // Set autoadvance for CD image on USB reinsert

#define CMD_MAINVOL    0x70 // Main Volume
#define CMD_OPLVOL     0x71 // Adlib volume
#define CMD_SBVOL      0x72 // Sound Blaster volume
#define CMD_CDVOL      0x73 // CD Audio Volume
#define CMD_GUSVOL     0x74 // GUS Volume
#define CMD_PSGVOL     0x75 // PSG Volume

#define CMD_DEFAULTS   0xE0 // Select reset to defaults register
#define CMD_SAVE       0xE1 // Select save settings register
#define CMD_REBOOT     0xE2 // Select reboot register
#define CMD_HWTYPE     0xF0 // Hardware type 
#define CMD_FLASH      0xFF // Firmware write mode
