/*
 *  Copyright (C) 2022-2025  Ian Scott
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
#include <conio.h>
#include <dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <i86.h>

#include "../common/picogus.h"

static void banner(void) {
    printf("PicoGUSinit v3.6.0 (c) 2025 Ian Scott - licensed under the GNU GPL v2\n");
}


static void usage(card_mode_t mode, bool print_all) {
    // Max line length @ 80 chars:
    //     "...............................................................................\n"
    printf("Usage:\n");
    printf("   /?            - show this message (/?? to show options for all modes)\n");
    printf("   /flash fw.uf2 - program the PicoGUS with the firmware file fw.uf2\n");
    printf("   /mode x       - change card mode to x (gus, sb, mpu, psg, adlib, usb)\n");
    printf("   /save         - save settings to the card to persist on system boot\n");
    printf("   /defaults     - set all settings for all modes to defaults\n");
    printf("   /wtvol x      - set volume of WT header. 0-100, Default 100 (2.0 cards only)\n");
    printf("   /joy 1|0      - enable/disable USB joystick support, Default: 0\n");
    printf("   /mainvol x    - set the main audio volume: 0 - 100\n");
    //     "...............................................................................\n"
    printf("MPU-401 settings:\n");
    printf("   /mpuport x    - set the base port of the MPU-401. Default: 330, 0 to disable\n");
    printf("   /mpudelay 1|0 - delay SYSEX (for rev.0 Roland MT-32)\n");
    printf("   /mpufake 1|0  - fake all notes off (for Roland RA-50)\n");
    if (mode == GUS_MODE || mode == MODE_GUSVOL || print_all) {
        //     "...............................................................................\n"
        printf("GUS settings:\n");
        printf("   /gusport x  - set the base port of the GUS. Default: 240\n");
        printf("   /gusbuf n   - set audio buffer to n samples. Default: 4, Min: 1, Max: 256\n");
        printf("                 (tweaking can help programs that hang or have audio glitches)\n");
        printf("   /gusdma n   - force DMA interval to n us. Default: 0, Min: 0, Max: 255\n");
        printf("                 Specifying 0 restores the GUS default behavior.\n");
        printf("                 (increase to fix games with streaming audio like Doom)\n");
        printf("   /gus44k 1|0 - Fixed 44.1kHz output for all active voice #s [EXPERIMENTAL]\n");
        printf("   /gusvol x   - set the GUS audio volume: 0 - 100\n");
    }
    if (mode == SB_MODE || mode == MODE_SBVOL || print_all) {
        //     "...............................................................................\n"
        printf("Sound Blaster settings:\n");
        printf("   /sbport x    - set the base port of the Sound Blaster. Default: 220\n");
        printf("   /sbvol x     - set the Sound Blaster audio volume: 0 - 100%\n");
    }
    if (mode == SB_MODE || mode == ADLIB_MODE || mode == MODE_OPLVOL || print_all) {
        printf("AdLib settings:\n");
        printf("   /oplport x   - set the base port of the OPL2. Default: 388, 0 to disable\n");
        printf("   /oplwait 1|0 - wait on OPL2 write. Can fix speed-sensitive early AdLib games\n");
        printf("   /oplvol x    - set the OPL2 audio volume: 0 - 100\n");
    }
    if (mode == SB_MODE || mode == USB_MODE || mode == MODE_CDVOL || print_all) {
        printf("CD-ROM settings:\n");
        printf("   /cdport x     - set base port of CD interface. Default: 250, 0 to disable\n");
        printf("   /cdlist       - list CD images on the inserted USB drive\n");
        printf("   /cdload n     - load image n in the list given by /cdlist. 0 to unload image\n");
        printf("   /cdloadname x - load CD image by name. Names with spaces can be quoted\n");
        printf("   /cdvol n      - set the CD audio volume: 0 - 100\n");
        printf("   /cdauto 1|0   - auto-advance loaded image when same USB drive is reinserted\n");
    }
    if (mode == PSG_MODE || mode == MODE_PSGVOL || print_all) {
        //     "...............................................................................\n"
        printf("Tandy settings:\n");
        printf("   /tandyport x - set the base port of the Tandy 3-voice. Default: 2C0\n");
        printf("   /psgvol x    - set the PSG audio volume: 0 - 100\n");
    }
    if (mode == PSG_MODE || mode == MODE_PSGVOL || print_all) {
        //     "...............................................................................\n"
        printf("CMS settings:\n");
        printf("   /cmsport x - set the base port of the CMS. Default: 220\n");
        printf("   /psgvol x  - set the PSG audio volume: 0 - 100\n");
    }
    if (mode == USB_MODE || mode == PSG_MODE || mode == ADLIB_MODE || print_all) {
        //     "...............................................................................\n"
        printf("Serial Mouse settings:\n");
        printf("   /mousecom n - mouse COM port. Default: 0, Choices: 0 (disable), 1, 2, 3, 4\n");
        printf("   /mouseproto n - set mouse protocol. Default: 0 (Microsoft)\n");
        printf("          0 - Microsoft Mouse 2-button,      1 - Logitech 3-button\n");
        printf("          2 - IntelliMouse 3-button + wheel, 3 - Mouse Systems 3-button\n");
        printf("   /mouserate n  - set report rate in Hz. Default: 60, Min: 20, Max: 200\n");
        printf("          (increase for smoother cursor movement, decrease for lower CPU load)\n");
        printf("   /mousesen n   - set mouse sensitivity (256 - 100%, 128 - 50%, 512 - 200%)\n");
    }
    if (mode == NE2000_MODE || print_all) {
        //     "...............................................................................\n"
        printf("NE2000/WiFi settings:\n");
        printf("   /ne2kport x   - set the base port of the NE2000. Default: 300\n");
        printf("   /wifissid abc - set the WiFi SSID to abc\n");
        printf("   /wifipass xyz - set the WiFi WPA/WPA2 password/key to xyz\n");
        printf("   /wifinopass   - unset the WiFi password to connect to an open access point\n");
        printf("   /wifistatus   - print current WiFi status\n");
    }
}

static const char* mouse_protocol_str[] = {
    "Microsoft", "Logitech", "IntelliMouse", "Mouse Systems"
};


static void err_ultrasnd(void) {
    //              "................................................................................\n"
    fprintf(stderr, "ERROR: In GUS mode but no ULTRASND variable set or is malformed!\n");
    fprintf(stderr, "The ULTRASND environment variable must be set in the following format:\n");
    fprintf(stderr, "\tset ULTRASND=xxx,y,n,z,n\n");
    fprintf(stderr, "Where xxx = port, y = DMA, z = IRQ. n is ignored.\n");
    fprintf(stderr, "Port is set via /gusport xxx option; DMA and IRQ configued via jumper.\n");
}


static void err_blaster(void) {
    //              "................................................................................\n"
    fprintf(stderr, "ERROR: In SB mode but no BLASTER variable set or is malformed!\n");
    fprintf(stderr, "The BLASTER environment variable must be set in the following format:\n");
    fprintf(stderr, "\tset BLASTER=Axxx Iy Dz T3\n");
    fprintf(stderr, "Where xxx = port, y = IRQ, z = DMA. T3 indicates an SB 2.0 compatible card.\n");
    fprintf(stderr, "Port is set via /sbport xxx option; DMA and IRQ configued via jumper.\n");
}


static void err_pigus(void) {
    fprintf(stderr, "ERROR: no PicoGUS detected!\n");
}


static void err_protocol(uint8_t expected, uint8_t got) {
    fprintf(stderr, "ERROR: PicoGUS card using protocol %u, needs %u\n", got, expected);
    fprintf(stderr, "Please run the latest PicoGUS firmware and pgusinit.exe versions together!\n");
    fprintf(stderr, "To flash new firmware, run pgusinit /flash picogus.uf2\n");
}


static int init_gus(void) {
    char* ultrasnd = getenv("ULTRASND");
    if (ultrasnd == NULL) {
        err_ultrasnd();
        return 1;
    }

    // Parse ULTRASND
    uint16_t port;
    int e;
    e = sscanf(ultrasnd, "%hx,%*hhu,%*hhu,%*hhu,%*hhu", &port);
    if (e != 1) {
        err_ultrasnd();
        return 2;
    }

    outp(CONTROL_PORT, MODE_GUSPORT); // Select port register
    uint16_t tmp_port = inpw(DATA_PORT_LOW);
    if (port != tmp_port) {
        err_ultrasnd();
        return 2;
    }

    // Detect if there's something GUS-like...
    // Set memory address to 0
    outp(port + 0x103, 0x43);
    outpw(port + 0x104, 0x0);
    outp(port + 0x103, 0x44);
    outpw(port + 0x104, 0x0);
    // Write something
    outp(port + 0x107, 0xDD);
    // Read it and see if it's the same
    if (inp(port + 0x107) != 0xDD) {
        fprintf(stderr, "ERROR: Card not responding to GUS commands on port %x\n", port);
        return 98;
    }
    printf("GUS-like card detected on port %x...\n", port);

    // Enable IRQ latches
    outp(port, 0x8);
    // Select reset register
    outp(port + 0x103, 0x4C);
    // Master reset to run. DAC enable and IRQ enable will be done by the application.
    outp(port + 0x105, 0x1);
    return 0;
}


static int init_sb(void) {
    char* blaster = getenv("BLASTER");
    if (blaster == NULL) {
        err_blaster();
        return 1;
    }

    // Parse BLASTER
    uint16_t port;
    int e;
    e = sscanf(blaster, "A%hx I%*hhu D%*hhu T3", &port);
    if (e != 1) {
        err_blaster();
        return 2;
    }

    outp(CONTROL_PORT, MODE_SBPORT); // Select port register
    uint16_t tmp_port = inpw(DATA_PORT_LOW);
    if (port != tmp_port) {
        err_blaster();
        return 2;
    }
    return 0;
}


static void print_string(uint8_t mode) {
    outp(CONTROL_PORT, 0xCC); // Knock on the door...
    outp(CONTROL_PORT, mode); // Select mode register

    char str[256] = {0};
    for (uint8_t i = 0; i < 255; ++i) {
        str[i] = inp(DATA_PORT_HIGH);
        if (!str[i]) {
            break;
        }
    }
    puts(str);
}


static bool wait_for_read(const uint8_t value) {
    for (uint32_t i = 0; i < 6000000; ++i) {    // Up to 6000000, for bigger fws like pg-multi.uf2, waiting for flash erase. If not, timeout and error.
        if (inp(DATA_PORT_HIGH) == value) {
            return true;
        }
    }
    return false;
}


static cdrom_image_status_t wait_for_cd_status(void) {
    outp(CONTROL_PORT, 0xCC); // Knock on the door...
    outp(CONTROL_PORT, MODE_CDSTATUS); // Select CD image status register
    cdrom_image_status_t cd_status;
    for (uint16_t i = 0; i < 256; ++i) {
        delay(100);
        cd_status = (cdrom_image_status_t)inp(DATA_PORT_HIGH);
        if (cd_status != CD_STATUS_BUSY) {
            break;
        }
    }
    return cd_status;
}

static bool print_cdimage_list(void) {
    outp(CONTROL_PORT, 0xCC); // Knock on the door...
    outp(CONTROL_PORT, MODE_CDLOAD); // Get currently loaded index
    uint8_t current_index = inp(DATA_PORT_HIGH);
    printf("Listing CD images on USB drive:\n");
    outp(CONTROL_PORT, MODE_CDLIST); // Select CD image list register
    cdrom_image_status_t cd_status = wait_for_cd_status();
    if (cd_status == CD_STATUS_BUSY) {
        printf("Timeout getting CD image list\n");
        return false;
    } else if (cd_status == CD_STATUS_ERROR) {
        printf("Error getting CD image list: ");
        print_string(MODE_CDERROR);
        return false;
    }
    outp(CONTROL_PORT, MODE_CDLIST); // Select CD image list register
    char b[256], c, *p = b;
    uint8_t line = 1;
    while ((c = inp(DATA_PORT_HIGH)) != 4 /* ASCII EOT */) {
        *p++ = c;
        if (!c) {
	    putchar(current_index == line ? '*' : ' ');
            printf(" %2d: %s\n", line++, b);
            p = b;
        }
    }
    if (current_index) {
	printf("Currently loaded image marked with \"*\".\n");
    } else {
	printf("No image currently loaded.\n");
    }
    printf("Run \"pgusinit /cdload n\" to load the nth image in the above list, 0 to unload.\n");
    return true;
}


static int print_cdimage_current(void) {
    outp(CONTROL_PORT, MODE_CDLOAD); // Get currently loaded index
    uint8_t current_index = inp(DATA_PORT_HIGH);
    if (!current_index) {
        printf("No CD image loaded.\n");
        return 97;
    }
    printf("CD image loaded: ");
    print_string(MODE_CDNAME);
    return 0;
}


static int wait_for_cd_load(void) {
    cdrom_image_status_t cd_status = wait_for_cd_status();
    if (cd_status == CD_STATUS_BUSY) {
        printf("Timeout loading CD image.\n");
        return 99;
    } else if ((int8_t)cd_status == CD_STATUS_ERROR) {
        printf("Error loading CD image: ");
        print_string(MODE_CDERROR);
        return 98;
    }
    return print_cdimage_current();
}


static void print_cdemu_status(void) {
    outp(CONTROL_PORT, MODE_CDAUTOADV); // Select joystick enable register
    uint8_t tmp_uint8 = inp(DATA_PORT_HIGH);
    outp(CONTROL_PORT, MODE_CDPORT); // Select port register
    uint16_t tmp_uint16 = inpw(DATA_PORT_LOW); // Get port
    outp(CONTROL_PORT, MODE_CDVOL); // Select CD volume register
    uint16_t tmp_vol = inp(DATA_PORT_HIGH);
    printf("CD-ROM emulation on port %x, image auto-advance %s\nCD-Audio Volume: %u%\n", tmp_uint16, tmp_uint8 ? "enabled" : "disabled", tmp_vol);
    
    print_cdimage_current();
}


static void write_settings(void) {
    outp(CONTROL_PORT, MODE_SAVE); // Select save settings register
    outp(DATA_PORT_HIGH, 0xff);
    printf("Settings saved to flash.\n");
    delay(100);
}

// For multifw
static int reboot_to_firmware(const uint8_t value, const bool permanent) {
    outp(CONTROL_PORT, 0xCC); // Knock on the door...

    outp(CONTROL_PORT, MODE_BOOTMODE); // Select firmware selection register
    outp(DATA_PORT_HIGH, value); // Send firmware number and permanent flag
    delay(100);

    printf("\nMode change requested.\n");
    if (permanent) {
        write_settings();
    }
    printf("Rebooting to fw: %s...\n", modenames[value]);
    outp(CONTROL_PORT, MODE_REBOOT); // Select reboot register
    outp(DATA_PORT_HIGH, 0xff);
    delay(100);

    // Wait for card to reboot to new firmware
    if (!wait_for_read(0xDD)) {
        fprintf(stderr, "ERROR: card is not alive after rebooting to new firmware\n");
        return 99;
    }
    printf("PicoGUS detected: Firmware version: ");
    print_string(MODE_FWSTRING);
    return 0;
}

static int write_firmware(const char* fw_filename) {
    union {
        uint8_t buf[512];
        struct UF2_Block {
            // 32 byte header
            uint32_t magicStart0;
            uint32_t magicStart1;
            uint32_t flags;
            uint32_t targetAddr;
            uint32_t payloadSize;
            uint32_t blockNo;
            uint32_t numBlocks;
            uint32_t fileSize; // or familyID;
            uint8_t data[476];
            uint32_t magicEnd;
        } uf2;
    } uf2_buf;

    FILE* fp = fopen(fw_filename, "rb");
    if (!fp) {
        fprintf(stderr, "ERROR: unable to open firmware file %s\n", fw_filename);
        return 10;
    }

    outp(CONTROL_PORT, 0x01); // Select protocol version register
    uint8_t protocol = inp(DATA_PORT_HIGH);
    if (PICOGUS_PROTOCOL_VER != protocol && protocol < 3) {
        printf("Older fw protocol version %d detected, upgrading firmware in compatibility mode\n", protocol);
    }

    uint32_t numBlocks = 1;
    for (uint32_t i = 0; i < numBlocks; ++i) {
        if (fread(uf2_buf.buf, 1, 512, fp) != 512) {
            fprintf(stderr, "ERROR: file %s is not a valid UF2 file - too short\n", fw_filename);
            return 11;
        }

        if (uf2_buf.uf2.magicStart0 != 0x0A324655 || uf2_buf.uf2.magicStart1 != 0x9E5D5157 || uf2_buf.uf2.magicEnd != 0x0AB16F30) {
            fprintf(stderr, "ERROR: file %s is not a valid UF2 file - bad magic\n", fw_filename);
            return 12;
        }

        if (i == 0) {
            numBlocks = uf2_buf.uf2.numBlocks;

            // Put card into programming mode
            outp(CONTROL_PORT, 0xCC); // Knock on the door...
            outp(CONTROL_PORT, MODE_FLASH); // Select firmware programming mode
            // Wait a bit for 2nd core on Pico to restart
            delay(100);
            if (!wait_for_read(PICO_FIRMWARE_IDLE)) {
                fprintf(stderr, "ERROR: Card is not in programming mode?\n");
                return 13;
            }
            fflush(stdout);
            fprintf(stderr, "Programming %d blocks", numBlocks);
        }

        if (i != uf2_buf.uf2.blockNo) {
            fprintf(stderr, "\nERROR: file %s is not a valid UF2 file - block mismatch\n", fw_filename);
            return 14;
        }

        for (uint16_t b = 0; b < 512; ++b) {
            // Write firmware byte
            outp(DATA_PORT_HIGH, uf2_buf.buf[b]);
            if (b == 512 && protocol == 1) {
                // Protocol 1 abuses IOCHRDY to pause during flash erase/write. Some chipsets give
                // up waiting on IOCHRDY and release the ISA bus after a certain amount of time before
                // the flash operation is finished. This is an extra delay to work around this issue.
                if (i == 0) { // first block takes longer due to flash erase
                    delay(5000);
                } else {
                    delay(25);
                }
            }
            if (i < (numBlocks - 1) || b < 511) { // If it's not the very last byte
                if (!wait_for_read(PICO_FIRMWARE_WRITING)) {
                    fprintf(stderr, "\nERROR: Card is not in firmware writing mode?\n");
                    return 15;
                }
            } else if (protocol > 1) {
                if (!wait_for_read(PICO_FIRMWARE_DONE)) {
                    fprintf(stderr, "\nERROR: Card has written last firmware byte but is not done\n");
                    return 15;
                }
                outp(CONTROL_PORT, 0xCC); // Knock on the door...
                outp(CONTROL_PORT, MODE_FLASH); // Select firmware programming mode, which will reboot the card in DONE
            }
        }
        fprintf(stderr, ".");
        //fprintf(stderr, "%u ", i);
    }
    fclose(fp);

    // Wait for card to reboot
    printf("\nProgramming complete. Waiting for the card to reboot...\n");
    if (!wait_for_read(0xDD)) {
        fprintf(stderr, "ERROR: card is not alive after programming firmware\n");
        return 99;
    }
    printf("PicoGUS detected: Firmware version: ");
    print_string(MODE_FWSTRING);
    return 0;
}

static void wifi_printStatus(void)
{
    outp(CONTROL_PORT, 0xCC); // Knock on the door...
    outp(CONTROL_PORT, MODE_WIFISTAT); // Select WiFi status mode
    outp(DATA_PORT_HIGH, 0); // Write to start getting the status
   
    printf("WiFi status: ");
    uint16_t try = 0;
    char c;
    while (c = inp(DATA_PORT_HIGH)) {
        if (c == 255) {
            if (++try == 10000) {
                printf("Error getting WiFI status\n");
                break;
            } else {
                continue;
            }
        }
        putchar(c);
    }
    putchar('\n');
}

static void send_string(uint8_t mode, char* str, int16_t max_len)
{
    outp(CONTROL_PORT, 0xCC); // Knock on the door...
    outp(CONTROL_PORT, mode);
    char chr;
    for (int16_t i = 0; i < max_len; ++i) {
        if (str[i] == 0) { // End of string
            break;
        }
        outp(DATA_PORT_HIGH, str[i]);
    }
    outp(DATA_PORT_HIGH, 0);
}

#define send_uint8_arg(mode) \
    if (i + 1 >= argc)                          \
    {                                           \
        usage(mode, false);                     \
        return 255;                             \
    }                                           \
    e = sscanf(argv[++i], "%hhu", &tmp_uint8);  \
    if (e != 1 || tmp_uint8 > 255)              \
    {                                           \
        usage(mode, false);                     \
        return 4;                               \
    }                                           \
    outp(CONTROL_PORT, mode);                   \
    outp(DATA_PORT_HIGH, tmp_uint8);

#define send_volume(mode) \
    if (i + 1 >= argc)                          \
    {                                           \
        usage(mode, false);                     \
        return 255;                             \
    }                                           \
    e = sscanf(argv[++i], "%hhu", &tmp_uint8);  \
    if (e != 1 || tmp_uint8 > 100)              \
    {                                           \
        usage(mode, false);                     \
        return 4;                               \
    }                                           \
    outp(CONTROL_PORT, mode);                   \
    outp(DATA_PORT_HIGH, tmp_uint8);

#define process_bool_opt(option) \
if (i + 1 >= argc) { \
    usage(mode, false); \
    return 255; \
} \
e = sscanf(argv[++i], "%1[01]", tmp_arg); \
if (e != 1) { \
    usage(mode, false); \
    return 5; \
} \
option = (tmp_arg[0] == '1') ? 1 : 0;


#define process_port_opt(option) \
if (i + 1 >= argc) { \
    usage(mode, false); \
    return 255; \
} \
e = sscanf(argv[++i], "%hx", &option); \
if (e != 1 || option > 0x3ffu) { \
    usage(mode, false); \
    return 4; \
}

int main(int argc, char* argv[]) {
    int e;
    uint8_t enable_joystick = 0;
    uint8_t wtvol;
    char fw_filename[256] = {0};
    bool wifichg = false;
    card_mode_t mode;
    char mode_name[8] = {0};
    int fw_num = 8;
    bool permanent = false;

    banner();
    // Get magic value from port on PicoGUS that is not on real GUS
    outp(CONTROL_PORT, 0xCC); // Knock on the door...
    outp(CONTROL_PORT, MODE_MAGIC); // Select magic string register
    if (inp(DATA_PORT_HIGH) != 0xDD) {
        err_pigus();
        return 99;
    };
    printf("PicoGUS detected: Firmware version: ");
    print_string(MODE_FWSTRING);

    int i = 1;
    // /flash option is special and can work across protocol versions, so if it's specified, that's all we do
    while (i < argc) {
        if (stricmp(argv[i], "/flash") == 0) {
            if (i + 1 >= argc) {
                usage(INVALID_MODE, false);
                return 255;
            }
            if (strlen(argv[++i]) > 255) {
                usage(INVALID_MODE, false);
                return 5;
            }
            return write_firmware(argv[i]);
        }
        ++i;
    }

    outp(CONTROL_PORT, 0x01); // Select protocol version register
    uint8_t protocol_got = inp(DATA_PORT_HIGH);
    if (PICOGUS_PROTOCOL_VER != protocol_got) {
      err_protocol(PICOGUS_PROTOCOL_VER, protocol_got);
      return 97;
    }

    outp(CONTROL_PORT, MODE_BOOTMODE); // Select mode register
    mode = inp(DATA_PORT_HIGH);

    char tmp_arg[2];
    uint16_t tmp_uint16;
    uint8_t tmp_uint8;
    i = 1;
    while (i < argc) {
        // global options /////////////////////////////////////////////////////////////////
        if (stricmp(argv[i], "/?") == 0) {
            usage(mode, false);
            return 0;
        } else if (stricmp(argv[i], "/??") == 0) {
            usage(mode, true);
            return 0;
        } else if (stricmp(argv[i], "/joy") == 0) {
            process_bool_opt(tmp_uint8);
            outp(CONTROL_PORT, MODE_JOYEN); // Select joystick enable register
            outp(DATA_PORT_HIGH, tmp_uint8);
        } else if (stricmp(argv[i], "/mode") == 0) {               
            if (i + 1 >= argc) {
                usage(mode, false);
                return 255;
            }
            e = sscanf(argv[++i], "%7s", mode_name);
            if (e != 1) {
                usage(mode, false);
                return 5;
            }
        } else if (stricmp(argv[i], "/wtvol") == 0) {
            if (i + 1 >= argc) {
                usage(mode, false);
                return 255;
            }
            e = sscanf(argv[++i], "%hhu", &tmp_uint8);
            if (e != 1 || tmp_uint8 > 100) {
                usage(mode, false);
                return 4;
            }
            outp(CONTROL_PORT, MODE_WTVOL); // Select wavetable volume register
            outp(DATA_PORT_HIGH, tmp_uint8); // Write volume
        } else if (stricmp(argv[i], "/save") == 0) {
            permanent = true;
        } else if (stricmp(argv[i], "/defaults") == 0) {
            outp(CONTROL_PORT, MODE_DEFAULTS); // Select defaults register
            outp(DATA_PORT_HIGH, 0xff);
        // GUS options /////////////////////////////////////////////////////////////////
        } else if (stricmp(argv[i], "/gus44k") == 0) {
            process_bool_opt(tmp_uint8);
            outp(CONTROL_PORT, MODE_GUS44K); // Select force 44k buffer
            outp(DATA_PORT_HIGH, tmp_uint8);
        } else if (stricmp(argv[i], "/gusbuf") == 0) {
            if (i + 1 >= argc) {
                usage(mode, false);
                return 255;
            }
            e = sscanf(argv[++i], "%hhu", &tmp_uint8);
            if (e != 1 || tmp_uint8 < 1) {
                usage(mode, false);
                return 3;
            }
            outp(CONTROL_PORT, MODE_GUSBUF); // Select audio buffer register
            outp(DATA_PORT_HIGH, (unsigned char)(tmp_uint8 - 1));
        } else if (stricmp(argv[i], "/gusdma") == 0) {
            if (i + 1 >= argc) {
                usage(mode, false);
                return 255;
            }
            e = sscanf(argv[++i], "%hhu", &tmp_uint8);
            if (e != 1) {
                usage(mode, false);
                return 4;
            }
            outp(CONTROL_PORT, MODE_GUSDMA); // Select DMA interval register
            outp(DATA_PORT_HIGH, tmp_uint8);
        } else if (stricmp(argv[i], "/gusport") == 0) {
            process_port_opt(tmp_uint16);
            outp(CONTROL_PORT, MODE_GUSPORT); // Select GUS port register
            outpw(DATA_PORT_LOW, tmp_uint16); // Write port
        // SB options /////////////////////////////////////////////////////////////////
        } else if (stricmp(argv[i], "/sbport") == 0) {
            process_port_opt(tmp_uint16);
            outp(CONTROL_PORT, MODE_SBPORT); // Select SB port register
            outpw(DATA_PORT_LOW, tmp_uint16); // Write port
        } else if (stricmp(argv[i], "/oplport") == 0) {
            process_port_opt(tmp_uint16);
            outp(CONTROL_PORT, MODE_OPLPORT); // Select OPL port register
            outpw(DATA_PORT_LOW, tmp_uint16); // Write port
        } else if (stricmp(argv[i], "/oplwait") == 0) {
            process_bool_opt(tmp_uint8);
            outp(CONTROL_PORT, MODE_OPLWAIT); // Select Adlib wait register
            outp(DATA_PORT_HIGH, tmp_uint8); // Write sysex delay and fake all notes off settings
        // MPU options /////////////////////////////////////////////////////////////////
        } else if (stricmp(argv[i], "/mpuport") == 0) {
            process_port_opt(tmp_uint16);
            outp(CONTROL_PORT, MODE_MPUPORT); // Select MPU port register
            outpw(DATA_PORT_LOW, tmp_uint16); // Write port
        } else if (stricmp(argv[i], "/mpudelay") == 0) {
            process_bool_opt(tmp_uint8);
            outp(CONTROL_PORT, MODE_MPUDELAY); // Select MPU sysex delay register
            outp(DATA_PORT_HIGH, tmp_uint8);
        } else if (stricmp(argv[i], "/mpufake") == 0) {
            process_bool_opt(tmp_uint8);
            outp(CONTROL_PORT, MODE_MPUFAKE); // Select MPU fake all notes off register
            outp(DATA_PORT_HIGH, tmp_uint8);
        // Tandy options /////////////////////////////////////////////////////////////////
        } else if (stricmp(argv[i], "/tandyport") == 0) {
            process_port_opt(tmp_uint16);
            outp(CONTROL_PORT, MODE_TANDYPORT); // Select Tandy port register
            outpw(DATA_PORT_LOW, tmp_uint16); // Write port
        // CMS options /////////////////////////////////////////////////////////////////
        } else if (stricmp(argv[i], "/cmsport") == 0) {
            process_port_opt(tmp_uint16);
            outp(CONTROL_PORT, MODE_CMSPORT); // Select CMS port register
            outpw(DATA_PORT_LOW, tmp_uint16); // Write port
        // Mouse options /////////////////////////////////////////////////////////////////
        } else if (stricmp(argv[i], "/mousecom") == 0) {
            if (i + 1 >= argc) {
                usage(mode, false);
                return 255;
            }
            e = sscanf(argv[++i], "%hhu", &tmp_uint8);
            if (e != 1 || tmp_uint8 > 3) {
                usage(mode, false);
                return 4;
            }
            switch (tmp_uint8) {
            case 0:
                tmp_uint16 = 0;
                break;
            case 1:    
                tmp_uint16 = 0x3f8;
                break;
            case 2:    
                tmp_uint16 = 0x2f8;
                break;
            case 3:    
                tmp_uint16 = 0x3e8;
                break;
            case 4:    
                tmp_uint16 = 0x2e8;
                break;
            default:
                usage(mode, false);
                return 4;
            }
            outp(CONTROL_PORT, MODE_MOUSEPORT); // Select mouse port register
            outpw(DATA_PORT_LOW, tmp_uint16);
        } else if (stricmp(argv[i], "/mousesen") == 0) {
            if (i + 1 >= argc) {
                usage(mode, false);
                return 255;
            }
            e = sscanf(argv[++i], "%hi", &tmp_uint16);
            if (e != 1) {
                usage(mode, false);
                return 4;
            }
            outp(CONTROL_PORT, MODE_MOUSESEN); // Select mouse sensitivity register
            outpw(DATA_PORT_LOW, tmp_uint16);
        } else if (stricmp(argv[i], "/mouseproto") == 0) {
            if (i + 1 >= argc) {
                usage(mode, false);
                return 255;
            }
            e = sscanf(argv[++i], "%hhu", &tmp_uint8);
            if (e != 1 || tmp_uint8 > 3) {
                usage(mode, false);
                return 4;
            }
            outp(CONTROL_PORT, MODE_MOUSEPROTO);
            outp(DATA_PORT_HIGH, tmp_uint8);
        } else if (stricmp(argv[i], "/mouserate") == 0) {
            if (i + 1 >= argc) {
                usage(mode, false);
                return 255;
            }
            e = sscanf(argv[++i], "%hhu", &tmp_uint8);
            if (e != 1 || tmp_uint8 < 20 || tmp_uint8 > 200) {
                usage(mode, false);
                return 4;
            }
            outp(CONTROL_PORT, MODE_MOUSERATE);
            outp(DATA_PORT_HIGH, tmp_uint8);
        // NE2000/WiFi options /////////////////////////////////////////////////////////////////
        } else if (stricmp(argv[i], "/ne2kport") == 0) {
            process_port_opt(tmp_uint16);
            outp(CONTROL_PORT, MODE_NE2KPORT); // Select NE2000 port register
            outpw(DATA_PORT_LOW, tmp_uint16); // Write port
        } else if (stricmp(argv[i], "/wifistatus") == 0) {
            wifi_printStatus();
        } else if (stricmp(argv[i], "/wifissid") == 0) {
            if (i + 1 >= argc) {
                usage(mode, false);
                return 255;
            }
            if (strlen(argv[++i]) > 32) {
                usage(mode, false);
                return 4;
            }
            wifichg = true;
            send_string(MODE_WIFISSID, argv[i], 32);
        } else if (stricmp(argv[i], "/wifipass") == 0) {
            if (i + 1 >= argc) {
                usage(mode, false);
                return 255;
            }
            if (strlen(argv[++i]) > 63) {
                usage(mode, false);
                return 4;
            }
            wifichg = true;
            send_string(MODE_WIFIPASS, argv[i], 63);
        } else if (stricmp(argv[i], "/wifinopass") == 0) {
            wifichg = true;
            send_string(MODE_WIFIPASS, "", 1);
        // CD-ROM options /////////////////////////////////////////////////////////////////
        } else if (stricmp(argv[i], "/cdport") == 0) {
            process_port_opt(tmp_uint16);
            outp(CONTROL_PORT, MODE_CDPORT); // Select CD port register
            outpw(DATA_PORT_LOW, tmp_uint16); // Write port
        } else if (stricmp(argv[i], "/cdlist") == 0) {
            if (!print_cdimage_list()) {
                return 98;
            }
            return 0;
        } else if (stricmp(argv[i], "/cdload") == 0) {
            if (i + 1 >= argc) {
                usage(mode, false);
                return 255;
            }
            e = sscanf(argv[++i], "%hhu", &tmp_uint8);
            if (e != 1) {
                usage(mode, false);
                return 3;
            }
            outp(CONTROL_PORT, MODE_CDLOAD); // Select CD image load register
            outp(DATA_PORT_HIGH, tmp_uint8);
            return wait_for_cd_load();
        } else if (stricmp(argv[i], "/cdloadname") == 0) {
            if (i + 1 >= argc) {
                usage(mode, false);
                return 255;
            }
            if (strlen(argv[++i]) > 127) {
                usage(mode, false);
                return 5;
            }
            send_string(MODE_CDNAME, argv[i], 127);
            return wait_for_cd_load();
        } else if (stricmp(argv[i], "/cdauto") == 0) {
            process_bool_opt(tmp_uint8);
            outp(CONTROL_PORT, MODE_CDAUTOADV); // Select CD image autoadvance register
            outp(DATA_PORT_HIGH, tmp_uint8);
        } else if (stricmp(argv[i], "/mainvol") == 0) {
            send_volume(MODE_MAINVOL);
        } else if (stricmp(argv[i], "/oplvol") == 0) {
            send_volume(MODE_OPLVOL);
        } else if (stricmp(argv[i], "/sbvol") == 0) {
            send_volume(MODE_SBVOL);
        } else if (stricmp(argv[i], "/cdvol") == 0) {
            send_volume(MODE_CDVOL);
        } else if (stricmp(argv[i], "/gusvol") == 0) {
            send_volume(MODE_GUSVOL);
        } else if (stricmp(argv[i], "/psgvol") == 0) {
            send_volume(MODE_PSGVOL);
        } else {
            printf("Unknown option: %s\n", argv[i]);
            usage(mode, false);
            return 255;
        }
        ++i;
    }


    if (mode_name[0]) {
        if (strnicmp(mode_name, "TANDY", 5) == 0 || strnicmp(mode_name, "CMS", 3) == 0) {
            // Backwards compatibility for old tandy and cms modes
            strcpy(mode_name, "PSG");
        } 
        int i;
        for (i = 1; i < 7; ++i) {
            if (strnicmp(modenames[i], mode_name, 7) == 0) {
                fw_num = i;
                break;
            }
        }
        if (i == 7) {
            usage(mode, false);
            return 255;
        }
        return reboot_to_firmware(fw_num, permanent);
    }

    if (wifichg) {
        outp(CONTROL_PORT, MODE_WIFIAPPLY);
        outp(DATA_PORT_HIGH, 0);
    }

    outp(CONTROL_PORT, MODE_HWTYPE); // Select hardware type register
    board_type_t board_type = inp(DATA_PORT_HIGH);
    if (board_type == PICO_BASED) {
        printf("Hardware: PicoGUS v1.x or PicoGUS Femto\n");
    } else if (board_type == PICOGUS_2) {
        printf("Hardware: PicoGUS v2.0\n");
    } else {
        printf("Hardware: Unknown\n");
    }
    printf("\n");

    outp(CONTROL_PORT, MODE_JOYEN); // Select joystick enable register
    tmp_uint8 = inp(DATA_PORT_HIGH);
    printf("USB joystick support %s\n", tmp_uint8 ? "enabled" : "disabled");

    if (board_type == PICOGUS_2) {
        outp(CONTROL_PORT, MODE_WTVOL); // Select wavetable volume register
        wtvol = inp(DATA_PORT_HIGH); // Read volume
        printf("Wavetable volume set to %u\n", wtvol);
    }

    outp(CONTROL_PORT, MODE_MPUPORT); // Select MPU port register
    tmp_uint16 = inpw(DATA_PORT_LOW); // Get port
    if (tmp_uint16) {
        outp(CONTROL_PORT, MODE_MPUDELAY); // Select MPU-401 delay register
        tmp_uint8 = inp(DATA_PORT_HIGH);
        printf("MPU-401: port %x; sysex delay: %s, ", tmp_uint16, tmp_uint8 ? "on" : "off");
        outp(CONTROL_PORT, MODE_MPUFAKE); // Select MPU-401 fake all notes off
        tmp_uint8 = inp(DATA_PORT_HIGH);
        printf("fake all notes off: %s\n", tmp_uint8 ? "on" : "off");
    } else {
        printf("MPU-401 disabled\n");
    }

    switch(mode) {
    case GUS_MODE:
        if (init_gus()) {
            return 1;
        }
        printf("GUS mode: ");
        outp(CONTROL_PORT, MODE_GUSBUF); // Select audio buffer register
        tmp_uint16 = inp(DATA_PORT_HIGH) + 1;
        printf("Audio buffer: %u samples; ", tmp_uint16);

        outp(CONTROL_PORT, MODE_GUSDMA); // Select DMA interval register
        tmp_uint8 = inp(DATA_PORT_HIGH);
        if (tmp_uint8 == 0) {
            printf("DMA interval: default; ");
        } else {
            printf("DMA interval: %u us; ", tmp_uint8);
        }

        outp(CONTROL_PORT, MODE_GUS44K); // Select force 44k buffer
        tmp_uint8 = inp(DATA_PORT_HIGH);
        if (tmp_uint8) {
            printf("Sample rate: fixed 44.1k\n");
        } else {
            printf("Sample rate: variable\n");
        }
        
        outp(CONTROL_PORT, MODE_GUSPORT); // Select port register
        tmp_uint16 = inpw(DATA_PORT_LOW); // Get port
        printf("Running in GUS mode on port %x\n", tmp_uint16);
        outp(CONTROL_PORT, MODE_GUSVOL); // Select GUS volume register
        tmp_uint8 = inp(DATA_PORT_HIGH);
        outp(CONTROL_PORT, MODE_MAINVOL); // Select Main volume register
        tmp_uint16 = inp(DATA_PORT_HIGH);
        printf("Volume:     GUS: %u     Main: %u\n", tmp_uint8, tmp_uint16);
        break;
    case ADLIB_MODE:
        outp(CONTROL_PORT, MODE_OPLPORT); // Select port register
        tmp_uint16 = inpw(DATA_PORT_LOW); // Get port
        printf("Running in AdLib/OPL2 mode on port %x", tmp_uint16);
        outp(CONTROL_PORT, MODE_OPLWAIT); // Select Adlib wait register
        tmp_uint8 = inp(DATA_PORT_HIGH);
        printf("%s\n", tmp_uint8 ? ", wait on" : "");
        break;
    case MPU_MODE:
        printf("Running in MPU-401 only mode (with IRQ)\n", tmp_uint16);
        break;
    case USB_MODE:
        printf("Running in USB mode\n", tmp_uint16);
        print_cdemu_status();
        break;
    case PSG_MODE:
        outp(CONTROL_PORT, MODE_TANDYPORT); // Select port register
        tmp_uint16 = inpw(DATA_PORT_LOW); // Get port
        printf("Running in PSG mode (Tandy 3-voice on port %x, ", tmp_uint16);
        outp(CONTROL_PORT, MODE_CMSPORT); // Select port register
        tmp_uint16 = inpw(DATA_PORT_LOW); // Get port
        printf("CMS/Game Blaster on port %x)\n", tmp_uint16);
        outp(CONTROL_PORT, MODE_PSGVOL); // Select PSG volume register
        tmp_uint8 = inp(DATA_PORT_HIGH);
        outp(CONTROL_PORT, MODE_MAINVOL); // Select Main volume register
        tmp_uint16 = inp(DATA_PORT_HIGH);
        printf("Volume:     PSG: %u     Main: %u\n", tmp_uint8, tmp_uint16);
        break;
    case SB_MODE:
        if (init_sb()) {
            return 1;
        }
        outp(CONTROL_PORT, MODE_SBPORT); // Select port register
        tmp_uint16 = inpw(DATA_PORT_LOW); // Get port
        printf("Running in Sound Blaster 2.0 mode on port %x ", tmp_uint16);
        outp(CONTROL_PORT, MODE_OPLPORT); // Select port register
        tmp_uint16 = inpw(DATA_PORT_LOW); // Get port
        if (tmp_uint16) {
            printf("(AdLib port %x", tmp_uint16);
            outp(CONTROL_PORT, MODE_OPLWAIT); // Select Adlib wait register
            tmp_uint8 = inp(DATA_PORT_HIGH);
            printf("%s)\n", tmp_uint8 ? ", wait on" : "");
            outp(CONTROL_PORT, MODE_OPLVOL); // Select Adlib volume register
            tmp_uint8 = inp(DATA_PORT_HIGH);
            printf("Volume:     ");
            printf("OPL: %u    ", tmp_uint8);
        } else {
            printf("(AdLib port disabled)\n");
            printf("Volume:     ");
        }
        outp(CONTROL_PORT, MODE_SBVOL); // Select Sound Blaster volume register
        uint8_t tmp_uint8 = inp(DATA_PORT_HIGH);
        printf("SB: %u    ", tmp_uint8);

        outp(CONTROL_PORT, MODE_MAINVOL); // Select Sound Blaster volume register
        tmp_uint8 = inp(DATA_PORT_HIGH);
        printf("Main: %u    \n", tmp_uint8);

        print_cdemu_status();
        break;
    case NE2000_MODE:
        outp(CONTROL_PORT, MODE_NE2KPORT); // Select port register
        tmp_uint16 = inpw(DATA_PORT_LOW); // Get port
        printf("Running in NE2000 mode on port %x\n", tmp_uint16);
        wifi_printStatus();
        break;
    default:
        printf("Running in unknown mode (maybe upgrade pgusinit?)\n");
        break;
    }
    if (mode == USB_MODE || mode == PSG_MODE || mode == ADLIB_MODE) {
        outp(CONTROL_PORT, MODE_MOUSEPORT); // Select port register
        tmp_uint16 = inpw(DATA_PORT_LOW); // Get port
        printf("Serial Mouse ");
        switch (tmp_uint16) {
        case 0:
            printf("disabled (enable with /mousecom n option)\n");
            break;
        case 0x3f8:
            printf("enabled on COM1\n");
            break;
        case 0x2f8:
            printf("enabled on COM2\n");
            break;
        case 0x3e8:
            printf("enabled on COM3\n");
            break;
        case 0x2e8:
            printf("enabled on COM4\n");
            break;
        default:
            printf("enabled on IO port %x\n", tmp_uint16);
        }
        if (tmp_uint16 != 0) {
            outp(CONTROL_PORT, MODE_MOUSERATE);
            tmp_uint8 = inp(DATA_PORT_HIGH);
            printf("Mouse report rate: %d Hz, ", tmp_uint8);

            outp(CONTROL_PORT, MODE_MOUSEPROTO);
            tmp_uint8 = inp(DATA_PORT_HIGH);
            printf("protocol: %s\n", mouse_protocol_str[tmp_uint8]);

            outp(CONTROL_PORT, MODE_MOUSESEN);
            tmp_uint16 = inpw(DATA_PORT_LOW);
            printf("Mouse sensitivity: %d (%d.%02d)\n", tmp_uint16, (tmp_uint16 >> 8), ((tmp_uint16 & 0xFF) * 100) >> 8);
        }
    }
    printf("PicoGUS initialized!\n");

    if (permanent) {
        write_settings();
    }

    return 0;
}

