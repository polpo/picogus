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
#include <io.h>
#include <conio.h>
#include <dos.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <i86.h>
#include <ctype.h>
#include <string.h>
#include "pgusinit.h"

static card_mode_t gMode;
static card_mode_t newMode = 0;
static board_type_t board_type;
static bool wifichg = false;
static bool permanent = false;
static bool is_console;
static uint8_t page_lines;

#include "../common/picogus.h"


const uint8_t get_screen_lines(void)
{
    uint16_t bios_screensize = *(uint16_t far *)MK_FP(0x40, 0x4c);
    uint8_t bios_screencols = *(uint8_t far *)MK_FP(0x40, 0x4a);

    if (bios_screensize && bios_screencols) {
        return bios_screensize / bios_screencols / 2;
    }
    return 25;
}

static void pageprintf(char *format, ...)
{
    static uint8_t printed_lines = 0;

    va_list arglist;
    va_start(arglist, format);
    vprintf(format, arglist);
    va_end(arglist);
    printed_lines++;
    if (is_console && (printed_lines >= page_lines)) {
        fprintf(stderr, "Press any key to continue...");
        getch();
        putchar('\r');
        printed_lines = 0;
    }
}

static void banner(void)
{
    printf("PicoGUSinit v3.7.0 (c) 2025 Ian Scott - licensed under the GNU GPL v2\n");
}


static void usage(card_mode_t mode, bool print_all)
{
    // Max line length @ 80 chars:
    //         "...............................................................................\n"
    pageprintf("Usage:\n");
    pageprintf("   /?            - show this message (/?? to show options for all modes)\n");
    pageprintf("   /flash fw.uf2 - program the PicoGUS with the firmware file fw.uf2\n");
    pageprintf("   /mode x       - change card mode to x (gus, sb, mpu, psg, adlib, usb)\n");
    pageprintf("   /save         - save settings to the card to persist on system boot\n");
    pageprintf("   /defaults     - set all settings for all modes to defaults\n");
    pageprintf("   /wtvol x      - set volume of WT header. 0-100, Default 100 (2.0 cards only)\n");
    pageprintf("   /joy 1|0      - enable/disable USB joystick support, Default: 0\n");
    pageprintf("   /mainvol x    - set the main audio volume: 0 - 100\n");
    //         "...............................................................................\n"
    pageprintf("MPU-401 settings:\n");
    pageprintf("   /mpuport x    - set the base port of the MPU-401. Default: 330, 0 to disable\n");
    pageprintf("   /mpudelay 1|0 - delay SYSEX (for rev.0 Roland MT-32)\n");
    pageprintf("   /mpufake 1|0  - fake all notes off (for Roland RA-50)\n");
    if (mode == GUS_MODE || print_all) {
        //         "...............................................................................\n"
        pageprintf("GUS settings:\n");
        pageprintf("   /gusport x  - set the base port of the GUS. Default: 240\n");
        pageprintf("   /gusbuf n   - set audio buffer to n samples. Default: 4, Min: 1, Max: 256\n");
        pageprintf("                 (tweaking can help programs that hang or have audio glitches)\n");
        pageprintf("   /gusdma n   - force DMA interval to n us. Default: 0, Min: 0, Max: 255\n");
        pageprintf("                 Specifying 0 restores the GUS default behavior.\n");
        pageprintf("                 (increase to fix games with streaming audio like Doom)\n");
        pageprintf("   /gus44k 1|0 - Fixed 44.1kHz output for all active voice #s [EXPERIMENTAL]\n");
        pageprintf("   /gusvol x   - set the GUS audio volume: 0 - 100\n");
    }
    if (mode == SB_MODE || print_all) {
        //         "...............................................................................\n"
        pageprintf("Sound Blaster settings:\n");
        pageprintf("   /sbport x    - set the base port of the Sound Blaster. Default: 220\n");
        pageprintf("   /sbvol x     - set the Sound Blaster audio volume: 0 - 100%\n");
    }
    if (mode == SB_MODE || mode == ADLIB_MODE || print_all) {
        pageprintf("AdLib settings:\n");
        pageprintf("   /oplport x   - set the base port of the OPL2. Default: 388, 0 to disable\n");
        pageprintf("   /oplwait 1|0 - wait on OPL2 write. Can fix speed-sensitive early AdLib games\n");
        pageprintf("   /oplvol x    - set the OPL2 audio volume: 0 - 100\n");
    }
    if (mode == SB_MODE || mode == USB_MODE || print_all) {
        pageprintf("CD-ROM settings:\n");
        pageprintf("   /cdport x     - set base port of CD interface. Default: 250, 0 to disable\n");
        pageprintf("   /cdlist       - list CD images on the inserted USB drive\n");
        pageprintf("   /cdload n     - load image n in the list given by /cdlist. 0 to unload image\n");
        pageprintf("   /cdloadname x - load CD image by name. Names with spaces can be quoted\n");
        pageprintf("   /cdvol n      - set the CD audio volume: 0 - 100\n");
        pageprintf("   /cdauto 1|0   - auto-advance loaded image when same USB drive is reinserted\n");
    }
    if (mode == PSG_MODE || print_all) {
        //         "...............................................................................\n"
        pageprintf("PSG settings:\n");
        pageprintf("   /tandyport x - set the base port of the Tandy 3-voice. Default: 2C0\n");
        pageprintf("   /cmsport x   - set the base port of the CMS. Default: 220\n");
        pageprintf("   /psgvol x    - set the PSG audio volume: 0 - 100\n");
    }
    if (mode == USB_MODE || mode == PSG_MODE || mode == ADLIB_MODE || print_all) {
        //         "...............................................................................\n"
        pageprintf("Serial Mouse settings:\n");
        pageprintf("   /mousecom n - mouse COM port. Default: 0, Choices: 0 (disable), 1, 2, 3, 4\n");
        pageprintf("   /mouseproto n - set mouse protocol. Default: 0 (Microsoft)\n");
        pageprintf("          0 - Microsoft Mouse 2-button,      1 - Logitech 3-button\n");
        pageprintf("          2 - IntelliMouse 3-button + wheel, 3 - Mouse Systems 3-button\n");
        pageprintf("   /mouserate n  - set report rate in Hz. Default: 60, Min: 20, Max: 200\n");
        pageprintf("          (increase for smoother cursor movement, decrease for lower CPU load)\n");
        pageprintf("   /mousesen n   - set mouse sensitivity (256 - 100%, 128 - 50%, 512 - 200%)\n");
    }
    if (mode == NE2000_MODE) { // NE2000 mode is special enough it should only be shown if the card is running the dedicated pg-ne2k firmware
        //         "...............................................................................\n"
        pageprintf("NE2000/WiFi settings:\n");
        pageprintf("   /ne2kport x   - set the base port of the NE2000. Default: 300\n");
        pageprintf("   /wifissid abc - set the WiFi SSID to abc\n");
        pageprintf("   /wifipass xyz - set the WiFi WPA/WPA2 password/key to xyz\n");
        pageprintf("   /wifinopass   - unset the WiFi password to connect to an open access point\n");
        pageprintf("   /wifistatus   - print current WiFi status\n");
    }
}


static const char* mouse_protocol_str[] = {
    "Microsoft", "Logitech", "IntelliMouse", "Mouse Systems"
};


static void err_ultrasnd(void)
{
    //              "................................................................................\n"
    fprintf(stderr, "ERROR: In GUS mode but no ULTRASND variable set or is malformed!\n");
    fprintf(stderr, "The ULTRASND environment variable must be set in the following format:\n");
    fprintf(stderr, "\tset ULTRASND=xxx,y,n,z,n\n");
    fprintf(stderr, "Where xxx = port, y = DMA, z = IRQ. n is ignored.\n");
    fprintf(stderr, "Port is set via /gusport xxx option; DMA and IRQ configued via jumper.\n");
}


static void err_blaster(void)
{
    //              "................................................................................\n"
    fprintf(stderr, "ERROR: In SB mode but no BLASTER variable set or is malformed!\n");
    fprintf(stderr, "The BLASTER environment variable must be set in the following format:\n");
    fprintf(stderr, "\tset BLASTER=Axxx Iy Dz T3\n");
    fprintf(stderr, "Where xxx = port, y = IRQ, z = DMA. T3 indicates an SB 2.0 compatible card.\n");
    fprintf(stderr, "Port is set via /sbport xxx option; DMA and IRQ configued via jumper.\n");
}


static void err_pigus(void)
{
    fprintf(stderr, "ERROR: no PicoGUS detected!\n");
}


static void err_protocol(uint8_t expected, uint8_t got)
{
    fprintf(stderr, "ERROR: PicoGUS card using protocol %u, needs %u\n", got, expected);
    fprintf(stderr, "Please run the latest PicoGUS firmware and pgusinit.exe versions together!\n");
    fprintf(stderr, "To flash new firmware, run pgusinit /flash picogus.uf2\n");
}


static int init_gus(void)
{
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

    outp(CONTROL_PORT, CMD_GUSPORT); // Select port register
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


static int init_sb(void)
{
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

    outp(CONTROL_PORT, CMD_SBPORT); // Select port register
    uint16_t tmp_port = inpw(DATA_PORT_LOW);
    if (port != tmp_port) {
        err_blaster();
        return 2;
    }
    return 0;
}


static void print_string(uint8_t cmd)
{
    outp(CONTROL_PORT, 0xCC); // Knock on the door...
    outp(CONTROL_PORT, cmd);  // Select command register

    char str[256] = {0};
    for (uint8_t i = 0; i < 255; ++i) {
        str[i] = inp(DATA_PORT_HIGH);
        if (!str[i]) {
            break;
        }
    }
    puts(str);
}


static bool wait_for_read(const uint8_t value)
{
    for (uint32_t i = 0; i < 6000000; ++i) {    // Up to 6000000, for bigger fws like pg-multi.uf2, waiting for flash erase. If not, timeout and error.
        if (inp(DATA_PORT_HIGH) == value) {
            return true;
        }
    }
    return false;
}


static cdrom_image_status_t wait_for_cd_status(void)
{
    outp(CONTROL_PORT, 0xCC); // Knock on the door...
    outp(CONTROL_PORT, CMD_CDSTATUS); // Select CD image status register
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


static int print_cdimage_list(void)
{
    outp(CONTROL_PORT, 0xCC); // Knock on the door...
    outp(CONTROL_PORT, CMD_CDLOAD); // Get currently loaded index
    uint8_t current_index = inp(DATA_PORT_HIGH);
    pageprintf("Listing CD images on USB drive:\n");

    outp(CONTROL_PORT, CMD_CDLIST); // Select CD image list register
    cdrom_image_status_t cd_status = wait_for_cd_status();
    if (cd_status == CD_STATUS_BUSY) {
        printf("Timeout getting CD image list\n");
        return 99;
    } else if (cd_status == CD_STATUS_ERROR) {
        printf("Error getting CD image list: ");
        print_string(CMD_CDERROR);
        return 99;
    }

    outp(CONTROL_PORT, CMD_CDLIST); // Select CD image list register
    char b[256], c, *p = b;
    uint8_t line = 1;

    while ((c = inp(DATA_PORT_HIGH)) != 4 /* ASCII EOT */) {
        *p++ = c;
        if (!c) {
            putchar(current_index == line ? '*' : ' ');
            pageprintf(" %2d: %s\n", line++, b);
            p = b;
        }
    }

    if (current_index) {
        printf("Currently loaded image marked with \"*\".\n");
    } else {
        printf("No image currently loaded.\n");
    }
    printf("Run \"pgusinit /cdload n\" to load the nth image in the above list, 0 to unload.\n");
    return 0;
}


static int print_cdimage_current(void)
{
    outp(CONTROL_PORT, CMD_CDLOAD); // Get currently loaded index
    uint8_t current_index = inp(DATA_PORT_HIGH);
    if (!current_index) {
        printf("No CD image loaded.\n");
        return 97;
    }
    printf("CD image loaded: ");
    print_string(CMD_CDNAME);
    return 0;
}


static int wait_for_cd_load(void)
{
    cdrom_image_status_t cd_status = wait_for_cd_status();
    if (cd_status == CD_STATUS_BUSY) {
        printf("Timeout loading CD image.\n");
        return 99;
    } else if ((int8_t)cd_status == CD_STATUS_ERROR) {
        printf("Error loading CD image: ");
        print_string(CMD_CDERROR);
        return 98;
    }
    return print_cdimage_current();
}


static void print_cdemu_status(void)
{
    outp(CONTROL_PORT, CMD_CDAUTOADV); // Select joystick enable register
    uint8_t tmp_uint8 = inp(DATA_PORT_HIGH);
    outp(CONTROL_PORT, CMD_CDPORT); // Select port register
    uint16_t tmp_uint16 = inpw(DATA_PORT_LOW); // Get port
    printf("CD-ROM emulation on port %x, image auto-advance %s\n", tmp_uint16, tmp_uint8 ? "enabled" : "disabled");
    
    print_cdimage_current();
}


static void write_settings(void)
{
    outp(CONTROL_PORT, CMD_SAVE); // Select save settings register
    outp(DATA_PORT_HIGH, 0xff);
    printf("Settings saved to flash.\n");
    delay(100);
}


static int reboot_to_firmware(const uint8_t value, const bool permanent)
{
    outp(CONTROL_PORT, 0xCC); // Knock on the door...

    outp(CONTROL_PORT, CMD_BOOTMODE); // Select firmware selection register
    outp(DATA_PORT_HIGH, value); // Send firmware number and permanent flag
    delay(100);

    printf("Mode change requested.\n");
    if (permanent) {
        write_settings();
    }
    printf("Rebooting to fw: %s...\n", modenames[value]);
    outp(CONTROL_PORT, CMD_REBOOT); // Select reboot register
    outp(DATA_PORT_HIGH, 0xff);
    delay(100);

    // Wait for card to reboot to new firmware
    if (!wait_for_read(0xDD)) {
        fprintf(stderr, "ERROR: card is not alive after rebooting to new firmware\n");
        return 99;
    }
    printf("PicoGUS detected: Firmware version: ");
    print_string(CMD_FWSTRING);
    return 0;
}

void print_progress_bar(uint16_t current, uint16_t total)
{
    #define BAR_WIDTH 50

    static uint16_t bar_step = 0; // init flag
    static uint16_t pct_step;
    static uint16_t next_bar, next_pct;
    static uint8_t filled = 0, percent = 0;
    static uint8_t last_filled = 255, last_percent = 255;

    if (bar_step == 0) {
        bar_step = (total + BAR_WIDTH - 1) / BAR_WIDTH; // ceiling division
        pct_step = (total + 99) / 100;
        next_bar = bar_step;
        next_pct = pct_step;
    }

    // Special case: if we're at 100% completion, ensure we show it
    if (current == total) {
        filled = BAR_WIDTH;
        percent = 100;
    } else {
        while (current >= next_bar && filled < BAR_WIDTH) {
            filled++;
            next_bar += bar_step;
        }

        while (current >= next_pct && percent < 100) {
            percent++;
            next_pct += pct_step;
        }
    }

    if (filled != last_filled) {
        // If the bar changed, redraw percentage and bar
        last_filled = filled;
        last_percent = percent;

        fprintf(stderr, "\r%3u%% [", percent);
        for (uint8_t i = 0; i < BAR_WIDTH; i++)
            fputc((i < filled) ? '=' : ' ', stderr);
        fputc(']', stderr);
        fflush(stderr);
    }
    else if (percent != last_percent) {
        // If only the percent changed, redraw just the percent
        last_percent = percent;
        fprintf(stderr, "\r%3u%%", percent);
        fflush(stderr);
    }
}

static int write_firmware(const char* fw_filename)
{
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
    if (protocol < PICOGUS_PROTOCOL_VER) {
        printf("Older fw protocol version %d detected, upgrading firmware in compatibility mode\n", protocol);
    }

    uint16_t numBlocks = 1;
    for (uint16_t i = 0; i < numBlocks; ++i) {
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
            outp(CONTROL_PORT, CMD_FLASH); // Select firmware programming mode
            // Wait a bit for 2nd core on Pico to restart
            delay(100);
            if (!wait_for_read(PICO_FIRMWARE_IDLE)) {
                fprintf(stderr, "ERROR: Card is not in programming mode?\n");
                return 13;
            }
            fflush(stdout);
            fprintf(stderr, "Preparing to program %d blocks...", numBlocks);
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
                outp(CONTROL_PORT, CMD_FLASH); // Select firmware programming mode, which will reboot the card in DONE
            }
        }
        print_progress_bar(i + 1, numBlocks);
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
    print_string(CMD_FWSTRING);
    return 0;
}


static void wifi_printStatus(void)
{
    outp(CONTROL_PORT, 0xCC); // Knock on the door...
    outp(CONTROL_PORT, CMD_WIFISTAT); // Select WiFi status command
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

static void send_string(const uint8_t cmd, const char* str, const int16_t max_len)
{
    outp(CONTROL_PORT, 0xCC); // Knock on the door...
    outp(CONTROL_PORT, cmd);
    char chr;
    for (int16_t i = 0; i < max_len; ++i) {
        if (str[i] == 0) { // End of string
            break;
        }
        outp(DATA_PORT_HIGH, str[i]);
    }
    outp(DATA_PORT_HIGH, 0);
}

static bool cmdDisplayUsage(const char* arg, const int print_all)
{
    usage(gMode, print_all);
    return true;
}

static bool cmdSendBool(const char* arg, const int cmd)
{
    uint8_t value;

    if (!strcmp(arg, "1") || !stricmp(arg, "true") || !stricmp(arg, "on")) {
        value = 1;
    } else if (!strcmp(arg, "0") || !stricmp(arg, "false") || !stricmp(arg, "off")) {
        value = 0;
    } else {
        usage(gMode, false);
        return false;
    }

    outp(CONTROL_PORT, cmd);
    outp(DATA_PORT_HIGH, value);
    return true;
}

static bool cmdSetMode(const char* arg, const int cmd)
{
    if (!stricmp(arg, "TANDY") || !stricmp(arg, "CMS")) {
        // Backwards compatibility for old tandy and cms modes
        newMode = PSG_MODE;
        return true;
    } 
    int j;
    for (j = 1; j < 7; ++j) {
        if (stricmp(modenames[j], arg) == 0) {
            newMode = j;
            return true;
        }
    }
    fprintf(stderr, "Invalid mode %s. Valid modes: gus, sb, mpu, psg, adlib, usb\n");
    return false;
}

static bool ctrlSendUint8(const char *arg, int cmd, int min, int max)
{
    char* endptr;
    uint8_t val = (uint8_t)strtoul(arg, &endptr, 10);

    if (*endptr != '\0' || val < min || val > max) {
        usage(gMode, false);
        return false;
    }

    outp(CONTROL_PORT, cmd);
    outp(DATA_PORT_HIGH, (uint8_t)val);
    return true;
}

static bool cmdSendUint8(const char* arg, const int cmd)
{   
    return ctrlSendUint8(arg, cmd, 0, 255);
}

static bool ctrlSendUint16(const char *arg, int cmd, long min, long max, int base)
{
    char *endptr;
    uint16_t val = (uint16_t)strtoul(arg, &endptr, base);

    if (*endptr != '\0' || val < min || val > max) {
        usage(gMode, false);
        return false;
    }

    outp(CONTROL_PORT, cmd);
    outpw(DATA_PORT_LOW, (uint16_t)val);
    return true;
}

static bool cmdSendUint16(const char* arg, const int cmd)
{
    return ctrlSendUint16(arg, cmd, 0, 65535, 10);
}

static bool cmdSendPort(const char* arg, const int cmd)
{
    return ctrlSendUint16(arg, cmd, 0, 0x3FF, 16);
}

static bool cmdDefaults(const char* arg, const int cmd)
{
    return cmdSendUint8(CMD_DEFAULTS, 0xff);
}

static bool cmdSetVol(const char* arg, const int cmd)
{
    return ctrlSendUint8(arg, cmd, 0, 100);
}

static bool cmdSendMousePort(const char *arg, const int cmd)
{
    char* endptr;
    uint8_t val = (uint8_t)strtoul(arg, &endptr, 10);
    if (*endptr != '\0' || val > 4) {
        usage(gMode, false);
        return false;
    }

    uint16_t port;
    switch (val) {
        case 0: port = 0x000; break;
        case 1: port = 0x3F8; break;
        case 2: port = 0x2F8; break;
        case 3: port = 0x3E8; break;
        case 4: port = 0x2E8; break;
        default:
            usage(gMode, false);
            return false;
    }

    outp(CONTROL_PORT, cmd);
    outpw(DATA_PORT_LOW, port);
    return true;
}

static bool cmdSendMouseSen(const char* arg, const int cmd)
{
    return ctrlSendUint16(arg, cmd, 0, 1024, 10);
}

static bool cmdSendMouseProto(const char* arg, const int cmd)
{
    return ctrlSendUint8(arg, cmd, 0, 3);
}

static bool cmdSendMouseRate(const char* arg, const int cmd)
{
    return ctrlSendUint8(arg, cmd, 20, 200);
}

static bool cmdWifiStatus(const char* arg, const int cmd)
{
    wifi_printStatus();
    exit(0);
}

static bool cmdWifiSSID(const char* arg, const int cmd)
{
    wifichg = true;
    send_string(cmd, arg, 32);
    return true;
}

static bool cmdWifiPass(const char* arg, const int cmd)
{
    wifichg = true;
    send_string(cmd, arg, 63);
    return true;
}

static bool cmdWifiNoPass(const char* arg, const int cmd)
{
    wifichg = true;
    send_string(cmd, "", 1);
    return true;
}

static bool cmdCDLoadName(const char* arg, const int cmd)
{
    send_string(cmd, arg, 127);
    exit(wait_for_cd_load());
}

static bool cmdCDList(const char* arg, const int cmd)
{
    exit(print_cdimage_list());
}

static bool cmdCDLoad(const char* arg, const int cmd)
{
    ctrlSendUint8(arg, cmd, 0, 255);
    exit(wait_for_cd_load());
}

static bool cmdGUSBuffer(const char* arg, const int cmd)
{
    uint8_t tmp_uint8;
    uint8_t e = sscanf(arg, "%hhu", &tmp_uint8);
    if (e != 1 || tmp_uint8 < 1)
    {
        usage(gMode, false);
        return false;
    }
    outp(CONTROL_PORT, cmd);
    outp(DATA_PORT_HIGH, (unsigned char)(tmp_uint8 - 1));
    return true;
}

static bool cmdFlashPico(const char* arg, const int cmd)
{
    if (strlen(arg) > 255)
    {
        usage(INVALID_MODE, false);
        return false;
    }
    write_firmware(arg);
    return true;
}

static bool cmdSave(const char* arg, const int cmd)
{
    permanent = true;
    return true;
}

ParseCommand parseCommandsMinimal[] = {
    {"/?", cmdDisplayUsage, 0, ARG_NONE},
    {"/??", cmdDisplayUsage, 1, ARG_NONE},
    {0}
};
ParseCommand parseCommandsFlash[] = {
    {"/?", cmdDisplayUsage, 0, ARG_NONE},
    {"/??", cmdDisplayUsage, 1, ARG_NONE},
    {"/flash", cmdFlashPico, 0, ARG_REQUIRE, "picogus.uf2"},
    {0}
};
ParseCommand parseCommands[] = {
    {"/flash", cmdFlashPico, 0, ARG_REQUIRE, "picogus.uf2"},
    {"/save", cmdSave, 0, ARG_NONE},
    {"/defaults", cmdDefaults, 0, ARG_NONE},
    {"/?", cmdDisplayUsage, 0, ARG_NONE},
    {"/??", cmdDisplayUsage, 1, ARG_NONE},
    {"/joy", cmdSendBool, CMD_JOYEN, ARG_REQUIRE},
    {"/mode", cmdSetMode, 0, ARG_REQUIRE},
    {"/wtvol", cmdSetVol, CMD_WTVOL, ARG_REQUIRE},
    {"/gus44k", cmdSendBool, CMD_GUS44K, ARG_REQUIRE, "false"},
    {"/gusbuf", cmdGUSBuffer, CMD_GUSBUF, ARG_REQUIRE, "4"},
    {"/gusdma", cmdSendUint8, CMD_GUSDMA, ARG_REQUIRE, "0"},
    {"/gusport", cmdSendPort, CMD_GUSPORT, ARG_REQUIRE, "240"},
    {"/sbport", cmdSendPort, CMD_SBPORT, ARG_REQUIRE, "220"},
    {"/oplport", cmdSendPort, CMD_OPLPORT, ARG_REQUIRE, "388"},
    {"/oplwait", cmdSendBool, CMD_OPLWAIT, ARG_REQUIRE, "false"},
    {"/mpuport", cmdSendPort, CMD_MPUPORT, ARG_REQUIRE, "330"},
    {"/mpudelay", cmdSendBool, CMD_MPUDELAY, ARG_REQUIRE, "false"},
    {"/mpufake", cmdSendBool, CMD_MPUFAKE, ARG_REQUIRE, "false"},
    {"/tandyport", cmdSendPort, CMD_TANDYPORT, ARG_REQUIRE, "2c0"},
    {"/cmsport", cmdSendPort, CMD_CMSPORT, ARG_REQUIRE, "220"},
    {"/mousecom", cmdSendMousePort, CMD_MOUSEPORT, ARG_REQUIRE, "0"},
    {"/mousesen", cmdSendMouseSen, CMD_MOUSESEN, ARG_REQUIRE, "256"},
    {"/mouseproto", cmdSendMouseProto, CMD_MOUSEPROTO, ARG_REQUIRE, "0"},
    {"/mouserate", cmdSendMouseRate, CMD_MOUSERATE, ARG_REQUIRE, "60"},
    {"/ne2kport", cmdSendPort, CMD_NE2KPORT, ARG_REQUIRE, "300"},
    {"/wifistatus", cmdWifiStatus, 0, ARG_NONE},
    {"/wifissid", cmdWifiSSID, CMD_WIFISSID, ARG_REQUIRE},
    {"/wifipass", cmdWifiPass, CMD_WIFIPASS, ARG_REQUIRE},
    {"/wifinopass", cmdWifiNoPass, CMD_WIFIPASS, ARG_NONE},
    {"/cdport", cmdSendPort, CMD_CDPORT, ARG_REQUIRE, "250"},
    {"/cdlist", cmdCDList, 0, ARG_NONE},
    {"/cdload", cmdCDLoad, CMD_CDLOAD, ARG_REQUIRE},
    {"/cdauto", cmdSendBool, CMD_CDAUTOADV, ARG_REQUIRE, "true"},
    {"/cdloadname", cmdCDLoadName, CMD_CDNAME, ARG_REQUIRE},
    {"/mainvol", cmdSetVol, CMD_MAINVOL, ARG_REQUIRE, "100"},
    {"/oplvol", cmdSetVol, CMD_OPLVOL, ARG_REQUIRE, "100"},
    {"/sbvol", cmdSetVol, CMD_SBVOL, ARG_REQUIRE, "100"},
    {"/cdvol", cmdSetVol, CMD_CDVOL, ARG_REQUIRE, "100"},
    {"/gusvol", cmdSetVol, CMD_GUSVOL, ARG_REQUIRE, "100"},
    {"/psgvol", cmdSetVol, CMD_PSGVOL, ARG_REQUIRE, "100"},
    {0}
};
 
ParseCommand *matchCommand(char *str, ParseCommand commands[])
{
    for (int i = 0; commands[i].name != NULL; i++) {
        if (!stricmp(commands[i].name, str)) {
            return &commands[i];
        }
    }
    return NULL;
}

int parseCommand(int argc, char* argv[], int* i, ParseCommand commands[])
{
    bool retVal = false;
    int idx = *i;

    if (!argv[idx]) {
        return retVal;
    }

    ParseCommand *command = matchCommand(argv[idx], commands);

    if (command) {
        char* arg = NULL;
        if (command->type == ARG_REQUIRE) {
            if (idx + 1 >= argc || argv[idx + 1][0] == '/') {
                printf("Error: Command %s requires input argument. ", argv[idx]);
                if (command->def) {
                    printf("Example: %s %s\n", argv[idx], command->def);
                } else {
                    printf("\n");
                }
                return retVal;  
            }
            arg = argv[++(*i)];
        }

        if (!stricmp(argv[idx + 1], "default")) {
            if (command->def) {
                argv[idx + 1] = command->def;
            } else {
                return retVal;
            }
        }
        return command->routine(arg, command->cmd);
    } else {
        printf("Invalid command %s. Run pgusinit /? for usage help", argv[idx]);
    }

    return retVal;
}

static uint8_t ctrlGetUint8(int cmd)
{
    outp(CONTROL_PORT, cmd);
    return inp(DATA_PORT_HIGH);
}

static uint16_t ctrlGetUint16(int cmd)
{
    outp(CONTROL_PORT, cmd);
    return inpw(DATA_PORT_LOW);
}

static void printPicoGus()
{
    printf("USB joystick support %s\n", ctrlGetUint8(CMD_JOYEN) ? "enabled" : "disabled");

    if (board_type == PICOGUS_2) {
        printf("Wavetable volume set to %u\n", ctrlGetUint8(CMD_WTVOL));
    }

    uint16_t mpuPort = ctrlGetUint16(CMD_MPUPORT);
    if (mpuPort) {
        printf("MPU-401: port %x; sysex delay: %s, ", mpuPort, ctrlGetUint8(CMD_MPUDELAY) ? "on" : "off");
        printf("fake all notes off: %s\n", ctrlGetUint8(CMD_MPUFAKE) ? "on" : "off");
    } else {
        printf("MPU-401 disabled\n");
    }
}

static void printGUSMode()
{
    if (init_gus()) {
        return;
    }
    printf("GUS mode: ");
    printf("Audio buffer: %u samples; ", ctrlGetUint8(CMD_GUSBUF) + 1);

    uint8_t tmp_uint8 = ctrlGetUint8(CMD_GUSDMA);
    if (tmp_uint8 == 0) {
        printf("DMA interval: default; ");
    } else {
        printf("DMA interval: %u us; ", tmp_uint8);
    }

    tmp_uint8 = ctrlGetUint8(CMD_GUS44K);
    if (tmp_uint8) {
        printf("Sample rate: fixed 44.1k\n");
    } else {
        printf("Sample rate: variable\n");
    }

    printf("Running in GUS mode on port %x\n", ctrlGetUint16(CMD_GUSPORT));
}

static void printAdlibMode()
{
    printf("Running in AdLib/OPL2 mode on port %x", ctrlGetUint16(CMD_OPLPORT));
    printf("%s\n", ctrlGetUint8(CMD_OPLWAIT) ? ", wait on" : "");
}

static void printUSBMode()
{
    printf("Running in USB mode\n");
    print_cdemu_status();
}

static void printPSGMode()
{
    printf("Running in PSG mode (Tandy 3-voice on port %x, ", ctrlGetUint16(CMD_TANDYPORT));
    printf("CMS/Game Blaster on port %x)\n", ctrlGetUint16(CMD_CMSPORT));
}

static void printSBMode()
{
    if (init_sb()) {
        return;
    }
    printf("Running in Sound Blaster 2.0 mode on port %x ", ctrlGetUint16(CMD_SBPORT));
    uint16_t tmp_uint16 = ctrlGetUint16(CMD_OPLPORT);
    if (tmp_uint16) {
        printf("(AdLib port %x", tmp_uint16);
        printf("%s)\n", ctrlGetUint8(CMD_OPLWAIT) ? ", wait on" : "");
    } else {
        printf("(AdLib port disabled)\n");
    }
    print_cdemu_status();
}

static void printNE2000Mode()
{
    printf("Running in NE2000 mode on port %x\n", ctrlGetUint16(CMD_NE2KPORT));
    wifi_printStatus();
}

static void printMultiMode()
{
    if (gMode == USB_MODE || gMode == PSG_MODE || gMode == ADLIB_MODE)
    {
        uint16_t tmp_uint16 = ctrlGetUint16(CMD_MOUSEPORT);
        printf("Serial Mouse ");
        switch (tmp_uint16)
        {
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
            printf("Mouse report rate: %d Hz, ", ctrlGetUint8(CMD_MOUSERATE));
            printf("protocol: %s\n", mouse_protocol_str[ctrlGetUint8(CMD_MOUSEPROTO)]);

            tmp_uint16 = ctrlGetUint16(CMD_MOUSESEN);
            printf("Mouse sensitivity: %d (%d.%02d)\n", tmp_uint16, (tmp_uint16 >> 8), ((tmp_uint16 & 0xFF) * 100) >> 8);
        }
    }
}

static void printVolume()
{
    printf("Volume: ");
    printf("Main: %u    ", ctrlGetUint8(CMD_MAINVOL));
    
    if (gMode == GUS_MODE) {
        printf("GUS: %u     ", ctrlGetUint8(CMD_GUSVOL));
    }
    if (gMode == SB_MODE) {
        printf("SB: %u    ", ctrlGetUint8(CMD_SBVOL));
    } 
    if (gMode == SB_MODE || gMode == ADLIB_MODE) {
        printf("OPL: %u    ", ctrlGetUint8(CMD_OPLVOL));
    } 
    if (gMode == SB_MODE || gMode == USB_MODE) {
        printf("CD: %u    ", ctrlGetUint8(CMD_CDVOL));
    } 
    if (gMode == PSG_MODE) {
        printf("PSG: %u     ", ctrlGetUint8(CMD_PSGVOL));
    }
    printf("\n");
}

typedef enum {
    INIT_OK,
    INIT_FW_MISMATCH,
    INIT_NOT_DETECTED
} init_status;

static init_status initPicoGUS()
{
    // Get magic value from port on PicoGUS that is not on real GUS
    outp(CONTROL_PORT, 0xCC); // Knock on the door...
    outp(CONTROL_PORT, CMD_MAGIC); // Select magic string register
    if (inp(DATA_PORT_HIGH) != 0xDD) {
        err_pigus();
        return INIT_NOT_DETECTED;
    };
    printf("PicoGUS detected: Firmware version: ");
    print_string(CMD_FWSTRING);

    outp(CONTROL_PORT, 0x01); // Select protocol version register
    uint8_t protocol_got = inp(DATA_PORT_HIGH);
    if (PICOGUS_PROTOCOL_VER != protocol_got) {
      err_protocol(PICOGUS_PROTOCOL_VER, protocol_got);
      return INIT_FW_MISMATCH;
    }

    outp(CONTROL_PORT, CMD_BOOTMODE); // Select mode register
    gMode = inp(DATA_PORT_HIGH);

    outp(CONTROL_PORT, CMD_HWTYPE); // Select hardware type register
    board_type = inp(DATA_PORT_HIGH);
    if (board_type == PICO_BASED) {
        printf("Hardware: PicoGUS v1.x or PicoGUS Femto\n");
    } else if (board_type == PICOGUS_2) {
        printf("Hardware: PicoGUS v2.0\n");
    } else {
        printf("Hardware: Unknown\n");
    }
    printf("\n");
    return INIT_OK;
}


int main(int argc, char* argv[]) {
    is_console = isatty(fileno(stdout));
    if (is_console) {
        page_lines = get_screen_lines() - 1;
    }

    banner();
    init_status status = initPicoGUS();
    if (status == INIT_FW_MISMATCH) {
        // Still allow firmware upgrade if firmware version mismatches
        for(int i = 1; i < argc; ++i) {
            if (!parseCommand(argc, argv, &i, parseCommandsFlash)) {
                return 1;
            } else {
                return 0;
            }
        }
        return 1;
    } else if (status == INIT_NOT_DETECTED) {
        // Still allow /? and /?? commands if not detected
        for(int i = 1; i < argc; ++i) {
            if (!parseCommand(argc, argv, &i, parseCommandsMinimal)) {
                return 1;
            }
        }
        return 1;
    }

    int commands = 0;
    for(int i = 1; i < argc; ++i) {
        if (!parseCommand(argc, argv, &i, parseCommands)) {
            return 1;
        }
    }

    // If mode was set in commands, apply it
    if (newMode) {
        return reboot_to_firmware(newMode, permanent);
    }

    if (wifichg) {
        ctrlSendUint8("0", CMD_WIFIAPPLY, 0, 255);
    }

    printPicoGus();
    printVolume();

    switch(gMode) {
    case GUS_MODE:
        printGUSMode();
        break;
    case ADLIB_MODE:
        printAdlibMode();
        break;
    case MPU_MODE:
        printf("Running in MPU-401 only mode (with IRQ)\n");
        break;
    case USB_MODE:
        printUSBMode();
        break;
    case PSG_MODE:
        printPSGMode();
        break;
    case SB_MODE:
        printSBMode();
        break;
    case NE2000_MODE:
        printNE2000Mode();
        break;
    default:
        printf("Running in unknown mode (maybe upgrade pgusinit?)\n");
        break;
    }
    printMultiMode();
    printf("PicoGUS initialized!\n");

    if (permanent) {
        write_settings();
    }

    return 0;
}

