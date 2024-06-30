#include <conio.h>
#include <dos.h> 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <i86.h>

#define CONTROL_PORT 0x1D0
#define DATA_PORT_LOW  0x1D1
#define DATA_PORT_HIGH 0x1D2
#define PICOGUS_PROTOCOL_VER 2

typedef enum {
    PICO_FIRMWARE_IDLE = 0,
    PICO_FIRMWARE_WRITING = 1,
    PICO_FIRMWARE_DONE = 0xFE,
    PICO_FIRMWARE_ERROR = 0xFF
} pico_firmware_status_t;

typedef enum { PICO_BASED = 0, PICOGUS_2 = 1 } board_type_t;

typedef enum {
    GUS_MODE = 0,
    ADLIB_MODE = 1,
    MPU_MODE = 2,
    TANDY_MODE = 3,
    CMS_MODE = 4,
    SB_MODE = 5,
    JOYSTICK_ONLY_MODE = 0x0f
} card_mode_t;

static const char *fwnames[8] = {
    "INVALID",
    "GUS",
    "SB",
    "MPU",
    "TANDY",
    "CMS",
    "JOY"
};

void banner(void) {
    printf("PicoGUSinit v3.0.0 (c) 2024 Ian Scott - licensed under the GNU GPL v2\n\n");
}

const char* usage_by_card[] = {
    "[/a n] [/d n] [/4]",       // GUS_MODE
    "[/p x]",                   // ADLIB_MODE
    "[/p x] [/v x] [/s] [/n]",  // MPU_MODE
    "[/p x]",                   // TANDY_MODE
    "[/p x]",                   // CMS_MODE
    "[/p x] [/o x] [/w]",       // SB_MODE
    "", "", "", "", "", "", "", "", "", // future expansion
    "",                         // JOYSTICK_ONLY_MODE
};

void usage(char *argv0, card_mode_t mode) {
    // Max line length @ 80 chars:
    //              "................................................................................\n"
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s [/?] | [/f fw.uf2] | [/m x [d]]", argv0);
    if (mode <= CMS_MODE) {
        fprintf(stderr, " | [/j] %s", usage_by_card[mode]);
    }
    fprintf(stderr, "\n\n");
    fprintf(stderr, "    /?        - show this message\n");
    fprintf(stderr, "    /f fw.uf2 - program the PicoGUS with the firmware file fw.uf2\n");
    fprintf(stderr, "    /j        - enable USB joystick support\n");
    fprintf(stderr, "    /v x      - set volume of the wavetable header. Scale 0-100, Default: 100\n");
    fprintf(stderr, "                (for PicoGUS v2.0 boards only)\n");
    fprintf(stderr, "    /m x      - change card mode to x (gus, sb, mpu, tandy, cms, joy)\n");
    fprintf(stderr, "    /s        - save settings to the card to persist on system boot\n");
    if (mode > GUS_MODE && mode < JOYSTICK_ONLY_MODE) {
        fprintf(stderr, "Sound Blaster/AdLib, MPU-401, Tandy, CMS modes only:\n");
        fprintf(stderr, "    /p x - set the (hex) base port address of the emulated card. Defaults:\n");
        fprintf(stderr, "           Sound Blaster: 220; MPU-401: 330; Tandy: 2C0; CMS: 220\n");
        //              "................................................................................\n"
    }
    if (mode == SB_MODE) {
        fprintf(stderr, "    /o x - set the base address of the OPL2/AdLib on the SB. Default: 388\n");
        fprintf(stderr, "    /w   - wait on OPL2 data write. Can fix speed-sensitive early AdLib games\n");
        //              "................................................................................\n"
    }
    if (mode == MPU_MODE) {
        fprintf(stderr, "MPU-401 mode only:\n");
        fprintf(stderr, "    /x   - delay SYSEX (for rev.0 Roland MT-32)\n");
        fprintf(stderr, "    /n   - fake all notes off (for Roland RA-50)\n");
        //              "................................................................................\n"
    }
    if (mode == GUS_MODE) {
        fprintf(stderr, "GUS mode only:\n");
        fprintf(stderr, "    /a n - set audio buffer to n samples. Default: 4, Min: 1, Max: 256\n");
        fprintf(stderr, "           (tweaking this can help programs that hang or have audio glitches)\n");
        fprintf(stderr, "    /d n - force DMA interval to n us. Default: 0, Min: 0, Max: 255\n");
        fprintf(stderr, "           Specifying 0 restores the GUS default behavior.\n");
        fprintf(stderr, "           (if games with streaming audio like Doom stutter, increase this)\n");
        fprintf(stderr, "    /4   - Enable fixed 44.1kHz output for all active voice #s [EXPERIMENTAL]\n");
        fprintf(stderr, "The ULTRASND environment variable must be set in the following format:\n");
        fprintf(stderr, "\tset ULTRASND=xxx,y,n,z,n\n");
        fprintf(stderr, "Where xxx = port, y = DMA, z = IRQ. n is ignored.\n");
        fprintf(stderr, "Port is set on the card according to ULTRASND; DMA and IRQ configued via jumper.");
        //              "................................................................................\n"
    }
}


void err_ultrasnd(char *argv0) {
    fprintf(stderr, "ERROR: no ULTRASND variable set or is malformed!\n");
    usage(argv0, GUS_MODE);
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
    uint8_t irq;
    uint8_t dma;
    int e;
    e = sscanf(ultrasnd, "%hx,%hhu,%*hhu,%hhu,%*hhu", &port, &irq, &dma);
    if (e != 3) {
        err_ultrasnd(argv0);
        return 2;
    }

    outp(CONTROL_PORT, 0x04); // Select port register
    outpw(DATA_PORT_LOW, port); // Write port

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

void print_firmware_string(void) {
    outp(CONTROL_PORT, 0xCC); // Knock on the door...
    outp(CONTROL_PORT, 0x02); // Select firmware string register

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
    outp(CONTROL_PORT, 0xE1); // Select save settings register
    outp(DATA_PORT_HIGH, 0xff);
    printf("Settings saved to flash.\n");
    delay(100);
}

// For multifw
int reboot_to_firmware(const uint8_t value, const bool permanent) {
    outp(CONTROL_PORT, 0xCC); // Knock on the door...

    outp(CONTROL_PORT, 0xE0); // Select firmware selection register
    outp(DATA_PORT_HIGH, value); // Send firmware number and permanent flag
            delay(100);

    printf("\nMode change requested.\n");
    if (permanent) {
        write_settings();
    }
    printf("Rebooting to fw: %s...\n", fwnames[value]);
    outp(CONTROL_PORT, 0xE2); // Select reboot register
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

int write_firmware(const char* fw_filename, uint8_t protocol) {
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
            outp(CONTROL_PORT, 0xFF); // Select firmware programming mode
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
            } else if (protocol == 2) {
                if (!wait_for_read(PICO_FIRMWARE_DONE)) {
                    fprintf(stderr, "\nERROR: Card has written last firmware byte but is not done\n");
                    return 15;
                }
                outp(CONTROL_PORT, 0xCC); // Knock on the door...
                outp(CONTROL_PORT, 0xFF); // Select firmware programming mode, which will reboot the card in DONE
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

int main(int argc, char* argv[]) {
    int e;
    unsigned short buffer_size = 0;
    unsigned short dma_interval = 0;
    uint8_t force_44k = 0;
    uint16_t port_override = 0;
    uint16_t opl_port_override = 0;
    uint8_t wt_volume;
    char fw_filename[256] = {0};
    card_mode_t mode;
    uint8_t mpu_delaysysex = 0;
    uint8_t mpu_fakeallnotesoff = 0;
    uint8_t enable_joystick = 0;
    uint8_t adlib_wait = 0;
    char mode_name[8] = {0};
    int fw_num = 8;
    bool permanent = false;

    banner();
    // Get magic value from port on PicoGUS that is not on real GUS
    outp(CONTROL_PORT, 0xCC); // Knock on the door...
    outp(CONTROL_PORT, 0x00); // Select magic string register
    if (inp(DATA_PORT_HIGH) != 0xDD) {
        err_pigus();
        return 99;
    };
    printf("PicoGUS detected: ");
    print_firmware_string();

    outp(CONTROL_PORT, 0x03); // Select mode register
    mode = inp(DATA_PORT_HIGH);

    // Default wavetable volume to 100 in MPU mode, 0 otherwise
    wt_volume = (mode == MPU_MODE) ? 100 : 0;

    int i = 1;
    while (i < argc) {
        if (stricmp(argv[i], "/?") == 0) {
            usage(argv[0], mode);
            return 0;
        } else if (stricmp(argv[i], "/j") == 0) {
            enable_joystick = 1;
        } else if (stricmp(argv[i], "/m") == 0) {               
            if (i + 1 >= argc) {
                usage(argv[0], mode);
                return 255;
            }
            e = sscanf(argv[++i], "%7s", mode_name);
            if (e != 1) {
                usage(argv[0], mode);
                return 5;
            }
        } else if (stricmp(argv[i], "/4") == 0) {
            force_44k = 1;
        } else if (stricmp(argv[i], "/a") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0], mode);
                return 255;
            }
            e = sscanf(argv[++i], "%hu", &buffer_size);
            if (e != 1 || buffer_size < 1 || buffer_size > 256) {
                usage(argv[0], mode);
                return 3;
            }
        } else if (stricmp(argv[i], "/d") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0], mode);
                return 255;
            }
            e = sscanf(argv[++i], "%hu", &dma_interval);
            if (e != 1 || dma_interval > 255) {
                usage(argv[0], mode);
                return 4;
            }
        } else if (stricmp(argv[i], "/p") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0], mode);
                return 255;
            }
            e = sscanf(argv[++i], "%hx", &port_override);
            if (e != 1 || port_override > 0x3ffu) {
                usage(argv[0], mode);
                return 4;
            }
        } else if (stricmp(argv[i], "/o") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0], mode);
                return 255;
            }
            e = sscanf(argv[++i], "%hx", &opl_port_override);
            if (e != 1 || opl_port_override > 0x3ffu) {
                usage(argv[0], mode);
                return 4;
            }
        } else if (stricmp(argv[i], "/w") == 0) {
            adlib_wait = 1;
        } else if (stricmp(argv[i], "/v") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0], mode);
                return 255;
            }
            e = sscanf(argv[++i], "%hu", &wt_volume);
            if (e != 1 || wt_volume > 100) {
                usage(argv[0], mode);
                return 4;
            }
        } else if (stricmp(argv[i], "/x") == 0) {
            mpu_delaysysex = 1;
        } else if (stricmp(argv[i], "/n") == 0) {
            mpu_fakeallnotesoff = 1;
        } else if (stricmp(argv[i], "/f") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0], mode);
                return 255;
            }
            e = sscanf(argv[++i], "%255s", fw_filename);
            if (e != 1) {
                usage(argv[0], mode);
                return 5;
            }
        } else if (stricmp(argv[i], "/s") == 0) {
            permanent = true;
        }
        ++i;
    }

    outp(CONTROL_PORT, 0x01); // Select protocol version register
    uint8_t protocol_got = inp(DATA_PORT_HIGH);
    if (PICOGUS_PROTOCOL_VER != protocol_got) {
        if (fw_filename[0] && protocol_got == 1) {
          printf("Older fw protocol version 1 detected, upgrading firmware in compatibility mode\n");
        } else {
          err_protocol(PICOGUS_PROTOCOL_VER, protocol_got);
          return 97;
        }
    }

    if (fw_filename[0]) {
        return write_firmware(fw_filename, protocol_got);
    }

    if (mode_name[0]) {
        int i;
        for (i = 1; i < 8; ++i) {
            if (strnicmp(fwnames[i], mode_name, 7) == 0) {
                fw_num = i;
                break;
            }
        }
        if (i == 8) {
            usage(argv[0], mode);
            return 255;
        }
        return reboot_to_firmware(fw_num, permanent);
    }

    outp(CONTROL_PORT, 0xf0); // Select hardware type register
    board_type_t board_type = inp(DATA_PORT_HIGH);
    if (board_type == PICO_BASED) {
        printf("Hardware: PicoGUS v1.x or PicoGUS Femto\n");
    } else if (board_type == PICOGUS_2) {
        printf("Hardware: PicoGUS v2.0\n");
    } else {
        printf("Hardware: Unknown\n");
    }
    printf("\n");

    uint16_t port;
    uint16_t opl_port;
    if (mode != GUS_MODE) {
        if (port_override) {
            outp(CONTROL_PORT, 0x04); // Select port register
            outpw(DATA_PORT_LOW, port_override); // Write port
        }

        outp(CONTROL_PORT, 0x04); // Select port register
        port = inpw(DATA_PORT_LOW); // Get port
        if (mode == SB_MODE) {
            if (opl_port_override) {
                outp(CONTROL_PORT, 0x05); // Select OPL port register
                outpw(DATA_PORT_LOW, opl_port_override); // Write port
            }
            outp(CONTROL_PORT, 0x05); // Select OPL port register
            opl_port = inpw(DATA_PORT_LOW); // Get port
        }
    }

    outp(CONTROL_PORT, 0x0f); // Select joystick enable register
    outp(DATA_PORT_HIGH, enable_joystick);
    printf("USB joystick support %s\n", enable_joystick ? "enabled" : "disabled (use /j to enable)");

    if (board_type == PICOGUS_2) {
        outp(CONTROL_PORT, 0x20); // Select wavetable volume register
        outp(DATA_PORT_HIGH, wt_volume); // Write volume
        printf("Wavetable volume set to %u\n", wt_volume);
    }

    switch(mode) {
    case GUS_MODE:
        init_gus(argv[0]);
        if (!buffer_size) {
            buffer_size = 4;
        }
        outp(CONTROL_PORT, 0x10); // Select audio buffer register
        outp(DATA_PORT_HIGH, (unsigned char)(buffer_size - 1));
        printf("Audio buffer size set to %u samples\n", buffer_size);
        
        outp(CONTROL_PORT, 0x11); // Select DMA interval register
        outp(DATA_PORT_HIGH, dma_interval);
        if (dma_interval == 0) {
            printf("DMA interval set to default behavior\n");
        } else {
            printf("DMA interval forced to %u us\n", dma_interval);
        }

        outp(CONTROL_PORT, 0x12); // Select force 44k buffer
        outp(DATA_PORT_HIGH, force_44k);
        if (force_44k) {
            printf("Fixed 44.1kHz output enabled (EXPERIMENTAL)\n");
        }
        
        outp(CONTROL_PORT, 0x04); // Select port register
        port = inpw(DATA_PORT_LOW); // Get port
        printf("Running in GUS mode on port %x\n", port);
        break;
    case ADLIB_MODE:
        printf("Running in AdLib/OPL2 mode on port %x\n", port);
        break;
    case MPU_MODE:
        printf("MPU-401 sysex delay: %s, fake all notes off: %s\n", mpu_delaysysex ? "on" : "off", mpu_fakeallnotesoff ? "on" : "off");
        outp(CONTROL_PORT, 0x21); // Select MPU settings register
        outp(DATA_PORT_HIGH, mpu_delaysysex | (mpu_fakeallnotesoff << 1)); // Write sysex delay and fake all notes off settings
        printf("Running in MPU-401 mode on port %x\n", port);
        break;
    case TANDY_MODE:
        printf("Running in Tandy 3-voice mode on port %x\n", port);
        break;
    case CMS_MODE:
        printf("Running in CMS/Game Blaster mode on port %x\n", port);
        break;
    case SB_MODE:
        outp(CONTROL_PORT, 0x30); // Select Adlib wait register
        outp(DATA_PORT_HIGH, adlib_wait); // Write sysex delay and fake all notes off settings
        printf("Running in Sound Blaster 2.0 mode on port %x (AdLib port %x%s)\n",
               port, opl_port, adlib_wait ? ", wait on" : "");
        break;
    case JOYSTICK_ONLY_MODE:
        printf("Running in Joystick exclusive mode on port 201\n");
    default:
        printf("Running in unknown mode on port %x (maybe upgrade pgusinit?)\n", port);
        break;
    }
    printf("PicoGUS initialized!\n");

    if (permanent) {
        write_settings();
    }

    return 0;
}
