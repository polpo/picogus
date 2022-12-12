# PicoGUSinit

PicoGUSinit (PGUSINIT.EXE) detects and initializes a PicoGUS card in GUS
emulation mode. It should be used instead of ULTRINIT.

## Using

PicoGUSinit requires the ULTRASND variable to be set, in the format:

`set ULTRINIT=240,1,1,5,5` where 240 is the PicoGUS's port, 1 is the DMA, and 5
is the IRQ.

Simply run `PGUSINIT.EXE`.

### Options

`/?` - shows help for PicoGUSinit.

`/a x` - sets the audio buffer size in samples. Defaults to 16 with a minimum of
8 and maximum of 256. Some programs require a different value to run properly.
See the Compatibility List wiki for notes on productions that require this to be
set: https://github.com/polpo/picogus/wiki/Compatibility-list

## Compiling

PicoGUSinit can be compiled with OpenWatcom 1.9 or 2.0. In DOS with OpenWatcom
installed, run `make` to compile.
