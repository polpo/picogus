#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


void banner(void) {
    printf("PiGUSinit v0.0.1\n\n");
}


void usage(void) {
    fprintf(stderr, "usage: pgusinit [/?]\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "\t/? - show this message\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "The ULTRASND environment variable must be set in the following format:\n");
    fprintf(stderr, "\tset ULTRASND=xxx,y,n,z,n\n");
    fprintf(stderr, "Where xxx = port, y = DMA, z = IRQ. n is ignored.\n");
    fprintf(stderr, "Port is configured via config file on the Raspberry Pi; DMA and IRQ via jumper.\n");
}


void err_ultrasnd(void) {
    fprintf(stderr, "ERROR: no ULTRASND variable set or is malformed!\n");
    usage();
}


void err_pigus(void) {
    fprintf(stderr, "ERROR: no PiGUS detected!\n"); 
}


int main(int argc, char* argv[]) {
    banner();
    if (strcmp(argv[1], "/?") == 0) {
        usage();
        return 0;
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
    int e = sscanf(ultrasnd, "%hx,%hhu,%*hhu,%hhu,%*hhu", &port, &irq, &dma);
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

    // Get magic value from port on PiGUS that is not on real GUS
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
    
    printf("PiGUS detected and initialized!\n");
    return 0;
}
