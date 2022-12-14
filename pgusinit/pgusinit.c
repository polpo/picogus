#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


void banner(void) {
    printf("PicoGUSinit v0.1.1\n");
    printf("(c) 2022 Ian Scott - licensed under the GNU GPL v2\n\n");
}


void usage(void) {
    fprintf(stderr, "usage: pgusinit [/?] [/a n]\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "\t/?   - show this message\n");
    fprintf(stderr, "\t/a n - set audio buffer to n samples. Default: 16, Min: 8, Max: 256\n");
    fprintf(stderr, "\t       (tweaking this can help demos that hang or have audio glitches)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "The ULTRASND environment variable must be set in the following format:\n");
    fprintf(stderr, "\tset ULTRASND=xxx,y,n,z,n\n");
    fprintf(stderr, "Where xxx = port, y = DMA, z = IRQ. n is ignored.\n");
    fprintf(stderr, "Port is hardcoded to 240 in PicoGUS firmware; DMA and IRQ configued via jumper.\n");
}


void err_ultrasnd(void) {
    fprintf(stderr, "ERROR: no ULTRASND variable set or is malformed!\n");
    usage();
}


void err_pigus(void) {
    fprintf(stderr, "ERROR: no PicoGUS detected!\n"); 
}


int main(int argc, char* argv[]) {
    int e;
    unsigned short buffer_size = 0;

    banner();
    if (strcmp(argv[1], "/?") == 0) {
        usage();
        return 0;
    }

    if (strcmp(argv[1], "/a") == 0) {
        if (argc != 3) {
            usage();
            return 255;
        }
        e = sscanf(argv[2], "%hu", &buffer_size);
        if (e != 1 || buffer_size < 8 || buffer_size > 256) {
            usage();
            return 3;
        }
    }

    char* ultrasnd = getenv("ULTRASND");
    if (ultrasnd == NULL) {
        err_ultrasnd();
        return 1;
    }

    // Parse ULTRASND
    unsigned short port;
    unsigned char irq;
    unsigned char dma;
    e = sscanf(ultrasnd, "%hx,%hhu,%*hhu,%hhu,%*hhu", &port, &irq, &dma);
    if (e != 3) {
        err_ultrasnd();
        return 2;
    }

    // Read from 0x103 to prime the Pi. The first read will come in late!
    inp(port + 0x103);

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
        err_pigus();
        return 98;
    }
    printf("GUS-like card detected...\n");

    // Get magic value from port on PicoGUS that is not on real GUS
    if (inp(port + 0x2) != 0xDD) {
        err_pigus();
        return 99;
    };

    // Enable IRQ latches
    outp(port, 0x8);
    // Select reset register
    outp(port + 0x103, 0x4C);
    // Master reset to run. DAC enable and IRQ enable will be done by the application.
    outp(port + 0x105, 0x1);

    if (!buffer_size) {
        buffer_size = 16;
    }
    outp(port + 0x2, (unsigned char)(buffer_size - 1));
    printf("Audio buffer size set to %u samples\n", buffer_size);
    
    printf("PicoGUS detected and initialized!\n");
    return 0;
}
