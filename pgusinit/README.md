# PicoGUSinit

PicoGUSinit (also called pgusinit, after the program's .exe file) detects and
initializes a PicoGUS card. In GUS emulation mode, it should be used instead of
ULTRINIT.

For more info on PicoGUS, see https://picog.us/.

The board must be loaded with firmware before using it for the first time. See
https://github.com/polpo/picogus/wiki/Building-your-PicoGUS#programming-the-pico
for programming instructions if your card didn't come programmed. The `/flash
x` option to pgusinit can be used to upgrade firmware on a running PicoGUS.
Firmware files that come with the release:

* `picogus.uf2` - All modes are now in a single firmware. To switch between
  modes, use the `/mode x` option in pgusinit.
* `pg-ne2k.uf2` - Special firmware for NE2000/WiFi. Only works on PicoGUS
  Femto and PicoGUS v1.x boards with a Pico W installed.

pgusinit must be run with firmware it is compatible with. If run with an
incompatible firmware, pgusinit will complain about a protocol mismatch. One
exception to this is that pgusinit can be used to upgrade an older version of
firmware to a more recent version. For example, firmware v1.2.0, which is
normally incompatible with pgusinit v3.0.0, can be upgraded to firmware
v2.0.0 with pgusinit v3.0.0. When a protocol mismatch is detected, only the
`/flash` option is accepted. If no PicoGUS is detected at all, only `/?` and
`/??` are accepted.

## Using

Simply run `PGUSINIT.EXE` to detect and initialize your card with default
settings. Options may be given for other settings.

Options that take a numeric value also accept `default` to restore that
setting's default (for example, `/gusbuf default`). On/off options accept `1`,
`true` or `on` to enable and `0`, `false` or `off` to disable.

`/mode` takes effect before any other options, so the other options on the
same command line apply to the new mode after the card reboots into it.

### Global options

* `/?` - shows help for PicoGUSinit. Detects the current card mode and only
  shows options valid for that mode.
* `/??` - shows help for PicoGUSinit for all modes.
* `/flash picogus.uf2` - uploads firmware in the file named picogus.uf2 to the
  PicoGUS. Used to upgrade firmware version.
* `/wtvol x` - sets wavetable header volume to x percent (PicoGUS 2.0 boards
  only). Default: 100. Available in all modes in case you want to use the
  wavetable header as an aux input (for example for an internal CD-ROM drive).
* `/mainvol x` - sets main volume level for all outputs to x percent.
* `/mode x` - changes the card to mode specified by x. Options:
    - `gus`: Gravis Ultrasound
    - `sb`: Sound Blaster (1.x, 2.0, Pro, or 16, see `/sbtype`) & AdLib.
      Supports CD-ROM emulation.
    - `mpu`: MPU-401 with intelligent mode and IRQ support
    - `psg`: Tandy 3-voice and CMS/Game Blaster. Supports USB serial mouse
      emulation.
    - `adlib`: AdLib only. Supports USB serial mouse emulation.
    - `usb`: Only emulates game port joystick and serial mouse via USB. Can be
      used if you don't want to emulate any sound cards. Supports CD-ROM
      emulation.
* `/save` - saves current settings to the card. Saved settings persist across
  reboots, meaning pgusinit does not need to be run unless you're changing
  options. The card mode is also saved.
* `/defaults` - restores all card settings to defaults.
* `/joy 1|0` - enable USB joystick support with 1, disable with 0. Default: 0

### GUS options

GUS emulation requires the ULTRASND variable to be set, in the format:

`set ULTRASND=240,1,1,5,5` where 240 is the PicoGUS's port, 1 is the DMA, and
5 is the IRQ. pgusinit will check that the port set on the PicoGUS matches the
port specified in ULTRASND. It also checks that both DMA values match, both
IRQ values match, and that they are supported by PicoGUS: DMA 1 or 3, IRQ 2/9,
3, 5 or 7. IRQ 4 is not supported by real GUS cards and is not recommended.

* `/gusenv` - sets the base I/O port from ULTRASND variable.
* `/gusport x` - sets the base I/O port of the GUS to x. Defaults to 240.
* `/gusbuf n` - sets the audio buffer size to n samples. Defaults to 4 with a
  minimum of 1 and maximum of 256. Some programs require a different value to
  run properly. Going lower than 4 is not advisable.
* `/gusdma n` - sets the DMA interval to n microseconds. Games that use
  streaming audio over DMA work better with higher values. Doom, for example,
  runs well with a value of 10-12. Note that increasing this will slow sample
  loading. Set to 0 to use the GUS's default DMA interval handling, where the
  DMA interval is set by the program using it. Default: 0, Max: 255.
* `/gus44k 1|0` - setting to 1 enables fixed 44.1kHz output. Normally the GF1
  varies its output sample rate from 44.1kHz at 14 voices to 19.2kHz at 32
  voices. Using this option enables 44.1kHz output for all numbers of voices,
  similar to the Interwave. This will result in stuttering in most games that
  use streaming DMA for sound effects like Doom, hence it is EXPERIMENTAL.
  Default: 0
* `/gusvol x` - sets the volume of the GUS output to x percent.

See the Compatibility List wiki for notes on programs that require special
settings for the above options:
https://github.com/polpo/picogus/wiki/Compatibility-list

### Sound Blaster/AdLib options

Sound Blaster emulation requires the BLASTER variable to be set, in the format:

`set BLASTER=A220 I5 D1 H1 P330 T6`, where 220 is the PicoGUS's SB emulation
port, 5 is the IRQ, 1 is the DMA, 330 is the MPU-401 port, and 6 is the
emulated SB type (see `/sbtype` option).
The high DMA (`H`) is required for SB16 (`T6`) and must match the low DMA
(`D`), since PicoGUS has a single DMA channel. The MPU-401 port (`Pxxx`) is
optional and only is used by SB16. Supported resources are DMA 1 or 3 (1 is
the only option for SB 1.x/2.0) and IRQ 2/9, 3, 4, 5 or 7.

pgusinit checks that the BLASTER variable matches the settings on the card and
reports an error if they differ.

* `/sbenv` - sets SB base port, IRQ, DMA and type from BLASTER variable. Also
  sets the MPU-401 port if `Pxxx` is present.
* `/sbport x` - sets the base port of the Sound Blaster. Defaults to 220.
* `/sbirq x` - sets the IRQ of the Sound Blaster. Must match jumper settings.
  Defaults to 5.
* `/sbdma x` - sets the DMA of the Sound Blaster. Must match jumper settings.
  Defaults to 1.
* `/sbtype x` - sets the Sound Blaster type. Defaults to 6. Allowed values are:
  1 - SB 1.x,          2 - SB Pro 1 (dual OPL2), 3 - SB 2.0
  4 - SB Pro 2 (OPL3), 6 - SB 16
* `/oplport x` - sets the base port of the OPL/AdLib. Defaults to 388, 0 to
  disable.
* `/oplwait 1|0` - wait on OPL2 data write. Can fix speed-sensitive early AdLib
  games on fast systems (example: 688 Attack Sub).
* `/sbvol x` - sets the volume of the SB PCM output to x percent.
* `/oplvol x` - sets the volume of the OPL2 output to x percent.
* `/sbfixtc 1|0` - fix SB time constant for 11/22/44 kHz sample rate. Can fix
  PCM audio slightly pitched up or down in pre-SB16 games and applications.
  Note that certain programs are already aware of SB time constant inaccuracy
  and will sound wrong if this option is enabled. Default = 0 (disabled)
* `/sblockmixer n` - lock writes to SB mixer settings. Choices:
  0 - write lock disabled (default)
  1 - lock all but Voice Volume registers (for panning effects e.g. in Wolf3D)
  2 - lock all registers

### MPU-401 options

These options are available in all modes.

* `/mpuport x` - sets the base port of the MPU-401. Defaults to 330, 0 to
  disable.
* `/mpudelay 1|0` - set to 1 to enable sysex delay to prevent buffer overflows
  on rev.0 Roland MT-32 units. Default: 0
* `/mpufake 1|0` - set to 1 to fake all-notes-off for the Roland RA-50.
  Default: 0
* `/mpuwait 1|0` - set to 1 to add extra wait on busy status reads (for fast
  PCs). Default: 0

### PSG (Tandy/CMS) options

* `/tandyport x` - sets the base port of the Tandy 3-voice. Defaults to 2c0, 0
  to disable.
* `/cmsport x` - sets the base port of the CMS. Defaults to 220, 0 to disable.
* `/psgvol x` - sets the volume of the PSG output to x percent.

### Serial mouse settings

Since a COM port requires an IRQ, serial mouse emulation is possible in modes
that do not require an IRQ: AdLib, PSG (Tandy/CMS), and USB.

* `/mousecom n` - mouse COM port. Default: 0, Choices: 0 (disable), 1, 2, 3, 4
* `/mouseproto n` - set mouse protocol. Default: 0 (Microsoft). Choices:
     0 - Microsoft Mouse 2-button,      1 - Logitech 3-button
     2 - IntelliMouse 3-button + wheel, 3 - Mouse Systems 3-button
* `/mouserate n` - set report rate in Hz. Default: 60, Min: 20, Max: 200.
  Increase for smoother cursor movement, decrease for lower CPU load
* `/mousesen n` - set mouse sensitivity (256 - 100%, 128 - 50%, 512 - 200%).
  Default: 256, Max: 1024

### CD-ROM emulation options

* `/cdport x` - set base port of CD interface. Default: 250, 0 to disable
* `/cdlist` - list CD images on the inserted USB drive
* `/cdload n` - load image n in the list given by /cdlist. 0 to unload image
* `/cdloadname x` - load CD image by name. Names with spaces can be quoted
* `/cdauto 1|0` - auto-advance loaded image when same USB drive is reinserted.
  Default: 1
* `/cdvol x` - sets the volume of the CD audio output to x percent.

### NE2000/WiFi options

Only available when running the `pg-ne2k.uf2` firmware on a PicoGUS 1.x board
with a Raspberry Pi Pico W.

* `/ne2kport x` - sets the base port of the NE2000. Defaults to 300.
* `/wifissid abc` - sets the WiFi SSID to abc. "Double quote" to use spaces.
* `/wifipass xyz` - sets the WiFi WPA/WPA2 password/key to xyz. "Double quote"
  to use spaces.
* `/wifinopass` - unsets the WiFi password to connect to an open access point.
* `/wifistatus` - prints the current WiFi status.

## Compiling (developers only)

If you're a developer, PicoGUSinit can be compiled with OpenWatcom 1.9 or 2.0.
Run `wmake` to compile. pgusinit.exe comes with all firmware releases.
