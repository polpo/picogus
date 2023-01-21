#include <conio.h>
#include <dos.h> 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <i86.h>

#define CONTROL_PORT 0x1D0
#define DATA_PORT_LOW  0x1D1
#define DATA_PORT_HIGH 0x1D2
#define PICOGUS_PROTOCOL_VER 1

typedef enum {
    PICO_FIRMWARE_IDLE = 0,
    PICO_FIRMWARE_WRITING = 1,
    PICO_FIRMWARE_ERROR = 0xFF
} pico_firmware_status_t;


void banner(void) {
    printf("PicoGUSinit v1.0.0\n");
    printf("(c) 2023 Ian Scott - licensed under the GNU GPL v2\n\n");
}


void usage(void) {
    // Max line length @ 80 chars:
    //              "................................................................................\n"
    fprintf(stderr, "usage: pgusinit [/?] | [/b x] [/a n] [/d n] | [/f fw.uf2]\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "    /?   - show this message\n");
    fprintf(stderr, "    /f fw.uf2 - Program the PicoGUS with the firmware file fw.uf2.\n");
    fprintf(stderr, "AdLib and MPU-401 mode only:\n");
    fprintf(stderr, "    /b x - set the base address of the emulated card. Defaults:\n");
    fprintf(stderr, "           AdLib: 388; MPU-401: 330\n");
    fprintf(stderr, "GUS mode only:\n");
    fprintf(stderr, "    /a n - set audio buffer to n samples. Default: 16, Min: 8, Max: 256\n");
    fprintf(stderr, "           (tweaking this can help programs that hang or have audio glitches)\n");
    fprintf(stderr, "    /d n - force DMA interval to n ms. Default: 0, Min: 1, Max: 256\n");
    fprintf(stderr, "           Specifying 0 restores the GUS default behavior.\n");
    fprintf(stderr, "           (try /d 10 with games with streaming audio like Doom...)\n");
    fprintf(stderr, "The ULTRASND environment variable must be set in the following format:\n");
    fprintf(stderr, "\tset ULTRASND=xxx,y,n,z,n\n");
    fprintf(stderr, "Where xxx = port, y = DMA, z = IRQ. n is ignored.\n");
    fprintf(stderr, "Port is set on the card according to ULTRASND; DMA and IRQ configued via jumper.");
    //              "................................................................................\n"
}


void err_ultrasnd(void) {
    fprintf(stderr, "ERROR: no ULTRASND variable set or is malformed!\n");
    usage();
}


void err_pigus(void) {
    fprintf(stderr, "ERROR: no PicoGUS detected!\n"); 
}


void err_protocol(uint8_t expected, uint8_t got) {
    fprintf(stderr, "ERROR: PicoGUS card using protocol %u, needs %u\n", expected, got); 
    fprintf(stderr, "Please run the latest PicoGUS firmware and pgusinit.exe versions together!\n");
}


int init_gus(void) {
    char* ultrasnd = getenv("ULTRASND");
    if (ultrasnd == NULL) {
        err_ultrasnd();
        return 1;
    }

    // Parse ULTRASND
    uint16_t port;
    uint8_t irq;
    uint8_t dma;
    int e;
    e = sscanf(ultrasnd, "%hx,%hhu,%*hhu,%hhu,%*hhu", &port, &irq, &dma);
    if (e != 3) {
        err_ultrasnd();
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
            if (inp(DATA_PORT_HIGH) != PICO_FIRMWARE_IDLE) {
                fprintf(stderr, "ERROR: Card is not in programming mode?\n");
                return 13;
            }
            fflush(stdout);
            fprintf(stderr, "Programming");
        }

        if (i != uf2_buf.uf2.blockNo) {
            fprintf(stderr, "\nERROR: file %s is not a valid UF2 file - block mismatch\n", fw_filename);
            return 14;
        }

        for (uint16_t b = 0; b < 512; ++b) {
            // Write firmware byte
            outp(DATA_PORT_HIGH, uf2_buf.buf[b]);
            if (i < (numBlocks - 1) || b < 511) { // If it's not the very last byte
                if (inp(DATA_PORT_HIGH) != PICO_FIRMWARE_WRITING) {
                    fprintf(stderr, "\nERROR: Card is not in firmware writing mode?\n");
                    return 15;
                }
            }
        }
        fprintf(stderr, ".");
        //fprintf(stderr, "%u ", i);
    }
    fclose(fp);

    // Wait for card to reboot
    printf("\nProgramming complete. Waiting for the card to reboot...\n");
    sleep(2);
    if (inp(DATA_PORT_HIGH) != 0xDD) {
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
    uint16_t port_override = 0;
    char fw_filename[256] = {0};

    banner();
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "/?") == 0) {
            usage();
            return 0;
        } else if (strcmp(argv[i], "/a") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 255;
            }
            e = sscanf(argv[++i], "%hu", &buffer_size);
            if (e != 1 || buffer_size < 8 || buffer_size > 256) {
                usage();
                return 3;
            }
        } else if (strcmp(argv[i], "/d") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 255;
            }
            e = sscanf(argv[++i], "%hu", &dma_interval);
            if (e != 1 || dma_interval > 256) {
                usage();
                return 4;
            }
        } else if (strcmp(argv[i], "/p") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 255;
            }
            e = sscanf(argv[++i], "%hx", &port_override);
            if (e != 1 || port_override > 0x3ffu) {
                usage();
                return 4;
            }
        } else if (strcmp(argv[i], "/f") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 255;
            }
            e = sscanf(argv[++i], "%255s", fw_filename);
            if (e != 1) {
                usage();
                return 5;
            }
        }
        ++i;
    }

    // Get magic value from port on PicoGUS that is not on real GUS
    outp(CONTROL_PORT, 0xCC); // Knock on the door...
    outp(CONTROL_PORT, 0x00); // Select magic string register
    if (inp(DATA_PORT_HIGH) != 0xDD) {
        err_pigus();
        return 99;
    };
    printf("PicoGUS detected: ");
    print_firmware_string();
    printf("\n");

    outp(CONTROL_PORT, 0x01); // Select protocol version register
    uint8_t protocol_got = inp(DATA_PORT_HIGH);
    if (PICOGUS_PROTOCOL_VER != protocol_got) {
        err_protocol(PICOGUS_PROTOCOL_VER, protocol_got);
        return 97;
    }

    if (fw_filename[0]) {
        return write_firmware(fw_filename);
    }

    outp(CONTROL_PORT, 0x03); // Select mode register
    uint8_t mode = inp(DATA_PORT_HIGH);

    uint16_t port;
    if (mode != 0) {
        if (port_override) {
            outp(CONTROL_PORT, 0x04); // Select port register
            outpw(DATA_PORT_LOW, port_override); // Write port
        }

        outp(CONTROL_PORT, 0x04); // Select port register
        port = inpw(DATA_PORT_LOW); // Get port
    }

    switch(mode) {
    case 0:
        init_gus();
        if (!buffer_size) {
            buffer_size = 16;
        }
        outp(CONTROL_PORT, 0x10); // Select audio buffer register
        outp(DATA_PORT_HIGH, (unsigned char)(buffer_size - 1));
        printf("Audio buffer size set to %u samples\n", buffer_size);
        
        outp(CONTROL_PORT, 0x11); // Select DMA interval register
        outp(DATA_PORT_HIGH, dma_interval);
        if (dma_interval == 0) {
            printf("DMA interval set to default behavior\n");
        } else {
            printf("DMA interval forced to %u ms\n", dma_interval);
        }
        outp(CONTROL_PORT, 0x04); // Select port register
        port = inpw(DATA_PORT_LOW); // Get port
        printf("Running in GUS mode on port %x\n", port);
        break;
    case 1:
        printf("Running in AdLib/OPL2 mode on port %x\n", port);
        break;
    case 2:
        printf("Running in MPU-401 mode on port %x\n", port);
        break;
    }
    printf("PicoGUS initialized!\n");

    return 0;
}
