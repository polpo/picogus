# v1.2

A new release of the more DIY friendly version of PicoGUS "backporting" improvements from the v2.0 board.

* Adds option for integrated DAC section. The purple "GY PCM5102" DAC modules have spotty QA, so integrating the DAC is both potentially lower cost and more reliable. Footprints remain to still allow use of the DAC module.
* Reset stability improvements: RC circuit on Pico RUN pin to delay start on power up and filter out glitches on ISA reset, and U5 is now 74AHC logic family for higher positive-going threshold
* ISA RESET has weak pulldown to allow reliable programming the Pico outside of powered PC
* DACK has weak pullup to allow PicoGUS to be used without DMA channel set
* 5V MIDI circuit: uses open drain inverter for better drive and uses more commonly available resistors
* U2 is now specified as an 74LVC244 in the BOM: it's cheaper and more available and in my testing performs just as well as the 74CB3T3245
* Jumper to apply power to micro USB-B connector on Pico to allow USB HID devices to be used
* Fixes to card dimensions to better follow ISA card specs: narrower card edge and better fit for card bracket in PC cases
* Production files for JLCPCB full PCB assembly
* Updates to thank you section on back

# v1.1.1

Functionally identical to v1.1. Brings released BOM, gerbers, and KiCad design into sync.

* C10 is a 1uF capacitor

# v1.1

First "final" released hardware version of PicoGUS.
