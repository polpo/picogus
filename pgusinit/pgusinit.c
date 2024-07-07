#include <conio.h>
#include <dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <i86.h>

#include "../common/picogus.h"

void banner(void) {
    printf("PicoGUSinit v3.0.0 (c) 2024 Ian Scott - licensed under the GNU GPL v2\n\n");
}


void usage(char *argv0, card_mode_t mode, bool print_all) {
    // Max line length @ 80 chars:
    //     "................................................................................\n"
    printf("Usage:\n");
    printf("   /?            - show this message (/?? to show options for all modes)\n");
    printf("   /flash fw.uf2 - program the PicoGUS with the firmware file fw.uf2\n");
    printf("   /wtvol x      - set volume of wavetable header. Scale 0-100, Default: 100\n");
    printf("                   (for PicoGUS v2.0 boards only)\n");
    printf("   /mode x       - change card mode to x (gus, sb, mpu, tandy, cms, adlib, usb)\n");
    printf("   /defaults     - set all settings for all modes to defaults\n");
    printf("   /save         - save settings to the card to persist on system boot\n");
    printf("   /joy 1|0      - enable/disable USB joystick support, Default: 0\n");
    printf("\n");
    if (mode == GUS_MODE || print_all) {
        //     "................................................................................\n"
        printf("GUS settings:\n");
        printf("   /gusport x  - set the base port of the GUS. Default: 240\n");
        printf("   /gusbuf n   - set audio buffer to n samples. Default: 4, Min: 1, Max: 256\n");
        printf("                 (tweaking can help programs that hang or have audio glitches)\n");
        printf("   /gusdma n   - force DMA interval to n us. Default: 0, Min: 0, Max: 255\n");
        printf("                 Specifying 0 restores the GUS default behavior.\n");
        printf("                 (increase to fix games with streaming audio like Doom)\n");
        printf("   /gus44k 1|0 - Fixed 44.1kHz output for all active voice #s [EXPERIMENTAL]\n");
        printf("\n");
    }
    if (mode == SB_MODE || print_all) {
        //     "................................................................................\n"
        printf("Sound Blaster settings:\n");
        printf("   /sbport x    - set the base port of the Sound Blaster. Default: 220\n");
    }
    if (mode == SB_MODE || mode == ADLIB_MODE || print_all) {
        printf("AdLib settings:\n");
        printf("   /oplport x   - set the base port of the OPL2/AdLib on the SB. Default: 388\n");
        printf("   /oplwait 1|0 - wait on OPL2 write. Can fix speed-sensitive early AdLib games\n");
        printf("\n");
    }
    if (mode == MPU_MODE || print_all) {
        //     "................................................................................\n"
        printf("MPU-401 settings:\n");
        printf("   /mpuport x    - set the base port of the MPU-401. Default: 330\n");
        printf("   /mpudelay 1|0 - delay SYSEX (for rev.0 Roland MT-32)\n");
        printf("   /mpufake 1|0  - fake all notes off (for Roland RA-50)\n");
        printf("\n");
    }
    if (mode == TANDY_MODE || print_all) {
        //     "................................................................................\n"
        printf("Tandy settings:\n");
        printf("   /tandyport x - set the base port of the Tandy 3-voice. Default: 2C0\n");
        printf("\n");
    }
    if (mode == CMS_MODE || print_all) {
        //     "................................................................................\n"
        printf("CMS settings:\n");
        printf("   /cmsport x - set the base port of the CMS. Default: 220\n");
        printf("\n");
    }
    if (mode == USB_MODE || mode == CMS_MODE || mode == TANDY_MODE || mode == ADLIB_MODE || print_all) {
        //     "................................................................................\n"
        printf("Serial Mouse settings:\n");
        printf("   /mousecom n - mouse COM port. Default: 0, Choices: 0 (disable), 1, 2, 3, 4\n");
        printf("   /mouseproto n - set mouse protocol. Default: 0 (Microsoft)\n");
        printf("          0 - Microsoft Mouse 2-button,      1 - Logitech 3-button\n");
        printf("          2 - IntelliMouse 3-button + wheel, 3 - Mouse Systems 3-button\n");
        printf("   /mouserate n  - set report rate in Hz. Default: 60, Min: 20, Max: 200\n");
        printf("          (increase for smoother cursor movement, decrease for lower CPU load)\n");
        printf("   /mousesen n   - set mouse sensitivity (256 - 100%, 128 - 50%, 512 - 200%)\n");
    }
    /*
    if (mode == GUS_MODE) {
        fprintf(stderr, "The ULTRASND environment variable must be set in the following format:\n");
        fprintf(stderr, "\tset ULTRASND=xxx,y,n,z,n\n");
        fprintf(stderr, "Where xxx = port, y = DMA, z = IRQ. n is ignored.\n");
        fprintf(stderr, "Port is set on the card according to ULTRASND; DMA and IRQ configued via jumper.");
    }
    */
}

const char* mouse_protocol_str[] = {
    "Microsoft", "Logitech", "IntelliMouse", "Mouse Systems"
};


void err_ultrasnd(char *argv0) {
    fprintf(stderr, "ERROR: no ULTRASND variable set or is malformed!\n");
    usage(argv0, GUS_MODE, false);
}


void err_blaster(char *argv0) {
    fprintf(stderr, "ERROR: no BLASTER variable set or is malformed!\n");
    usage(argv0, SB_MODE, false);
}


void err_pigus(void) {
    fprintf(stderr, "ERROR: no PicoGUS detected!\n");
}


void err_protocol(uint8_t expected, uint8_t got) {
    fprintf(stderr, "ERROR: PicoGUS card using protocol %u, needs %u\n", got, expected);
    fprintf(stderr, "Please run the latest PicoGUS firmware and pgusinit.exe versions together!\n");
}


int init_gus(char *argv0) {
    char* ultrasnd = getenv("ULTRASND");
    if (ultrasnd == NULL) {
        err_ultrasnd(argv0);
        return 1;
    }

    // Parse ULTRASND
    uint16_t port;
    int e;
    e = sscanf(ultrasnd, "%hx,%*hhu,%*hhu,%*hhu,%*hhu", &port);
    if (e != 1) {
        err_ultrasnd(argv0);
        return 2;
    }

    outp(CONTROL_PORT, MODE_GUSPORT); // Select port register
    uint16_t tmp_port = inpw(DATA_PORT_LOW);
    if (port != tmp_port) {
        err_ultrasnd(argv0);
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


int init_sb(char *argv0) {
    char* blaster = getenv("BLASTER");
    if (blaster == NULL) {
        err_blaster(argv0);
        return 1;
    }

    // Parse BLASTER
    uint16_t port;
    int e;
    e = sscanf(blaster, "A%hx I%*hhu D%*hhu T3", &port);
    if (e != 1) {
        err_blaster(argv0);
        return 2;
    }

    outp(CONTROL_PORT, MODE_SBPORT); // Select port register
    uint16_t tmp_port = inpw(DATA_PORT_LOW);
    if (port != tmp_port) {
        err_blaster(argv0);
        return 2;
    }
}


void print_firmware_string(void) {
    outp(CONTROL_PORT, 0xCC); // Knock on the door...
    outp(CONTROL_PORT, MODE_FWSTRING); // Select firmware string register

    char firmware_string[256] = {0};
    for (int i = 0; i < 255; ++i) {
        firmware_string[i] = inp(DATA_PORT_HIGH);
        if (!firmware_string[i]) {
            break;
        }
    }
    printf("Firmware version: %s\n", firmware_string);
}


bool wait_for_read(const uint8_t value) {
    for (uint32_t i = 0; i < 6000000; ++i) {    // Up to 6000000, for bigger fws like pg-multi.uf2, waiting for flash erase. If not, timeout and error.
        if (inp(DATA_PORT_HIGH) == value) {
            return true;
        }
    }
    return false;
}

void write_settings(void) {
    outp(CONTROL_PORT, MODE_SAVE); // Select save settings register
    outp(DATA_PORT_HIGH, 0xff);
    printf("Settings saved to flash.\n");
    delay(100);
}

// For multifw
int reboot_to_firmware(const uint8_t value, const bool permanent) {
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
    printf("PicoGUS detected: ");
    print_firmware_string();
    return 0;
}

int write_firmware(const char* fw_filename) {
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
    printf("PicoGUS detected: ");
    print_firmware_string();
    return 0;
}

#define process_bool_opt(option) \
if (i + 1 >= argc) { \
    usage(argv[0], mode, false); \
    return 255; \
} \
e = sscanf(argv[++i], "%1[01]", tmp_arg); \
if (e != 1) { \
    usage(argv[0], mode, false); \
    return 5; \
} \
option = (tmp_arg[0] == '1') ? 1 : 0;


#define process_port_opt(option) \
if (i + 1 >= argc) { \
    usage(argv[0], mode, false); \
    return 255; \
} \
e = sscanf(argv[++i], "%hx", &option); \
if (e != 1 || option > 0x3ffu) { \
    usage(argv[0], mode, false); \
    return 4; \
}

int main(int argc, char* argv[]) {
    int e;
    uint8_t enable_joystick = 0;
    uint8_t wtvol;
    char fw_filename[256] = {0};
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
    printf("PicoGUS detected: ");
    print_firmware_string();

    int i = 1;
    // /flash option is special and can work across protocol versions, so if it's specified, that's all we do
    while (i < argc) {
        if (stricmp(argv[i], "/flash") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0], INVALID_MODE, false);
                return 255;
            }
            e = sscanf(argv[++i], "%255s", fw_filename);
            if (e != 1) {
                usage(argv[0], INVALID_MODE, false);
                return 5;
            }
            return write_firmware(fw_filename);
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
            usage(argv[0], mode, false);
            return 0;
        } else if (stricmp(argv[i], "/??") == 0) {
            usage(argv[0], mode, true);
            return 0;
        } else if (stricmp(argv[i], "/joy") == 0) {
            process_bool_opt(tmp_uint8);
            outp(CONTROL_PORT, MODE_JOYEN); // Select joystick enable register
            outp(DATA_PORT_HIGH, tmp_uint8);
        } else if (stricmp(argv[i], "/mode") == 0) {               
            if (i + 1 >= argc) {
                usage(argv[0], mode, false);
                return 255;
            }
            e = sscanf(argv[++i], "%7s", mode_name);
            if (e != 1) {
                usage(argv[0], mode, false);
                return 5;
            }
        } else if (stricmp(argv[i], "/wtvol") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0], mode, false);
                return 255;
            }
            e = sscanf(argv[++i], "%hhu", &tmp_uint8);
            if (e != 1 || tmp_uint8 > 100) {
                usage(argv[0], mode, false);
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
                usage(argv[0], mode, false);
                return 255;
            }
            e = sscanf(argv[++i], "%hhu", &tmp_uint8);
            if (e != 1 || tmp_uint8 < 1) {
                usage(argv[0], mode, false);
                return 3;
            }
            outp(CONTROL_PORT, MODE_GUSBUF); // Select audio buffer register
            outp(DATA_PORT_HIGH, (unsigned char)(tmp_uint8 - 1));
        } else if (stricmp(argv[i], "/gusdma") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0], mode, false);
                return 255;
            }
            e = sscanf(argv[++i], "%hhu", &tmp_uint8);
            if (e != 1) {
                usage(argv[0], mode, false);
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
                usage(argv[0], mode, false);
                return 255;
            }
            e = sscanf(argv[++i], "%hhu", &tmp_uint8);
            if (e != 1 || tmp_uint8 > 3) {
                usage(argv[0], mode, false);
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
                usage(argv[0], mode, false);
                return 4;
            }
            outp(CONTROL_PORT, MODE_MOUSEPORT); // Select mouse port register
            outpw(DATA_PORT_LOW, tmp_uint16);
        } else if (stricmp(argv[i], "/mousesen") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0], mode, false);
                return 255;
            }
            e = sscanf(argv[++i], "%hi", &tmp_uint16);
            if (e != 1) {
                usage(argv[0], mode, false);
                return 4;
            }
            outp(CONTROL_PORT, MODE_MOUSESEN); // Select mouse sensitivity register
            outpw(DATA_PORT_LOW, tmp_uint16);
        } else if (stricmp(argv[i], "/mouseproto") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0], mode, false);
                return 255;
            }
            e = sscanf(argv[++i], "%hhu", &tmp_uint8);
            if (e != 1 || tmp_uint8 > 3) {
                usage(argv[0], mode, false);
                return 4;
            }
            outp(CONTROL_PORT, MODE_MOUSEPROTO);
            outp(DATA_PORT_HIGH, tmp_uint8);
        } else if (stricmp(argv[i], "/mouserate") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0], mode, false);
                return 255;
            }
            e = sscanf(argv[++i], "%hhu", &tmp_uint8);
            if (e != 1 || tmp_uint8 < 20 || tmp_uint8 > 200) {
                usage(argv[0], mode, false);
                return 4;
            }
            outp(CONTROL_PORT, MODE_MOUSERATE);
            outp(DATA_PORT_HIGH, tmp_uint8);
        } else {
            printf("Unknown option: %s\n", argv[i]);
            usage(argv[0], mode, false);
            return 255;
        }
        ++i;
    }


    if (mode_name[0]) {
        int i;
        for (i = 1; i < 8; ++i) {
            if (strnicmp(modenames[i], mode_name, 7) == 0) {
                fw_num = i;
                break;
            }
        }
        if (i == 8) {
            usage(argv[0], mode, false);
            return 255;
        }
        return reboot_to_firmware(fw_num, permanent);
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

    switch(mode) {
    case GUS_MODE:
        if (init_gus(argv[0])) {
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
        outp(CONTROL_PORT, MODE_MPUDELAY); // Select force 44k buffer
        tmp_uint8 = inp(DATA_PORT_HIGH);
        printf("MPU-401 sysex delay: %s; ", tmp_uint8 ? "on" : "off");
        outp(CONTROL_PORT, MODE_MPUFAKE); // Select force 44k buffer
        tmp_uint8 = inp(DATA_PORT_HIGH);
        printf("fake all notes off: %s\n", tmp_uint8 ? "on" : "off");
        outp(CONTROL_PORT, MODE_MPUPORT); // Select port register
        tmp_uint16 = inpw(DATA_PORT_LOW); // Get port
        printf("Running in MPU-401 mode on port %x\n", tmp_uint16);
        break;
    case TANDY_MODE:
        outp(CONTROL_PORT, MODE_TANDYPORT); // Select port register
        tmp_uint16 = inpw(DATA_PORT_LOW); // Get port
        printf("Running in Tandy 3-voice mode on port %x\n", tmp_uint16);
        break;
    case CMS_MODE:
        outp(CONTROL_PORT, MODE_CMSPORT); // Select port register
        tmp_uint16 = inpw(DATA_PORT_LOW); // Get port
        printf("Running in CMS/Game Blaster mode on port %x\n", tmp_uint16);
        break;
    case SB_MODE:
        if (init_sb(argv[0])) {
            return 1;
        }
        outp(CONTROL_PORT, MODE_SBPORT); // Select port register
        tmp_uint16 = inpw(DATA_PORT_LOW); // Get port
        printf("Running in Sound Blaster 2.0 mode on port %x ", tmp_uint16);
        outp(CONTROL_PORT, MODE_OPLPORT); // Select port register
        tmp_uint16 = inpw(DATA_PORT_LOW); // Get port
        printf("(AdLib port %x", tmp_uint16);
        outp(CONTROL_PORT, MODE_OPLWAIT); // Select Adlib wait register
        tmp_uint8 = inp(DATA_PORT_HIGH);
        printf("%s)\n", tmp_uint8 ? ", wait on" : "");
        break;
    default:
        printf("Running in unknown mode (maybe upgrade pgusinit?)\n");
        break;
    }
    if (mode == USB_MODE || mode == CMS_MODE || mode == TANDY_MODE || mode == ADLIB_MODE) {
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

