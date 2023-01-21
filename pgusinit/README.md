# PicoGUSinit

PicoGUSinit (PGUSINIT.EXE) detects and initializes a PicoGUS card. In GUS
emulation mode, it should be used instead of ULTRINIT.

For more info on PicoGUS, see https://github.com/polpo/picogus.

The board must be loaded with firmware before using it for the first time. See
https://github.com/polpo/picogus/wiki/Building-your-PicoGUS#programming-the-pico
for programming instructions.

## Using

Simply run `PGUSINIT.EXE` to detect and initialize your card with default
settings. Options may be given for other settings:

### Global options

* `/?` - shows help for PicoGUSinit.
* `/f firmware.uf2` - uploads firmware in the file named firmware.uf2 to the
  PicoGUS.

Firmware files that come with the releases:

* `pg-gus.uf2` - GUS emulation
* `pg-adlib.uf2` - AdLib emulation
* `pg-mpu.uf2` - MPU-401 with intelligent mode emulation

### GUS emulation mode

GUS emulation mode requires the ULTRASND variable to be set, in the format:

`set ULTRASND=240,1,1,5,5` where 240 is the PicoGUS's port, 1 is the DMA, and 5
is the IRQ. The port on the PicoGUS will be programmed to use the port
specified in ULTRASND.

* `/a n` - sets the audio buffer size to n samples. Defaults to 16 with a
  minimum of 8 and maximum of 256. Some programs require a different value to
  run properly.
* `/d n` - sets the DMA interval to n microseconds. Games that use streaming
  audio over DMA work better with higher values. Doom, for example, runs well
  with a value of 10-12. Note that increasing this will slow down sample
  loading. Set to 0 to use the GUS's default DMA interval handling, where the
  DMA interval is set by the program using it.

See the Compatibility List wiki for notes on programs that require these
options to be set: https://github.com/polpo/picogus/wiki/Compatibility-list

### AdLib and MPU-401 emulation modes

* `/p x` - sets the base port of the emulated card to x. Defaults to 388 for
  AdLib and 330 for MPU-401.

## Compiling

PicoGUSinit can be compiled with OpenWatcom 1.9 or 2.0. In DOS with OpenWatcom
installed, run `wmake` to compile.
