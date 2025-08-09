# PicoGUSinit

PicoGUSinit (also called pgusinit, after the program's .exe file) detects and
initializes a PicoGUS card. In GUS emulation mode, it should be used instead of
ULTRINIT.

For more info on PicoGUS, see https://picog.us/.

The board must be loaded with firmware before using it for the first time. See
https://github.com/polpo/picogus/wiki/Building-your-PicoGUS#programming-the-pico
for programming instructions if your card didn't come programmed. The `/flash
x` option to pgusinit can be used to upgrade firmware on a running PicoGUS.
Firmware files that comes with release:

* `picogus.uf2` - All modes are now in a single firmware. To switch between
  modes, use the `/mode x` option in pgusinit.
* `pg-ne2k.uf2` - Special firmware for NE2000/WiFi. Only works on PicoGUS
  Femto and PicoGUS v1.x boards with a Pico W installed.

pgusinit must be run with firmware it is compatible with. If run with an
incompatible firmware, pgusinit will complain about a protocol mismatch. One
exception to this is that pgusinit can be used to upgrade an older version of
firmware to a more recent version. For example, firmware v1.2.0, which is
normally incompatibile with pgusinit v3.0.0, can be upgraded to firmware
v2.0.0 with pgusinit v3.0.0.

## Using

Simply run `PGUSINIT.EXE` to detect and initialize your card with default
settings. Options may be given for other settings:

### Global options

* `/?` - shows help for PicoGUSinit. Detects the current card mode and only
  shows options valid for that mode.
* `/??` - shows help for PicoGUSinit for all modes.
* `/flash picogus.uf2` - uploads firmware in the file named picogus.uf2 to the
  PicoGUS. Used to upgrade firmware version.
* `/wtvol x` - sets wavetable header volume to x percent (PicoGUS 2.0 boards
  only).  Available in all modes in case you want to use the wavetable header
  as an aux input (for example for an internal CD-ROM drive).
* `/mainvol x` - sets main volume level for all outputs.
* `/mode x` - changes the card to mode specified by x. Options:
    - `gus`: Gravis Ultrasound
    - `sb`: Sound Blaster 2.0 & AdLib. Supports CD-rom emulation.
    - `mpu`: MPU-401 with intelligent mode and IRQ support
    - `psg`: Tandy 3-voice and CMS/Game Blaster. Supports USB serial mouse
      emulation.
    - `adlib`: AdLib only. Supports USB serial mouse emulation.
    - `usb`: Only emulates game port joystick and serial mouse via USB. Can be
      used if you don't want to emulate any sound cards. Supports CD-ROM
      emulation.
* `/save` - saves current settings to the card. Saved settings persist across
  reboots, meaning pgusinit does not need to be run. The card mode is also
  saved.
* `/defaults` - restores all card settings to defaults.
* `/joy 1|0` - enable USB joystick support with 1, disable with 0.

### GUS options

GUS emulation requires the ULTRASND variable to be set, in the format:

`set ULTRASND=240,1,1,5,5` where 240 is the PicoGUS's port, 1 is the DMA, and
5 is the IRQ. pgusinit will check that the port set on the PicoGUS matches the
port specified in ULTRASND.

* `/gusport x` - sets the base I/O port of the GUS to x. Defaults to 240.
* `/gusbuf n` - sets the audio buffer size to n samples. Defaults to 4 with a
  minimum of 1 and maximum of 256. Some programs require a different value to
  run properly. Going lower than 4 is not advisable.
* `/gusdma n` - sets the DMA interval to n microseconds. Games that use
  streaming audio over DMA work better with higher values. Doom, for example,
  runs well with a value of 10-12. Note that increasing this will slow sample
  loading. Set to 0 to use the GUS's default DMA interval handling, where the
  DMA interval is set by the program using it.
* `/gus44k 1|0` - setting to 1 enables fixed 44.1kHz output. Normally the GF1
  varies its output sample rate from 44.1kHz at 14 voices to 19.2kHz at 32
  voices. Using this option enables 44.1kHz output for all numbers of voices,
  similar to the Interwave. This will result in stuttering in most games that
  use streaming DMA for sound effects like Doom, hence it is EXPERIMENTAL.
* `/gusvol x` - sets the volume of the GUS output to x percent.

See the Compatibility List wiki for notes on programs that require special
settings for the above options:
https://github.com/polpo/picogus/wiki/Compatibility-list

### Sound Blaster/AdLib options

* `/sbport x` - sets the base port of the Sound Blaster. Defaults to 220.
* `/oplport x` - sets the base port of the OPL/AdLib. Defaults to 388.
* `/oplwait` - wait on OPL2 data write. Can fix speed-sensitive early AdLib
  games on fast systems (example: 688 Attack Sub).
* `/oplvol x` - sets the volume of the OPL2 output to x percent.

### MPU-401 options

* `/mpuport x` - sets the base port of the MPU-401. Defaults to 330.
* `/mpudelay` - set to 1 to enable sysex delay to prevent buffer overflows on
  older MPU-401 revisions.
* `/mpufake` - set to 1 to fake all-notes-off for the Roland RA-50.

### Tandy options

* `/tandyport x` - sets the base port of the Tandy 3-voice. Defaults to 2c0.
* `/psgvol x` - sets the volume of the PSG output to x percent.

### CMS options

* `/cmsport x` - sets the base port of the CMS. Defaults to 220.
* `/psgvol x` - sets the volume of the PSG output to x percent.

### Serial mouse settings

Since a COM port requires an IRQ, serial mouse emulation is possible in modes
that do not require an IRQ: AdLib, CMS, Tandy, and USB.

* `/mousecom n` - mouse COM port. Default: 0, Choices: 0 (disable), 1, 2, 3, 4
* `/mouseproto n` - set mouse protocol. Default: 0 (Microsoft). Choices:
     0 - Microsoft Mouse 2-button,      1 - Logitech 3-button
     2 - IntelliMouse 3-button + wheel, 3 - Mouse Systems 3-button
* `/mouserate n` - set report rate in Hz. Default: 60, Min: 20, Max: 200.
  Increase for smoother cursor movement, decrease for lower CPU load
* `/mousesen n` - set mouse sensitivity (256 - 100%, 128 - 50%, 512 - 200%)

### CD-ROM emulation options

* `/cdport x` - set base port of CD interface. Default: 250, 0 to disable
* `/cdlist` - list CD images on the inserted USB drive
* `/cdload n` - load image n in the list given by /cdlist. 0 to unload image
* `/cdloadname x` - load CD image by name. Names with spaces can be quoted
* `/cdauto 1|0` - auto-advance loaded image when same USB drive is reinserted
* `/cdvol x` - sets the volume of the CD audio output to x percent.

## Compiling

PicoGUSinit can be compiled with OpenWatcom 1.9 or 2.0. Run `wmake` to compile.
