# Third-party notices

Reboot's engine, tools, tests, waves and demo phrases are original work under
the licence in [LICENSE](LICENSE). `Source/DSP/OutputStage.h` and the shared
tools named in [CLAUDE.md](CLAUDE.md) are the Protocodus portfolio's own.

## Sources read, not copied

The cores implement what these documents state; each is cited beside the code
that uses it.

- Pan Docs (gbdev, CC0): Audio, Audio Registers, Audio details; the gbdev
  wiki's "Game Boy sound hardware" for the frame sequencer's steps.
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

No ROM image, game music data or recording is part of this repository.
Game Boy, NES and Famicom are trademarks of their owners, named only to
identify the hardware emulated.

The licensed Reason Jukebox SDK is not part of this repository.
