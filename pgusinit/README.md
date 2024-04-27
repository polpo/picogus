# PicoGUSinit v2.1.5

PicoGUSinit (also called pgusinit, after the program's .exe file) detects and
initializes a PicoGUS card. In GUS emulation mode, it should be used instead of
ULTRINIT.

For more info on PicoGUS, see https://github.com/polpo/picogus.

The board must be loaded with firmware before using it for the first time. See
https://github.com/polpo/picogus/wiki/Building-your-PicoGUS#programming-the-pico
for programming instructions.

pgusinit must be run with firmware it is compatible with. If run with an
incompatible firmware, pgusinit will complain. One exception to this is that
pgusinit can be used to upgrade an older version of firmware to a more recent
version. For example, firmware v0.7.0, which is normally incompatibile with
pgusinit v2.0.0, can be upgraded to firmware v1.0.0 with pgusinit v2.0.0.

## Using

Simply run `PGUSINIT.EXE` to detect and initialize your card with default
settings. Options may be given for other settings:

### Global options

* `/?` - shows help for PicoGUSinit. Detects the current card mode and only
  shows options valid for that mode.
* `/f firmware.uf2` - uploads firmware in the file named firmware.uf2 to the
  PicoGUS.
* `/j` - enables game port joystick emulation mode (disabled by default).
* `/v x` - sets wavetable header volume to x percent (PicoGUS 2.0 boards only).
  Available in all modes in case you want to use the wavetable header as an aux
  input (for example for an internal CD-ROM drive).
* `/m x [d]` - changes operation mode (reboots card to x firmware, only if 
   pg-multi.uf2 is flashed) for x: (1=gus, 2=sb, 3=mpu, 4=tandy, 5=cms, 6=joy)
   the optional parameter 'd' makes the selected mode permanent at system boot

Firmware files that come with the releases:

* `pg-gus.uf2` - GUS emulation
* `pg-sb.uf2` - Sound Blaster/AdLib emulation
* `pg-mpu.uf2` - MPU-401 with intelligent mode emulation
* `pg-tandy.uf2` - Tandy 3-Voice emulation
* `pg-cms.uf2` - CMS/Game Blaster emulation
* `pg-joyex.uf2` - Joystick exclusive mode (doesn't emulate any sound cards,
  just the game port)
* `pg-multi.uf2` - Multi-firmware. Contains all 6 firmwares in one. To be used
  with /m x option in pgusinit to change mode.

### GUS emulation mode

GUS emulation mode requires the ULTRASND variable to be set, in the format:

`set ULTRASND=240,1,1,5,5` where 240 is the PicoGUS's port, 1 is the DMA, and 5
is the IRQ. The port on the PicoGUS will be programmed to use the port
specified in ULTRASND.

* `/a n` - sets the audio buffer size to n samples. Defaults to 4 with a
  minimum of 1 and maximum of 256. Some programs require a different value to
  run properly. Going lower than 4 is not advisable.
* `/d n` - sets the DMA interval to n microseconds. Games that use streaming
  audio over DMA work better with higher values. Doom, for example, runs well
  with a value of 10-12. Note that increasing this will slow down sample
  loading. Set to 0 to use the GUS's default DMA interval handling, where the
  DMA interval is set by the program using it.
* `/4` - enables fixed 44.1kHz output. Normally the GF1 varies its output
  sample rate from 44.1kHz at 14 voices to 19.2kHz at 32 voices. Using this
  option enables 44.1kHz output for all numbers of voices, similar to the
  Interwave. This will result in stuttering in most games that use streaming DMA
  for sound effects like Doom, hence it is EXPERIMENTAL.

See the Compatibility List wiki for notes on programs that require these
options to be set: https://github.com/polpo/picogus/wiki/Compatibility-list

### Sound Blaster/AdLib, MPU-401, Tandy, and CMS emulation modes

* `/p x` - sets the base port of the emulated card to x. Defaults to 220 for
  Sound Blaster, 330 for MPU-401, 2C0 for Tandy, and 220 for CMS.

### Sound Blaster/AdLib mode

* `/o x` - sets the base port of the OPL/AdLib. Defaults to 388.
* `/w` - wait on OPL2 data write. Can fix speed-sensitive early AdLib games on
  fast systems (example: 688 Attack Sub).

### MPU-401 emulation mode

* `/s` - enable sysex delay to prevent buffer overflows on older MPU-401
  revisions.
* `/n` - Fake all-notes-off for the Roland RA-50.

## Compiling

PicoGUSinit can be compiled with OpenWatcom 1.9 or 2.0. In DOS with OpenWatcom
installed, run `wmake` to compile.
