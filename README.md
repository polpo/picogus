# PicoGUS

![PicoGUS Logo](picogus-logo.svg)

ISA card emulation on the Raspberry Pi Pico's RP2040 microcontroller. Initially focusing on [Gravis Ultrasound (GUS)](https://en.wikipedia.org/wiki/Gravis_Ultrasound) sound card emulation, hence the name PicoGUS.

![picogus-prototype](https://user-images.githubusercontent.com/1544908/182006174-71a1792d-ac5b-4c2b-8e61-94a05a0ef55c.jpg)

Please see [the Wiki](https://github.com/polpo/picogus/wiki) for current status.

Looking for the original project, using Raspberry Pi 3/4? See the [pigus repo](htts://github.com/polpo/pigus).

See/hear PicoGUS in action on YouTube:

[![Watch the video](https://img.youtube.com/vi/rZopvkDTv08/hqdefault.jpg)](https://youtu.be/rZopvkDTv08)

## Open Source Credits

* [Raspberry Pi Pico C/C++ SDK](https://github.com/raspberrypi/pico-sdk)
* `pico_audio_i2s` from [pico-extras](https://github.com/raspberrypi/pico-extras)
* `stdio_async_uart` from [PicoCart64](https://github.com/kbeckmann/PicoCart64)
* `gus-x.cpp` adapted from [DOSBox-X](https://github.com/joncampbell123/dosbox-x)
* OPL/AdLib emulation from [rp2040-doom](https://github.com/kilograham/rp2040-doom)

## License

The hardware portions of this repository (hw/ directory) are licensed under the CERN OHL version 2, permissive.

The software portions of this repository (sw/, pgusinit/ directories) are licensed under the GNU GPL version 2.
