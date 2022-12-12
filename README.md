# PicoGUS

![PicoGUS Logo](picogus-logo.svg)

ISA card emulation on the Raspberry Pi Pico's RP2040 microcontroller. Initially focusing on [Gravis Ultrasound (GUS)](https://en.wikipedia.org/wiki/Gravis_Ultrasound) sound card emulation, hence the name PicoGUS.

Current status: Beta! See the [main wiki page](https://github.com/polpo/picogus/wiki) for current status and the [compatibility list](https://github.com/polpo/picogus/wiki/Compatibility-list) for support status of various DOS programs. This project has a heavy demoscene focus due to the GUS's history so that's what I've concentrated on.

Want to make your own beta PicoGUS? See the [build guide](https://github.com/polpo/picogus/wiki/Building-your-PicoGUS). **Important caveat**: due to the specs of the Pico, assumptions made by programs written to use the GUS, the imprecise nature of emulation, and the varying specs of retro DOS PC hardware, some things will likely never be perfect. **This is still a work in progress.**

Have a PicoGUS and want to use it? See the [configuring and using your PicoGUS guide](https://github.com/polpo/picogus/wiki/Configuring-and-using-your-PicoGUS).

![PicoGUS v1.1 beta PCB](https://user-images.githubusercontent.com/1544908/197922769-fbc45c85-0fd3-4b5a-896e-0c56e5fa171e.jpg)

Looking for the original project, using Raspberry Pi 3/4? See the [pigus repo](https://github.com/polpo/pigus).

See/hear PicoGUS in action on YouTube:

[![Watch the September 2022 Update](https://img.youtube.com/vi/h4iWSnTc9Ag/hqdefault.jpg)](https://youtu.be/h4iWSnTc9Ag)

[![Watch the November 2022 Update](https://img.youtube.com/vi/CkJvkJVRscQ/hqdefault.jpg)](https://youtu.be/CkJvkJVRscQ)

## Open Source Credits

* [Raspberry Pi Pico C/C++ SDK](https://github.com/raspberrypi/pico-sdk)
* `pico_audio_i2s` from [pico-extras](https://github.com/raspberrypi/pico-extras)
* `stdio_async_uart` from [PicoCart64](https://github.com/kbeckmann/PicoCart64)
* `gus-x.cpp` adapted from [DOSBox-X](https://github.com/joncampbell123/dosbox-x)
* OPL/AdLib emulation from [rp2040-doom](https://github.com/kilograham/rp2040-doom)
* MPU-401 emulation from [HardMPU](https://github.com/ab0tj/HardMPU)

## License

The hardware portions of this repository (hw/ directory) are licensed under the CERN OHL version 2, permissive.

The software portions of this repository (sw/, pgusinit/ directories) are licensed under the GNU GPL version 2.
