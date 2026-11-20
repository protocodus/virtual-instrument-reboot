# Third-party notices

Reboot's engine, tools, tests, waves and demo phrases are original work under
the licence in [LICENSE](LICENSE). `Source/DSP/OutputStage.h` and the shared
tools named in [CLAUDE.md](CLAUDE.md) are the Protocodus portfolio's own.

## Sources read, not copied

The cores implement what these documents state; each is cited beside the code
that uses it.

- Pan Docs (gbdev, CC0): Audio, Audio Registers, Audio details; the gbdev
  wiki's "Game Boy sound hardware" for the frame sequencer's steps.
- SameBoy (Lior Halphon, Expat/MIT licence,
  [ThirdParty/sameboy-LICENSE.txt](ThirdParty/sameboy-LICENSE.txt)): read for the
  hardware timing and analog decay rules it documents (CH3 6-T-cycle trigger
  delay, 2-T-cycle DMG Wave RAM retrigger corruption window, and 50 us DAC
  turn-off decay in `Source/DSP/Chips/GameBoyApu.h` and
  `Source/DSP/Boards/Handheld.h`). No code is copied.
- The NESdev Wiki: the APU pages, "APU Mixer", "Cycle reference chart",
  "Expansion audio", and the VRC6, MMC5, Namco 163, FDS and Sunsoft 5B audio
  pages.
- General Instrument's AY-3-8910 data manual, and Ayumi (Peter Sovietov,
  MIT licence, [ThirdParty/ayumi-LICENSE.txt](ThirdParty/ayumi-LICENSE.txt))
  and MAME's `ay8910.cpp` (BSD-3-Clause) for the rules they state about the
  counters, the noise and the envelope (`Source/DSP/Chips/Ay8910.h`, the
  5B's core). None of their code is copied.
- Brad Smith's nes-audio-tests (freely redistributable) and its results in
  the NESdev forum (threads t=17741 and t=24288): the 5B's level against
  the APU and the APU's DAC curves.

During the 0.5.1 pulse audit a reviewer viewed an excerpt of Mesen2's
square-channel header, contrary to this project's source-review rule.
A second reviewer independently reconstructed all 32 lookup entries from
Reboot 0.5.0's existing time-order waveforms and the counter direction in
the [NESdev APU Pulse reference](https://www.nesdev.org/wiki/APU_Pulse#Implementation_details).
The result matches the committed table byte for byte. The regression
exercises those waveforms through Reboot's public register interface.
No emulator implementation was incorporated.

No ROM image, game music data or recording is part of this repository.
Game Boy, NES and Famicom are trademarks of their owners, named only to
identify the hardware emulated.

The licensed Reason Jukebox SDK is not part of this repository.
