# PicoGUS

![PicoGUS Logo](picogus-logo.svg)

ISA sound card emulation on the Raspberry Pi Pico's RP2040 microcontroller. PicoGUS can emulate:

* [Gravis Ultrasound (GUS)](https://en.wikipedia.org/wiki/Gravis_Ultrasound) - the primary focus of PicoGUS, hence the name
* [AdLib (OPL2)](https://en.wikipedia.org/wiki/Ad_Lib,_Inc.)
* [MPU-401 (with intelligent mode)](https://en.wikipedia.org/wiki/MPU-401) - outputs MIDI data on 3.5mm MIDI TRS connector
* [Tandy 3-voice](http://www.vgmpf.com/Wiki/index.php?title=Tandy_3_Voice)
* [CMS/Game Blaster](http://nerdlypleasures.blogspot.com/2012/10/all-you-ever-wanted-to-know-about.html)

Current status: Beta! See the [main wiki page](https://github.com/polpo/picogus/wiki) for current status and the [compatibility list](https://github.com/polpo/picogus/wiki/Compatibility-list) for support status of various DOS programs and other system compatibility notes. This project has a heavy demoscene focus due to the GUS's history so that's what I've concentrated on, but GUS support in games is quickly improving.

Want to make your own beta PicoGUS? See the [build guide](https://github.com/polpo/picogus/wiki/Building-your-PicoGUS). **Important caveat**: due to the specs of the Pico, assumptions made by programs written to use the GUS, the imprecise nature of emulation, and the varying specs of retro DOS PC hardware, some things will likely never be perfect. **This is still a work in progress.**

Have a PicoGUS and want to use it? See the [configuring and using your PicoGUS guide](https://github.com/polpo/picogus/wiki/Configuring-and-using-your-PicoGUS).

![PicoGUS v1.1 beta PCB](https://user-images.githubusercontent.com/1544908/215666529-fc694b8f-aec3-4679-87df-b53d0c406c99.jpg)

Looking for the original project, using Raspberry Pi 3/4? See the [pigus repo](https://github.com/polpo/pigus).

See/hear PicoGUS in action on YouTube:

[<img src="https://img.youtube.com/vi/h4iWSnTc9Ag/hqdefault.jpg" alt="September 2022 update" width=400>](https://youtu.be/h4iWSnTc9Ag)
[<img src="https://img.youtube.com/vi/CkJvkJVRscQ/hqdefault.jpg" alt="October 2022 update" width=400>](https://youtu.be/CkJvkJVRscQ)
[<img src="https://img.youtube.com/vi/F5Zk_hHHkTg/hqdefault.jpg" alt="December 2022 update" width=400>](https://youtu.be/F5Zk_hHHkTg)
[<img src="https://img.youtube.com/vi/sOHTagrWcIE/hqdefault.jpg" alt="January 2023 update" width=400>](https://youtu.be/sOHTagrWcIE)

## Open Source Credits

* [Raspberry Pi Pico C/C++ SDK](https://github.com/raspberrypi/pico-sdk)
* `pico_audio_i2s` from [pico-extras](https://github.com/raspberrypi/pico-extras)
* `stdio_async_uart` from [PicoCart64](https://github.com/kbeckmann/PicoCart64)
* `gus-x.cpp` adapted from [DOSBox-X](https://github.com/joncampbell123/dosbox-x)
* OPL/AdLib emulation from [rp2040-doom](https://github.com/kilograham/rp2040-doom) (based on [emu8950](https://github.com/digital-sound-antiques/emu8950))
* MPU-401 emulation adapted from [HardMPU](https://github.com/ab0tj/HardMPU) and [DOSBox-X](https://github.com/joncampbell123/dosbox-x)
* Tandy 3-voice emulation from [emu76489](https://github.com/digital-sound-antiques/emu76489)
* CMS emulation adapted from [MAME](https://github.com/mamedev/mame)

## License

The hardware portions of this repository (hw/ directory) are licensed under the CERN OHL version 2, permissive.

The software portions of this repository (sw/, pgusinit/ directories) are licensed under the GNU GPL version 2.
