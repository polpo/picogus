# v1.2.0

## New features/fixes

### Sound Blaster 2.0 emulation:

The AdLib mode (`pg-adlib.uf2`) has been replaced with a _new_ Sound Blaster 2.0 mode (`pg-sb.uf2`)! Code to emulate the Sound Blaster DSP was contributed by [Kevin Moonlight](https://github.com/yyzkevin) - huge thanks to him.

* Sound Blaster 2.0 DSP support is pretty well tested but you may run into issues. Please consult the [Compatibility List](https://github.com/polpo/picogus/wiki/Compatibility-list) for titles with known issues, and feel free to [file an issue on GitHub](https://github.com/polpo/picogus/issues/new) to report any problems.
* AdLib/OPL2 emulation has been improved, fixing issues with missing/wrong notes.
* The SB base port can be set with pgusinit with the `/p` option, and the OPL/AdLib base port with the new `/o` option.
* Some older titles supporting AdLib are "speed sensitive" and have weird playback on fast systems (even with a genuine OPL2/OPL3). A new `/w` option in pgusinit can work around these issues in most circumstances.

### MPU-401 emulation:

* Fixes an issue reported by zuldan on Vogons where running pgusinit in MPU-401 mode wouldn't work on certain systems. Fixed by initing the MPU-401 and sending the reset MIDI data asynchronously, preventing IOCHRDY from being held low for too long.
* Sysex delay handled without busy wait. Shouldn't have much effect presently, but may allow MPU-401 to be emulated simultaneously with other modes in the future.

### General:

* Allows the volume of the wavetable header to be set with the `/v` option in pgusinit in all modes. This is useful if you want to use the wavetable header to mix in other external audio sources like a CD-ROM drive (connection guide coming to the Wiki).

# v1.1.0

## New features/fixes

### GUS emulation:
* Works around a hardware bug in the PCM510xA DAC that results in ~10% of chips to be silent at 22.05kHz sampling rate. This is the rate produced by the GUS at 28 channels, and is used by Doom and the demo "Dope". The workaround is to run the DAC at 44.1kHz, linearly interpolating the 22.05kHz output.
* Increased fractional precision of wave addresses to 10 bits from 9.
* New EXPERIMENTAL fixed 44.1kHz output. Normally the GF1 varies its output sample rate from 44.1kHz at 14 voices to 19.2kHz at 32 voices. Using this option enables 44.1kHz output for all numbers of voices, similar to the Interwave. This gives much higher quality output when playing lots of channels.  This will result in stuttering in most games that use streaming DMA for sound effects like Doom, hence it is EXPERIMENTAL. It can be enabled with the `/4` switch in pgusinit. Thanks to [wbcbz7](https://github.com/wbcbz7) for contributing this support along with the increased wave address precision.
* Known issue: Impulse Tracker constantly re-inits the GUS to get the best sampling rate and if it passes through 28 channels rapidly, audio artifacts can be heard. To work around, run PicoGUS in fixed 44.1kHz output mode.

### pgusinit
* New `/4` switch when in GUS mode to enable fixed 44.1kHz output.

# v1.0.2

## New features/fixes

### General:
* New bus timing, fixes compatibility with IBM PC/AT 5170.
* Faster IOCHRDY timing to improve compatibility and spend less time on ISA bus.
* Less aggressive overclock and different startup timing to improve stability on reset.
* Added support for Xbox 360 wireless adapter dongle.

# v1.0.1

As always immediately after I do a big release, a significant bugfix comes soon after! Don't forget to look at the notes for firmware v1.0.0.

### General:
* Increase stability upon startup on PicoGUS 2.0 boards.

# v1.0.0

The PicoGUS 2.0 hardware brings v1.0.0 of the PicoGUS firmware! This firmware runs on _all_ released revisions of the PicoGUS hardware: 1.1, 1.1.1, 1.2, and 2.0.

## New features/fixes

### General:
* Support for PicoGUS 2.0 hardware and its software-controlled wavetable header volume (`/v xxx` pgusinit option).
* A preview of game port joystick emulation using USB joysticks is included in all sound card emulation modes, enabled with the `/j` pgusinit option. There is also a "joystick exclusive" firmware (`pg-joyex.uf2`) for when you only want to use a USB joystick and not emluate any sound cards. This is considered a preview because only a few USB joystick types are supported: wired Xbox 360 (and third party clone) controllers and the Sony DualShock 4. More joysticks will be supported in the future! See the [Compatibility List](https://github.com/polpo/picogus/wiki/Compatibility-list) wiki page for more information about this feature.
* Much more reliable firmware flashing from DOS. The previous method abused IOCHRDY to hold the ISA bus far longer than usual, and some chipsets would release the bus before certain flash operations were complete.
* A more robust startup sequence to improve firmware boot stability. All firmwares have the same LED sequence when starting so you will know when the firmware is ready.
* Compatibility with slower PSRAM chips that are rated for only 104MHz, such as the Vilsion Tech VTI7064MSME and ISSI IS66WVS1M8BLL-104NLI. This widens available options for those DIYing their own PicoGUS boards.

### pgusinit

pgusinit has been updated to version v2.0.0, featuring:

* Updates to pgusinit to support the new firmware flashing protocol, as well as support upgrading from v0.x.x firmware to v1.x.x firmware. If you have a v0.x.x version of firmware running on your PicoGUS, you can upgrade to v1.0.0 with the latest version of pgusinit included with the firmware release package.
* Detects the current card mode and only shows options applicable to that mode when using `/?` to ask for help.

### Adlib emulation:
* Fixes an issue where some software would incorrectly detect PicoGUS as an OPL3 instead of an OPL2.

### MPU-401 emulation:
* Sysex delay can be enabled with the `/s` pgusinit option. This will prevent buffer overflows on older MPU-401 revisions.
* Fake all-notes-off for the Roland RA-50 can be enabled with the `/n` pgusinit option.
* Fixes song change issue in Frederik Pohl's Gateway. PicoGUS will detect when Gateway is running and enable a hack in how the version and ack are returned. Run pgusinit to restore default operation after you're done playing Gateway.
* Running pgusinit will silence any stuck notes if a program exits unceremoniously.

# v0.7.0

## _Note:_ Please use the version of pgusinit.exe that comes with this firmware!

Version v0.7.0 of the firmware has a much smaller default GUS audio buffer size of 4 samples, thanks to the higher RP2040 clock speed. If you run an older version of pgusinit.exe, it will set the buffer size to 16 samples, negating many of the improvements that this version brings for GUS emulation.

## Release notes:

### General:
* Runs the RP2040 at 400MHz. This allows for faster reaction to ISA events, holding IOCHRDY low for shorter time periods. This allows better performance for other ISA cards and may help run on faster ISA bus speeds.
* Fixes from @SuperIlu for pgusinit to fix usage message and improve command line parsing.

### GUS emulation:
* Higher clock speed enables much shorter interval before checking for IRQs. This should improve compatibility in titles that use IRQs and DMA.
* Fixes for Hand386 and other M6117D-based systems.
* Changes to pgusinit to default to the new GUS buffer size of 4 samples, and support setting the buffer size below 8 samples. Please use the new version of pgusinit that ships with this firmware!

### Tandy emulation:
* New emulation core contributed by [Aaron Giles](https://aarongiles.com/). This is the same core used in [DREAMM](https://aarongiles.com/dreamm/) and should result in better accuracy across all titles. or example, sound effects such as crickets chirping and meteor crash in Maniac Mansion and snoring in Zak McKracken sound much better.

### CMS emulation:
* New emulation core contributed by [Aaron Giles](https://aarongiles.com/). This is the same core used in [DREAMM](https://aarongiles.com/dreamm/) and should result in better accuracy across all titles.
* Fixes issue with pitch of CMS sound being too high.

### MPU-401 emulation:
* Misc optimizations might improve compatibility in some titles.

For known issues, please see the [Compatibility List](https://github.com/polpo/picogus/wiki/Compatibility-list) wiki page.

# v0.6.1

Another bugfix release! This release fixes a bug introduced in firmware [v0.5.0](https://github.com/polpo/picogus/releases/tag/v0.5.0) that caused Descent to freeze on the `Loading...` indicator when running setup and starting the game. 

### GUS emulation:
* Fixes freeze when starting Descent. This patch release uses a smaller/simpler 4-byte DMA buffer that solves the freeze with Descent while also hopefully still preventing stuttering in Doom introduced by level changes (the reason for the DMA buffer in the first place).

For known issues, please see the [Compatibility List](https://github.com/polpo/picogus/wiki/Compatibility-list) wiki page.

# v0.6.0

### CMS emulation:
* New emulation mode: **CMS/Game Blaster**.

For known issues, please see the [Compatibility List](https://github.com/polpo/picogus/wiki/Compatibility-list) wiki page.

# v0.5.1

Bugfix release! All of the new features of [v0.5.0](https://github.com/polpo/picogus/releases/tag/v0.5.0) plus the following fixes:

### Tandy emulation:

* Fixed reporting of base port when running pgusinit when on Tandy firmware
* Fixed firmware flashing from DOS when on Tandy firmware

# v0.5.0

### General:
* More reliable firmware flashing from pgusinit by clocking down the Pico and disabling interrupts. Many thanks to JazeFox at Vogons for debugging this issue and suggesting a fix.

### Tandy emulation:
* New emulation mode: **Tandy 3-Voice**. This emulation is on port 2C0h by default, to avoid conflicts or masked IO on the default Tandy port of 0C0h in non-Tandy systems. This is the same alternate port that [Matze79's "TNDY" card](https://www.vogons.org/viewtopic.php?f=62&t=54249) can use. Use the [TNDY driver program](https://github.com/JKnipperts/TNDY) to redirect port 0C0h to 2C0h. Also [many games need to be patched](https://www.vogons.org/viewtopic.php?t=77679) to allow for Tandy sound on non-Tandy systems with CGA, EGA, or VGA graphics. Note that this mode has not received much optimization, so accuracy and compatibility can be improved.

### GUS emulation:
* Resets DMA status bit to 0 on completion of DMA handler. Fixes some Demoscene titles.
* Buffers uploaded DMA samples instead of writing 1 byte at a time. Currently this buffer is 8 bytes, and may be configurable in future releases. Should help stuttering/slowdown issues in games that use streaming DMA audio like Doom.
* Bails out on sound rendering when sample rate changes. This should eliminate any "warbling" sounds when Impulse Tracker resets the channel count on the fly.
* Uses the [RP2040 hardware interpolation](https://people.ece.cornell.edu/land/courses/ece4760/RP2040/C_SDK_interpolator/index_interpolator.html) unit to interpolate samples and clamp output to 16 bits.

For known issues, please see the [Compatibility List](https://github.com/polpo/picogus/wiki/Compatibility-list) wiki page.

# v0.4.0

### General:
* New version (v1.0.1) of pgusinit.exe - DMA interval is in µs, not ms

### GUS emulation:
* Refactored DMA transfer completion handler: this results in much higher compatibility with titles that use streaming DMA. Overriding the DMA transfer interval is not necessary in most circumstances now.
  * Fixes games: Quake, Doom (and Doom engine games), Duke3D, Descent, Hocus Pocus (partially); Demo: Inside
* Fixed bug where DMA control register was not properly reset between transfers. Fixes garbage samples when uploading mixed 16 & 8 bit samples.
  * Fixes music players/trackers: Open Cubic Player (with `gusFastUpload=on`), Fast Tracker 2

# v0.3.0

### General:
* Firmware can now be programmed from DOS (requires FW v0.3.0 or above - upgrading from previous versions must be done by USB)
   * Emulation mode can be changed "on the fly" by uploading different firmwares
* New version (v1.0.0) of pgusinit.exe - Firmware versions v0.3.0+ require this new version of pgusinit.exe!

### GUS emulation:
* Major DMA improvements!
  * DMA PIO state machine rewrite to solve sample glitches (fixes Star Control II)
  * Asynchronous writing to PSRAM of DMAed samples (fixes Doom)
  * Smarter sample cache (fixes Doom)
  * Adjustable DMA interval (fixes Doom)
* One firmware for all ports - base port is now configured with `ULTRASND` environment variable and pgusinit.exe

### MPU-401 emulation:
* Intelligent mode emulation is now working in all but a few titles
* Base port now configured with pgusinit.exe (defaults to 330)

### AdLib emulation:
* Base port now configured with pgusinit.exe (defaults to 388) 

# v0.2.0

* Solved a regression where lots of channels playing 16-bit samples would cause audio stuttering (example: the demo Aeon Drift by Disaster Area). Added a super simple sample pair buffer per channel to solve this.
* Super-unstable DMA support preview. Count on it to fail, but feel free to celebrate when it works! 😅
* Fixed a small bug in pgusinit.exe that didn't set the audio buffer size to 16 if the `/a` option was not given on the command line.
* Includes firmwares for GUS port 220, 240, and 260.
* Built with a new automatic release script.

For firmware programming instructions, see the ["Programming the Pico" part of the build guide](https://github.com/polpo/picogus/wiki/Building-your-PicoGUS#programming-the-pico).

# v0.1.0

* New release format: a ZIP file containing firmware UF2 files and PGUSINIT.EXE.
* Features all IRQ fixes shown in the [November 2022 update video](https://youtu.be/CkJvkJVRscQ) and then some – regressions introduced by those fixes have now been fixed. 😅
* New version of PicoGUSinit (PGUSINIT.EXE) to allow the audio buffer size on the PicoGUS to be configured via the `/a n` option, where `n` is the size of the buffer in samples. Some productions may still freeze, or have audio glitches, and configuring the buffer size can help. Please see the [Compatibility List](https://github.com/polpo/picogus/wiki/Compatibility-list) for the flag to use for any problematic programs.
* Preview firmware for Adlib and MPU-401 emulation.

For firmware programming instructions, see the ["Programming the Pico" part of the build guide](https://github.com/polpo/picogus/wiki/Building-your-PicoGUS#programming-the-pico).

# v0.0.1

Contents:
`picogus.uf2` - Firmware with Gravis Ultrasound emulation core, port 240
`PGUSINIT.EXE` - DOS executable to detect and initialize the PicoGUS
