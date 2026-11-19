# Decisions

What was decided, by whom and on what grounds. A direction chosen by ear is
recorded as chosen by ear; a measured one names its measure.

## 2026-11-18 — 0.5.0: strict hardware blackbox and CPU-bus sound drivers

**Owner direction** (2026-11-18): "research and enhance authenticity, fidelity
and realism of the sound engine. the desired architecture is as follows: use
chip + related analog electronic as a blackbox (implemented as faithfully as
possible) and then having a driver above that will process MIDI played by the
user, trying to implement whatever is played as instructions to the black box -
only instructions that the real unit accepts are possible to be send."

- **Pure CPU-bus blackbox boundary (`Chips/` + `Boards/`).** Removed all
  synthetic backdoors (`SetWaveExtension`, `LoadWave`, `ChannelSettings`,
  `Shadow`, `PeriodFor`, `Start`/`Change`/`Stop`, and driver headers) from
  `Chips/` and `Boards/`. `Handheld` and `Console` expose only `Write(address,
  value)`, `Read(address)`, `FinishBoot()`, `RunTo(cycle)`, and `Render()`.
  Multi-register sequences pass through a cycle-accurate `BusWriteQueue` using
  SM83 timing (`8`/`12` T-cycles) on `Handheld` and 6502 store timing (`4`/`6`
  CPU cycles) on `Console`, with 6502 address decoding (including Sunsoft 5B
  `$C000`/`$E000` register latch addressing).
- **Driver layer (`Driver/`) handles musical play within hardware limits.**
  `HandheldDriver` and `ConsoleDriver` translate MIDI, envelopes, fallback
  routing, and waves strictly into legal CPU bus writes:
  - Handheld CH3 below 32 Hz (`A0`–`B0`) runs a master-cycle `NR32` timer ISR
    writing output levels at 32 steps per cycle against an all-`$FF` Wave RAM
    instead of modifying chip period registers. On `Start()` when a channel is
    already silently parked at `$08` (`sh.active && sh.volume == 0 && volume
    == 0`), the driver skips rewriting `$08` to `NRx2` before trigger so DMG
    zombie increments cannot bump `0 -> 1` across the bus spacing to `NRx4`.
  - Handheld `< 64 Hz` Pulse fallback onto CH3 loads exact 32-nibble Pulse duty
    waves matching the selected duty shape.
  - Handheld Noise retriggers when switching between 15-bit and 7-bit LFSR
    widths mid-note to prevent all-1s lock-up.
  - Console FDS (over ~1747 Hz) and Namco 163 (8-ch over ~3728 Hz) fold
    multiple harmonic cycles into the wavetable (`mult = 1, 2, 4`) so the
    entire piano range up to C8 plays natively on the cartridge chip without
    falling back to APU Triangle.
  - Native volume quantization preserves hardware DAC ranges (`0..32` on FDS
    `$4080`, `0..42` on VRC6 sawtooth `$B000`, `0..3` on GB CH3, `0..1` gate on
    APU Triangle, `0..15` elsewhere).
- **Hardware & analog fidelity fixes.** Added CH3 6-T-cycle trigger delay,
  2-T-cycle DMG Wave RAM retrigger corruption window + `buffer = waveRam_[0]`,
  active Wave RAM lock/CGB byte redirect, CGB-04/DMG `NRx2` zombie mode guarded
  by `channel.on` and `Envelope::halted`, DMG `NR52=0` length preservation,
  `50 us` continuous BLEP-aligned (`StepBuffer::ReadDelta`) DAC-off decay on
  `Handheld::Path::Line`, VRC6 `$B002` `E=0` sawtooth accumulator reset,
  rainwarrior RP2C33 FDS negative-bias modulator rounding + `latchedGain_` + lazy
  `CatchUp()`, MMC5 `$5011` PCM DAC output, and FDS `$4089` master-volume
  2–2R impedance corner shifting (`2 / 3 / 4 / 5 kHz`).
- **A–Z listening review verdict (owner, 2026-11-18).** Rendered five
  RMS-matched sets (48 kHz, 64-frame blocks, `Path = Line`, `A` = shipping
  `0.5.0`) testing: (1) sub-64 Hz CH3 Pulse duty table (`A`) vs `kWaves[0]`
  (`B`); (2) FDS `2x`/`4x` folded wavetables up to `C8` (`A`) vs single-cycle
  ceiling handing off to APU Triangle (`B`); (3) VRC6 native `0..42` sawtooth
  volume resolution (`A`) vs 4-bit 16-step quantization (`B`); (4) FDS
  `$4089 = 0` (`2 kHz` LPF) with RP2C33 negative-bias modulator rounding (`A`)
  vs `$4089 = 3` (`5 kHz` LPF, level-matched `B`) vs naive symmetric modulator
  scaling (`C`); and (5) Color `47 Hz` + `176 Hz` dual-pole Line fit (`A`) vs
  Pocket `25 Hz` + `96 Hz` (`B`) vs DMG `28 Hz` single-pole (`C`). Owner
  direction ("keep the most advanced model") kept `A` across all five sets;
  every retained candidate already matches the hardware measurements and
  specifications implemented in `0.5.0`.

## 2026-11-18 — 0.4.0: one voice, two machines

**Owner direction** (2026-11-18): "remove two layers - keep only one. keep
double mode. keep only handheld and console devices. ... cleanup all
remaining devices, etc - not needed (no backwards compatibility needed).
check why the envelope (adsr) is not respected by device. ensure consistent
output level across devices and tones. keep authenticity of the hardware."

- **The PSG and the micro are removed**, with the SN76489 and beeper cores
  and their tests. The AY core stays as the 5B's. No listening test: the
  owner's scope decision.
- **No backwards compatibility.** Every control is renumbered and the `a_`
  and `b_` ids are gone (`Source/DSP/RebootControls.h`). This is the one
  exception to "never rename or reuse a control id", granted by the owner
  for 0.4.0 because no build has reached users; it does not license another.
- **One layer, Double kept.** The two layers' machines are one set again;
  Double still gives a second set on the right.

## 2026-11-18 — the envelope in milliseconds

**Why the envelope seemed ignored.** Attack, Decay and Release were frames
a volume step (0 to 15). One step at 15 levels made 1 already a quarter of
a second at 60 Hz, a setting of 0 was instant and nothing lay between; the
arpeggio did not restart it, so on Arp presets only the first step had an
attack; and the triangle has no volume, so there the envelope only gates.
**Decision:** times in ms (0 to 5000, a cubic knob), the envelope moved a
fraction of a step each frame and the chip written the whole step reached,
once a frame (the chip still sees only what it accepts); each Arp step
restarts it. Tagged **ours** (the playing layer). Measured, not by ear:
`Reboot.Engine`'s envelope checks hold the times to a frame.

## 2026-11-18 — the level trims

**Decision:** a trim after the machine brings every part and shape of every
machine, revision, cartridge and path to the loudness of a 50 % square at
`Engine::kReferenceRms`, by ITU-R BS.1770's K-weighted mean square over C3,
C4 and C5 (`Tools/RebootLevels.cpp`, its protocol at its head;
`Reboot.Levels` holds every one to 0.05 dB). Before it the parts ranged from
5 dB under to 4 dB over. Tagged **ours**: the chips and boards are not
changed, only how loud each part arrives.

Not decided by ear: whether K-weighted loudness or plain RMS matches parts
better to a listener (they differ most for the bright 163 waves and the
noise). If it is questioned, the A–Z test is K-weighted (A, shipping)
against RMS (B), the same phrase on every part, level-matched by the measure
under test. Until then K-weighting stands as the standard loudness measure.

## 2026-11-18 — a machine starts at rest

`Reboot.ParameterMatrix` found a DC offset of 0.098 (limit 0.05) with eight
consoles at Bias 64 on Pin: every reset stepped each console from nothing to
its rest level (the triangle's held first step and the DMC at Bias, up to
0.7 of full scale at Bias 127), a thump through the 5 Hz Pin blocker.
**Decision:** a reset machine's output stages start settled on its rest
level (`Console::Reset`), so a reset is silent; a Bias moved while playing
still steps as the $4011 write does. Tagged **ours**: a machine chosen is
heard as one that has stood switched on. The limit was not changed.
`Reboot.Boards` checks a machine at rest gives no output on either path at
Bias 0 and 127.
