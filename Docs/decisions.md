# Decisions

What was decided, by whom and on what grounds. A direction chosen by ear is
recorded as chosen by ear; a measured one names its measure.

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
