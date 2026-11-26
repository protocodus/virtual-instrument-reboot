# Reboot

Reboot is a sound-chip synthesizer: the sound hardware of two 8-bit
machines, emulated from the published specifications of their chips and
played from a keyboard. The handheld board is the APU from the Game Boy and
Game Boy Color; the console board the 2A03 from the NES and Famicom (the
2A07 on the PAL console), with the Famicom's cartridge chips beside it:
Konami's VRC6, Nintendo's MMC5, Namco's 163, the Famicom Disk System's wave
unit and Sunsoft's 5B. Every chip is an integer core that runs on its
machine's own clock, and a keyboard plays it through register writes, as a
game's sound driver did, so the chip's own pitch steps, volume steps and
quirks are what is heard. This repository holds the JUCE-free C++17 engine,
its tools and its tests. Reboot is being built as a Rack Extension for
Reason, which plays this engine; a plug-in front end is not built yet, and
this repository has no release.

## Audio demos

All eight factory presets, each playing a phrase written for it
(`Tools/write_preset_midi.py`, committed in `Docs/audio/midi/`) through the
shipping engine as the preset loads: on the handheld, a square lead with a
delayed vibrato, a gliding bass on the wave channel, an arpeggio on the
Color's pulse whose every step starts the envelope again, and a groove on
the noise channel; on the console, the triangle's bass, a gliding pulse
doubled and detuned, the VRC6's sawtooth and the disk unit's modulated wave
on six consoles.

<!-- audio-demos-begin -->

| # | Preset | Demo | Length | Peak |
| --- | --- | --- | --- | --- |
| 1 | Square Lead | [01-square-lead.wav](Docs/audio/01-square-lead.wav) | 9.7 s | -12.1 dBFS |
| 2 | Wave Bass | [02-wave-bass.wav](Docs/audio/02-wave-bass.wav) | 8.4 s | -11.6 dBFS |
| 3 | Pocket Arp | [03-pocket-arp.wav](Docs/audio/03-pocket-arp.wav) | 10.4 s | -16.9 dBFS |
| 4 | Static Snare | [04-static-snare.wav](Docs/audio/04-static-snare.wav) | 10.8 s | -15.8 dBFS |
| 5 | Triangle Bass | [05-triangle-bass.wav](Docs/audio/05-triangle-bass.wav) | 8.3 s | -17.5 dBFS |
| 6 | Pulse Glide | [06-pulse-glide.wav](Docs/audio/06-pulse-glide.wav) | 9.0 s | -19.2 dBFS |
| 7 | Saw Lead | [07-saw-lead.wav](Docs/audio/07-saw-lead.wav) | 8.4 s | -18.8 dBFS |
| 8 | Disk Organ | [08-disk-organ.wav](Docs/audio/08-disk-organ.wav) | 10.2 s | -7.9 dBFS |

<!-- audio-demos-end -->

`build-dsp/RenderRebootPresetDemos Docs/audio/midi <out-dir>` renders them,
48 kHz 24-bit stereo, with the `demos.json` from which `python3
Tools/update_readme_blocks.py demos --readme README.md --manifest
<out-dir>/demos.json` writes the table. There is no main build yet, so the
WAVs are rendered and committed by hand after a change that alters them.

## Version history

### 0.10.0 — 2026-11-26 — six-board chords, intra-board voice packing, and chip/driver fixes

Boards defaults to 6 (Disk Organ with it), so every one of the twelve sounds plays six-voice chords out of the box, while untouched boards rest at zero cost. A Poly chord now fills a machine's own channels before taking another unit, so Handheld Pulse dyads pair CH1 and CH2 on one unit (and Left/Right with Sides Spread), Console Pulse chords share one 2A03's nonlinear pulse DAC, Sunsoft 5B triads drive one cartridge's saturating amplifier together, and Namco 163 chords pack onto one multiplexer first; separate single-note phrases still rotate across units.

On the handheld, CGB `$FF76`/`$FF77` (`PCM12`/`PCM34`) read back the live digital channel nibbles (`0xFF` on DMG), `NRx2` zombie writes during the 64 Hz envelope clock's high phase (`divStep == 7`) reload the step timer (SameBoy), re-enabling a CMOS DAC clamps any remaining `50 us` floating turn-off tail instead of superposing onto the driven rail, and the DMG wave driver turns `NR30` off before active CH3 retriggers so rapid arpeggios never hit the 2-T-cycle Wave RAM corruption window. On the console, `$4015` tracks the 4-step frame interrupt and non-looping DMC sample-end interrupt flags, Namco 163 `$4800` auto-increment wraps `$7F -> $00`, and the disk unit's `$4088` modulation tables are exact zero-sum anti-symmetric sines folded across 1, 2 and 4 cycles with compensated depth so FM stays 1:1 harmonic up to C8 without counter drift.

### 0.9.0 — 2026-11-23 — Glide with Sync, and a Volume that does not click

One control joins, at the value that plays as 0.8.0 did. With Sync, Glide sync time makes Glide a note value at the song's tempo, as Arp rate and Vibrato rate already were: Off, or 1/64 to 1/1, which the glide covers in the driver's ticks nearest it. Volume, our trim after the machines, now moves over 64 samples when it changes, where it jumped at once and clicked. The chips' own steps are untouched: a volume register written, a note started or stopped, still steps the output as it did on the hardware. Front ends get one Sound list to show in place of Board, Expansion and Part (`kSounds`, `SetSound`): every part the machines offer, a cartridge's on the Console revision with the slot.

### 0.8.0 — 2026-11-23 — Sync to the song's tempo

Three controls join, each at the value that plays as 0.7.0 did. Sync makes Arp rate and Vibrato rate note values at the song's tempo, which a front end passes in (`Engine::SetTempo`), and while the song plays an arpeggio keeps its grid (`Engine::SetSongPosition`): Arp sync rate is a step of 1/64 to 1/4, Vibrato sync rate a cycle of 1/1 to 1/16T. The driver stays on its frames: each arpeggio step lands on the tick nearest its place in time, so the steps mix lengths of n and n + 1 ticks, as a tracker's rows do at a tempo its frames do not divide, none is off by more than half a tick (8 ms at Driver rate 1, 1 ms at 8), and they never drift.

### 0.7.0 — 2026-11-23 — the chips' own envelopes, sweeps and sides

Five controls join, each at the value that plays as 0.6.0 did. Envelope Chip hands a note's volume to its channel's own envelope unit: the trigger starts it and the chip steps it, where the driver wrote every step before. It plays the handheld's pulses and noise (climbing, or falling from the Level), the console's and the MMC5's pulses and the console's noise (falling from 15), the disk unit's gain (up to 32, or down from the Level) and the 5B's one envelope, which the newest note takes. Its paces are the nearest to Attack, Decay and Release, and it runs on past Sustain to 0, as a unit does; Envelope Loop lets the console's, the MMC5's and the 5B's fall again and again. Sweep and Sweep pace play the pitch sweeps of the handheld's first pulse and the console's two pulses from each note's start. Sides puts the handheld's first pulse on the left and its second on the right. Load plays the handheld's Line into 32 Ohm headphones, whose socket adds a high-pass near 50 Hz.

The original handheld's volume writes while a channel plays (zombie mode) now follow SameBoy's reported rule for every value, where only `$08` did before.

### 0.6.0 — 2026-11-23 — two real units, each on its own frames

Every machine is now a unit of its own: its own crystal, the moment it was switched on, and its own output level and filter corners within its parts' tolerances. Its sound driver counts frames from its own power-on, so two units' envelope and vibrato steps fall at their own moments. Double is two units of the same board set up the same and given the same notes, the first on the left and the second on the right: their differences are the stereo, and Detune pulls the two crystals apart, moving the whole unit rather than its period steps. A held note on the second unit is not a copy of the first.

Four controls join, each at the value that plays as 0.5.2 did. Poly channels lets a chord spill from the part's channels onto the chip's other pitched voices once the part's are held (the handheld's two pulses then its wave; the console's pulses, triangle and cartridge voices). Arp order plays up, down, up and down, or as played. Arp retrigger off moves the pitch only and leaves the envelope. Driver rate runs the driver 1 to 8 times a frame, as a driver on a timer interrupt does; times stay in ms and the arpeggio counts ticks.

### 0.5.2 — 2026-11-20 — held notes and rendering cost

Releasing the sustain pedal in Mono keeps the tail on the last sounding pitch. In Poly, a pitch bend can carry a held note across the selected channel’s range onto an available fallback. Editing a chord during an arpeggio advances from the last sounding pitch. Cartridge settings can be saved on an inactive board without restarting a held note.

Mono passages share the oversampling filters once both channels have the same history. The band-limited step renderer processes only pending filter tails. Disk master-volume writes reach the output filter at the same sample across different callback sizes. [Playback tests and CPU measurements](Docs/validation/0.5.2.md)

### 0.5.1 — 2026-11-19 — pulse attacks, bass glides and output switching

Console and expansion B pulse notes start on the documented duty sequence.
Expansion A's saw starts in the same phase after any disabled interval.
Handheld power-off now reaches the board output at the register-write cycle
and clears its mixer registers. On both machines the Pin and Line filters
follow the signal continuously, so changing the monitor tap after silence
does not replay an earlier transient.

Mono carries its envelope and glide across a channel's pitch limit and
returns to the selected part when it can play the pitch again. Arp can play
the next in-range step after an out-of-range starting note.

### 0.5.0 — 2026-11-19 — hardware blackbox, CPU bus drivers and analog fidelity

- **Strict hardware-blackbox / sound-driver split.** `Chips/` and `Boards/`
  emulate only the physical silicon, DACs, mixers, CPU address decoding, and
  analog stages (`Handheld` and `Console` accept only CPU `Write(addr, val)` /
  `Read(addr)`, `FinishBoot()`, `RunTo(cycle)`, and `Render()`). Multi-register
  updates queue with SM83 (`8`/`12` T-cycle) and 6502 (`4`/`6` CPU cycle) bus
  write spacing, including Sunsoft 5B `$C000`/`$E000` register latching. All
  musical translation now lives above the bus in `Driver/` (`HandheldDriver` and
  `ConsoleDriver`).
- **Handheld chip and analog authenticity.** Added CH3's 6-T-cycle trigger
  delay, the 2-T-cycle DMG Wave RAM retrigger corruption window with byte-0
  reload, active CH3 Wave RAM read/write lock (DMG) and played-byte redirect
  (CGB), consistent CGB-04/DMG zombie volume writes guarded by `channel.on` and
  envelope halt, DMG `NR52=0` length preservation and powered-off length
  writes, full register readback, and a `50 us` BLEP-aligned DAC turn-off
  fade passing continuously through the coupling capacitor and Color amplifier
  on Line.
- **Console chip and board authenticity.** Added `$4015` / `$4800` readback,
  VRC6 sawtooth 14-step accumulator reset on `$B002` `E=0`, rainwarrior RP2C33
  FDS negative-bias modulator rounding with immediate `latchedGain_` tracking
  and lazy `CatchUp()`, MMC5 `$5011` 8-bit PCM DAC connection into the mixer,
  and FDS `$4089` master-volume low-pass corner switching (`2 / 3 / 4 / 5 kHz`)
  preserving filter state.
- **Authentic driver techniques across the full compass.** Sub-32 Hz Handheld
  CH3 (`A0`–`B0`) now plays through a master-cycle `NR32` timer ISR over
  all-`$FF` Wave RAM rather than modifying chip silicon; Handheld `< 64 Hz`
  Pulse fallback loads exact CH3 Pulse duty tables; Noise retriggers on 15/7-bit
  LFSR width change mid-note; FDS and 8-channel Namco 163 fold harmonic cycles
  into wavetable RAM up to C8 without falling back to the APU Triangle; native
  volume resolution (`0..32` FDS, `0..42` VRC6 sawtooth, `0..3` CH3, `0..1`
  Triangle, `0..15` others) replaces uniform 4-bit truncation; N163 streams via
  `$F800` auto-increment bursts and 5B caches its `$C000` register latch.

### 0.4.0 — 2026-11-18 — one voice, two machines, envelopes in time

- **Two machines.** The PSG and the micro boards are gone, with their
  chips (the SN76489 and the beeper; the AY core stays, as the 5B's). The
  handheld and the console, with every revision and cartridge, remain.
- **One layer.** Layers A and B are one voice again, on one board's
  machines; Double still gives it a second set on the right. Every control
  is renumbered and the `a_` prefix dropped (`part`, `shape`, `level`, ...);
  no session or patch from 0.3.0 or earlier carries over (owner,
  2026-11-18: no backwards compatibility).
- **Envelope times in milliseconds.** Attack, Decay and Release are times,
  0 to 5000 ms on a cubic knob, where they were frames a volume step, which
  made 1 already a quarter of a second and left nothing between. The
  envelope moves a fraction of a step each frame and the chip is written the
  whole step it reaches, still once a frame; a time under a frame is at
  once. Decay is the time from full down to Sustain, Release from where the
  key leaves the note down to silence, and a change is heard at once. Init
  now decays over 400 ms to 10 and releases over 250 ms, so every envelope
  knob does something from Init. Each step of the arpeggio starts the
  envelope again. The triangle still has no volume: on it the envelope only
  gates the note, as on the console.
- **Every part at one loudness.** A level trim after the machine brings each
  part and shape of every machine, revision, cartridge and path to the
  loudness of the reference square (`Source/DSP/LevelTrims.h`, measured by
  `RebootLevels`, held to 0.05 dB by `Reboot.Levels`). Before it, the parts
  and shapes ranged from 5 dB under the reference (the APU's pulse behind
  the MMC5's board) to 4 dB over (the 163's brightest waves), and the 163
  dropped with every channel it ran. The chips play as before; only how
  loud each part arrives changes.
- **No thump on a reset.** A console chosen, or reset by a change of its
  revision, cartridge or count, used to step from nothing to its rest level
  (the triangle's held step and the DMC at Bias, up to 0.7 of full scale
  at Bias 127) through the output stage; it now starts settled on that
  level, silent.
- **Eight presets**, four a board, each with a demo: Wave Bass, Static
  Snare and Triangle Bass get phrases of their own, Pulse Glide plays as
  one voice doubled, and the times are re-set in milliseconds. Every demo
  is re-rendered.

### 0.3.0 — 2026-11-17 — a machine for each layer

- Layers A and B each play machines of their own: each has its own Board,
  Revision, Expansion, Expansion channels, Bias, Path and Boards, so a
  console's triangle can play under a handheld's pulses. The seven controls
  that were the device's are now layer A's, under the same ids, and layer B
  gets seven new ones (`b_board` to `b_boards`). A patch saved before 0.3.0
  with B on plays B on Init's handheld until B's board is set; the factory
  presets set it.
- Each layer moves on its own machines' frame, and its arpeggio steps there.
  Double gives each layer its own second set. A change of a layer's machine
  settings, or of its On, resets only that layer's machines; a layer that is
  off runs none.
- Pulse Glide and Plain Organ set B's board to A's. Two layers on one board
  now sum after two machines' mixers rather than inside one, so Pulse
  Glide's two pulses no longer share the console's nonlinear pulse DAC: it
  is 0.3 dB louder, with peaks 4.7 dB higher, and is re-rendered. Plain
  Organ keeps its six notes as two PSGs a layer, where it had four shared.

### 0.2.3 — 2026-11-17 — the 128K's TV sound

- On Line, the 128K now plays through its TV modulator's input, from its
  service manual: the beeper's pin through 68 kOhm and the AY through
  39 kOhm into one node. The beeper stands about 14 dB over one AY channel
  there, where Pin keeps them level. The AY's 0 to 1 V is the AY data
  manual's; the tied AY pins taken as their average and the 128K's pin
  taken as the 48K Issue 3's are a labelled estimate and sibling value
  (Known gaps), under the owner's new rule that sibling units and
  estimates may fill a gap until better material arrives.

### 0.2.2 — 2026-11-17 — the Color's Line from its own recording

- On Line, the Color is thin again, now by its own measurement: two
  high-passes, 176 Hz and 47 Hz, fitted to Herbert Weixelbaum's published
  waveform of a Color playing a 65.41 Hz square. 0.2.1 had given it the
  original's 28 Hz from a recording of the Pocket, which shares the Color's
  amplifier chip; but the corner is set by a capacitor on the board, and the
  same recordings put the Pocket at 96 and 25 Hz and the Color well under
  it. The original's 28 Hz is unchanged and fits its recording at 26 Hz.
  Pocket Arp is re-rendered.

### 0.2.1 — 2026-11-17 — the Color's bass

- On Line, the Color's coupling capacitor is the original's, a corner near
  28 Hz instead of Pan Docs' 706 Hz: a recording of the Pocket, which uses
  the Color's amplifier chip, rejects 706 Hz. The Color keeps its bass:
  Pocket Arp is 4.5 dB up in RMS and its peaks 1.8 dB down, since the
  706 Hz corner had turned each pulse edge into a spike.

### 0.2.0 — 2026-11-17 — two cartridge boards on Line

- On Line, the MMC5 cartridge's board: the APU and the MMC5 pass
  15 kOhm and 100 nF into its inverting amplifier with 10 kOhm feedback, so
  the whole console is high-passed at 106 Hz, two thirds as loud and
  inverted (the NESdev Wiki's HVC-ETROM-02 drawing).
- On Line, the 5B's amplifier as rainwarrior measured a Gimmick! cartridge:
  its inputs high-passed at 12 Hz (the APU) and 20 Hz (its own channels), a
  2.7:1 curve from 1 dB above one channel at volume 14, then 12 Hz. A 5B
  channel at volume 15 now comes out 1.26 dB under its linear level, and a
  loud 5B squeezes the APU with it.

### 0.1.0 — 2026-11-17 — first pass: four boards, five cartridge chips, sixteen presets

- The handheld, console, PSG and micro boards with their revisions, the
  Famicom's five cartridge chips, the band-limited step renderer, the
  playing layer (two layers, Poly, Mono and Arp, envelopes in the chip's
  volume steps, vibrato, glide, bend, the host's master tuning) and eight
  demos.
- Oversampling at 1x (the default), 2x, 4x or 8x, and Double: a second set
  of machines on the right, detuned against the first on the left.
- Authenticity pass: the handheld's pulse and wave triggers as Pan Docs
  states them, the 5B at its measured level (7.7 dB over the APU's pulse,
  where it was set equal), and the ULA's contention of the beeper's loop.

## Downloads

There is no build to download yet.

---

## How it works

**The signal path.** A key becomes legal register writes pushed onto a machine's
CPU bus on the cycle its MIDI event (or timer/VBlank frame) falls on, spaced by
SM83 (`8`/`12` T-cycle) or 6502 (`4`/`6` CPU cycle) store timing. The board's
chips run from event to event on their master clocks and emit each channel's
DAC code whenever one changes; the board turns the codes into levels with its
DAC and mixer (and per-channel `50 us` DAC-off decay on Handheld Line) as
band-limited steps in time, exact to the clock cycle. The steps pass either the
board's own output stage (**Line**) or only a DC blocker (**Pin**), then the
part's level trim, Volume and the portfolio's output stage. A machine chosen
starts as one that has stood at rest, its output stages settled on the resting
levels left after initial driver boot writes, so a reset or a new Bias at reset
does not thump; a Bias moved while playing steps as a `$4011` write does.

```
keys ─▶ sound driver ─▶ CPU bus writes (SM83 / 6502) ─▶ board blackbox (chips + DACs + mixer)
     ─▶ steps ─▶ band-limited at the host rate ─▶ Pin: 5 Hz DC blocker │ Line: output stage
     ─▶ trim ─▶ Volume ─▶ limiter at -1.5 dBFS
```

**Where every value comes from.** Each mechanism carries one of these tags
in the code beside it. A stage that nothing defensible describes is left out
and listed under Known gaps, and so is every sibling value and estimate, to
be replaced when a source arrives.

| Tag | Meaning |
| --- | --- |
| spec | stated by the maker's documents or an authoritative technical reference (Pan Docs, the NESdev Wiki, the AY data manual for the 5B's core) |
| reported | a measurement or fit someone else published, cited beside the code; it ships labelled until our own measurement replaces it |
| ours | the playing layer and the conventions that let a keyboard play the chips; never claimed as hardware |
| sibling | a value a closely related unit or board documents, borrowed with the reason it transfers |
| estimate | a typical part or an engineering judgement, with its reasoning beside it |

The core uses the hardware references above. The record of materials
reviewed, including the 0.5.1 pulse-sequencer audit, is in the
[third-party notices](THIRD_PARTY_NOTICES.md).

### The handheld (Game Boy, Game Boy Color)

- **Chip (spec; CH3 delay, DMG corruption window and DMG zombie writes reported):** Pan Docs
  (CC0): two pulses with four duties, the first with its period sweep, a
  32-sample 4-bit wave channel, a noise channel with its 15- and 7-bit LFSR,
  length counters, volume envelopes and the 512 Hz frame sequencer on the gbdev
  wiki's step map, clocked at 4194304 Hz. Quirks: the first duty step after the
  first trigger plays as 0, duty position survives a trigger, pulse triggers
  keep the low two bits of the frequency timer, wave triggers play the previous
  sample buffer's high nibble before index 1, active CH3 Wave RAM writes/reads
  are locked (DMG 2-T-cycle window) or redirected (CGB), CGB `$FF76`/`$FF77`
  (`PCM12`/`PCM34`) digital output readback (`0xFF` on DMG), CGB-04's zombie
  volume writes (`NRx2` while a channel plays), DMG `NR52=0` length
  preservation, and, reported from SameBoy (Expat), CH3's 6-T-cycle trigger
  delay, the 2-T-cycle DMG Wave RAM retrigger corruption window, the 64 Hz
  envelope clock high-phase (`divStep == 7`) step-timer reload on `NRx2`
  writes, and the
  original's zombie writes: each acts as two, the old value to `$FF` and then
  `$FF` to the new, counting the volume a step when a pace appears or `$x8`
  meets `$x8` and mirroring it when the direction turns. Revisions: the original (DMG) and the Color (CGB).
- **DACs and mixer (spec, 50 us decay reported):** a DAC on maps 0 to 15 to
  analog +1 to -1, off decays toward 0 over 50 us on Line and clamps back to
  the driven rail when re-enabled; NR51 sends channels
  to each side and NR50 scales the sides.
- **Line (reported):** Pan Docs' coupling capacitor, driven continuously (with
  the 50 us DAC-off decay) at corners fitted to Herbert Weixelbaum's recordings
  (herbertweixelbaum.com, "game boy sound comparison": each machine at its
  highest volume into an audio interface, playing a 50 % pulse at 65.41 Hz,
  shown as waveforms; `Tools/fit_handheld_line.py`). The original fits one
  pole at 26 Hz and keeps Pan Docs' factor 0.999958 per clock, 28 Hz. The
  Color's waveform falls away from each edge and undershoots; it fits two
  first-order high-passes, 176 Hz and 47 Hz, the capacitor at 47 Hz and,
  after it, the amplifier, which Ken Shirriff's die analysis of the Color's
  IR3R53N says has "an external feedback capacitor, which causes it to act
  as a high-pass filter". Pan Docs' single 706 Hz pole for the Color fits
  neither. Those recordings are the socket into a line input, where its
  coupling capacitor's corner is under 1 Hz. **Load** Headphones adds that
  capacitor into its load: 100 uF (the original's headphone board's C1 and
  C2, in repair kits' parts lists; reported; the Color's taken as the same,
  sibling) into 32 Ohm with nothing in series (estimate), a high-pass at
  49.7 Hz. The amplifier's gain and clipping and the speaker are not
  modelled.

### The console (NES, Famicom, PAL NES)

- **Chip (spec):** the NESdev Wiki's APU pages: two pulses with sweep, the
  triangle, the noise channel's two modes, the DMC's held level, the
  envelopes, length counters, `$4015` status and frame/DMC interrupt flags,
  and the frame counter, in CPU
  cycles at the NTSC 1789773 Hz (19687500 / 11) or the PAL 1662607 Hz, with
  the PAL 2A07's own noise and DMC period tables. A pulse period change that
  crosses the high bits writes $4003 or $4007, as a driver must, and the phase
  reset it forces is heard.
- **Mixer (reported):** the wiki's nonlinear formulas,
  pulse = 95.88 / (8128 / (p1 + p2) + 100) and tnd = 159.79 /
  (1 / (t / 8227 + n / 12241 + d / 22638) + 100), which the page calls
  approximate; Brad Smith's DAC measurements of a front-loader and a
  Famicom found its curves "reasonably good", with variance between units
  (NESdev forum t=24288). **Bias** sets the DMC's held level, which moves
  the triangle and noise through that mixer.
- **Line (reported):** the front-loader's first-order high-pass at 90 Hz
  and low-pass at 14 kHz (the wiki's second-order low-pass sits "comfortably
  above the audible range"); the Famicom's high-pass at 37 Hz. The PAL
  console takes the NTSC front-loader's, unsourced. Before that stage, two
  cartridges' own boards:
  - **MMC5 (spec):** the wiki's HVC-ETROM-02 drawing sends the APU's audio
    (cartridge pin 45), the MMC5's DAC (`$5011` at 7-bit `$4011` step scale)
    and its pulses each through 15 kOhm and 100 nF into the inverting input of
    the MMC5's amplifier, with 10 kOhm of feedback, out to pin 46. Everything
    the console plays is high-passed at 106 Hz and scaled by -2/3, taking that
    input as a virtual ground as the drawing's internal reference implies.
  - **5B (reported, one cartridge):** rainwarrior's fit to his recordings
    of Gimmick! (NESdev forum t=17745 and p=225726, September 2018): the
    APU's input high-passed at 12 Hz and the 5B's channels at 20 Hz, mixed
    in its amplifier, which is linear for one square "up to and including
    volume 14" and above that follows "a threshold set 1dB above volume 14
    ... with a compression ratio of 2.7:1 dB", then its output high-passed
    at 12 Hz. He found the onset immediate and the stage "just a
    nonlinearity", so the curve acts on the instantaneous level after the
    input high-passes (our reading): a centred square at volume 14 meets
    it 1 dB under the threshold, and the APU passes the same curve.
- **Cartridge chips (spec, levels reported):** they sound only on the
  Famicom revision, whose cartridge slot returns audio (NESdev Wiki,
  "Expansion audio"). Expansion A is the VRC6 (two pulses with eight
  duties, the accumulating sawtooth with `$B002` `E=0` clock reset, the halt
  and 16x/256x frequency bits); B the MMC5 (two pulses without sweep plus
  `$5011` 8-bit PCM DAC); C the Namco 163 (one to eight wavetable channels in
  128 bytes of RAM, updated one every 15 CPU cycles through one DAC, so eight
  channels whine as the chip does); D the disk unit (a 64-step wave, its
  modulator with its 32-step table, RP2C33 negative-bias modulator rounding,
  master volume and `$4089`-coupled `2 / 3 / 4 / 5 kHz` 2–2R low-pass); E the
  5B (the AY core addressed via `$C000`/`$E000` at a 16-cycle tick with its
  1.5 dB DAC steps). Their levels against the APU are what each chip's wiki
  page reports: the VRC6 at the APU pulse's level and inverted, the MMC5 equal
  and inverted, the 163 12 dB over the APU's square (the page gives 11 to 13),
  the disk unit at 2.4 times a square. The 5B's is Brad Smith's survey
  (nes-audio-tests, db_5b): its square at volume 12 measured 1.3 dB under the
  APU's at full volume, so its loudest channel is 7.7 dB over the APU's pulse.

### The band-limited renderer

A board's output is exactly a list of steps, each on a clock cycle. Each step
adds a band-limited impulse (a Kaiser-windowed sinc, beta 10, 33 taps, 256
phases, from `Tools/generate_blep.py`) into a difference buffer in integer
units, and integrating it gives the steps at the host's rate: no aliasing
folds back below half the host rate, every phase sums to exactly one, so the
level returns exactly after any number of steps, and the output is the same
bits on every processor. The machine clock and the host rate meet in exact
integers (`Render/Timebase.h`), so nothing drifts however long it plays. A
channel that cannot be heard is not stepped at all: its counters and noise
register are jumped ahead exactly when it can be (`Chips/Lfsr.h`).

### Oversampling and Double (ours)

- **Oversampling** renders the machines at 2, 4 or 8 times the host rate and
  brings them back through one to three 2:1 halfband stages (67 taps, flat
  to 0.2 and at least 98 dB down from 0.3 of each stage's input rate, from
  `Tools/generate_halfband.py`). The steps are then band-limited at the
  higher rate, and the boards' analog stages, whose corners are designed by
  the bilinear transform, sit further from that rate's Nyquist. Below 19.2
  kHz at 48 kHz the result is within 0.05 dB of 1x. The factor is halved
  until the machines run at or under 384 kHz, so 8x at 96 kHz runs at 4x.
  It is setup, not a control (`Engine::Prepare`), and starts at 1x. Each
  stage delays by whole host samples, so the latency is 16 samples at 1x,
  then 24, 28 and 30 at 2x, 4x and 8x, plus the output stage's 96 at 48
  kHz.
- **Units.** Each machine is one unit, the same unit every time: the one
  with its place's serial number (`Boards/Board.h`, `DrawTraits`). Serial
  0 is the nominal unit; the others are drawn from the board's spread. The
  handheld's crystals lie 20 ppm apart (Pan Docs, Specifications: real
  units "tend to run about 50-70 PPM slow"; reported, the nominal unit
  kept at the documents' 4194304 Hz), the console's 1.9 ppm either way
  (NESdev, Cycle reference chart: "21.477272 MHz ± 40 Hz"; reported). A
  unit is heard as one that has stood since its driver's start, for up to
  a second on the handheld and 262144 cycles on the console, so its video
  frame, dividers and noise register stand where that leaves them. Its
  output level lies within 5 % and every corner of its output stage within
  10 % of nominal (estimate: what 5 % resistors and 10 % capacitors give).
- **Double** is a second set of units of the same board, set up the same
  and sent the same notes at the same moments (owner, 2026-11-18: "double
  is just having two devices of the same kind with the same setup and
  wiring them to separate channels"). The first set plays on the left and
  the second on the right, each folded to mono at the level one side of a
  single set has: two machines side by side, as chip musicians play two
  handhelds live. With no detune the left is the single set's sound, bit
  for bit, and the right is another unit's: on the console a slow attack's
  first step comes 16 ms apart on the two sides, and the second unit's
  level sits 3 % above the first's. Detune pulls the two sets' crystals
  apart, half of it either way, so the whole unit runs fast or slow: its
  pitch, its frames and its envelopes together, the period registers
  unchanged. A change of Double resets the units.
- **CPU** on the development container (Release, GCC 13.3), six held keys
  on each preset at 48 kHz, measured on 0.2.3: 0.16 % of a core on average
  and 0.9 % at worst (Disk Organ's four consoles) at 1x; 1.1 % and 2.4 % at
  8x; with Double, 0.28 % and 1.8 % at 1x, 1.5 % and 3.9 % at 8x. These
  are not Reason's figures.

### The sound driver (ours)

The layer that turns keys into register writes (`Source/DSP/Driver/`) is a
sound driver of our own, and it only pushes legal CPU bus writes onto each
machine blackbox.

- **Pitch** is a period register, chosen as the one nearest in pitch (in
  ratio, not in count), so the chip's quantisation and out-of-tune top notes
  are its own. A key below a part's reach goes to its board's fallback: the
  handheld's CH3 below the pulses' 64 Hz (loaded with exact Pulse duty tables),
  extended below 32 Hz (`A0`–`B0`) via a master-cycle `NR32` timer ISR over
  all-`$FF` Wave RAM (ours); the console's triangle below the pulses' 54.6 Hz.
  On FDS above ~1747 Hz and 8-channel Namco 163 above ~3728 Hz, the driver folds
  multiple harmonic cycles into wavetable RAM (and on FDS into zero-sum `$4088`
  modulation tables with compensated depth so FM stays 1:1 harmonic) so high
  notes up to C8 play on the cartridge chip itself. On DMG CH3, active
  retriggers turn `NR30` off before re-enabling so rapid arpeggios never hit
  the 2-T-cycle Wave RAM corruption window.
- **Frames.** Everything that moves while a note sounds (the envelope's
  volume steps, vibrato, glide, the pitch wheel, the host's master tuning)
  is written at the unit's own frame, as a game's driver ran on its video
  frame: 59.73 Hz on the handheld, 60.10 or 50.01 Hz on the console, each
  unit's counted from its own power-on on its own crystal. The driver
  knows only the nominal clock, so a unit whose crystal runs off plays as
  far off. **Driver rate** ticks the driver 2 to 8 times a frame instead,
  as a driver on a timer interrupt does (the handheld's timer, Pan Docs
  "Timer and Divider Registers"; on the console a cartridge's cycle
  counter or the DMC's IRQ), as LSDj's FAST mode and an NSF's play-speed
  field use; whole ticks a frame are ours. A note's own start and end land
  on its sample, and CH3's `NR32` timer ISR fires at its exact master
  cycle between ticks.
- **Sync.** With Sync the arpeggio's step, the vibrato's cycle and a
  glide's time are note values at the song's tempo, which the host passes in (`Engine::SetTempo`,
  120 BPM until it does). The driver still ticks on its frames. A step is
  due a note value after the last, counted in time from the arpeggio's first
  key; while the song plays (`Engine::SetSongPosition`), that key's step
  starts on the line of the note value's grid nearest it, so the steps keep
  the song's grid however early or late the key. The tick nearest each
  step's time takes it: the steps mix lengths of n
  and n + 1 ticks, as a tracker's rows do at a tempo its frames do not
  divide, are never more than half a tick off (8.4 ms at Driver rate 1 on
  the handheld, 1 ms at 8) and never drift. A 16th is a whole number of the
  handheld's frames only at 895.9 / n BPM (179.2 at 5 frames, 128.0 at 7,
  59.7 at 15). The vibrato moves each tick by the share of its cycle that a
  tick is at the tempo, and a glide takes the whole number of ticks nearest
  its note value (Off slides at once). A new tempo or note value keeps the
  next step's place in its step. Ours.
- **Envelopes** are times: Attack from silence to full, Decay from full
  down to Sustain, Release from where the key leaves the note down to
  silence. Each frame the envelope moves by the share of its stage that a
  frame is, and the chip's own volume register is written the native step
  it has reached (`0..32` on FDS `$4080`, `0..42` on VRC6 sawtooth `$B000`,
  `0..3` on handheld CH3, `0..1` gate on APU triangle, `0..15` elsewhere;
  handheld pulses and noise step by `$08` zombie writes). On handheld CH1/CH2
  and noise, switching 15/7-bit LFSR width retriggers cleanly to prevent all-1s
  lock-up, and retriggering a silent `$08`-parked channel skips redundant `$08`
  writes so DMG zombie mode cannot blip `0 -> 1` before trigger. Mono's legato
  notes keep their envelope; each Arp step starts it again, unless Arp
  retrigger is off (LSDj keeps the two apart: its chord command moves the
  pitch, and a retrigger is another command).
- **The chips' own envelopes.** **Envelope** Chip hands a note's volume to
  its channel's envelope unit where it has one, and the trigger starts it:
  the handheld's pulses and noise (`NRx2`, a step every 1 to 7 64ths of a
  second, climbing or falling from the volume written); the console's pulses
  and noise and the MMC5's pulses (`$4000` bit 4 clear, falling from 15 every
  1 to 16 quarter frames, or of the MMC5's 241 Hz ticks); the disk unit's
  gain (`$4080` bit 7 clear, a step every 8 × 233 cycles times 1 to 64, up
  to 32 or down to 0 from the gain written); and the 5B's envelope
  (`R11`–`R13`, two of its 32 steps a volume step, falling once, climbing
  once or falling again and again from its top). A note with an Attack
  climbs from 0 to the top where the unit can climb; else it falls from its
  start at the pace nearest the step the driver's Decay would take, and runs
  on past Sustain to 0, since a unit stops only at an end; at Sustain 15 the
  driver holds the level. Release retriggers a falling unit from where the
  driver counts it (the handheld's and the disk unit's), lets the console's
  run on to 0, and takes a climbing or looping note back to the driver's
  steps. **Loop** (Envelope's third setting) lets the console's, the MMC5's
  and the 5B's fall again and again. The 5B has one envelope for its three
  channels: the newest note takes it, and the notes it played go on under
  the driver from where they were. A note handed to a channel without a
  unit (a fallback) goes back to the driver, and changing Envelope plays the
  held keys again. The mapping of times to paces is ours; the driver counts
  the unit's steps from the trigger, which can be a step from the chip's own.
- **Sweeps and sides.** **Sweep** writes the pitch sweep of the handheld's
  first pulse (`NR10`, every 1 to 7 128ths of a second; Sweep pace 8 plays
  as 7) and the console's pulses (`$4001`, every 1 to 8 half frames) before
  each note's trigger: + raises the pitch and − lowers it, a step's size 8
  minus the shift. The driver then leaves the swept pulse's period to the
  sweep, so vibrato, glide and the wheel do not reach it, and a sweep that
  runs out of range turns the handheld's channel off or mutes the console's,
  as the chips do. **Sides** Spread writes `NR51` so the handheld's first
  pulse plays on the left only and its second on the right only, the wave
  and noise on both.
- **Voices.** One voice plays up to eight machines of one board
  (**Boards**, 6 by default), with its revision, expansion, bias and path. A
  chord packs onto the free channels of a machine already holding a key
  before taking an idle machine (so pulses share one chip's mixer or stereo
  sides and 5B/163 chords share one cartridge), while separate phrases take
  whichever idle machine was freed longest ago; once all free channels are
  taken, a note takes the oldest released note's, else the oldest note's.
  With **Poly channels** on, a Poly chord spills onto the chip's other
  pitched parts once every channel of the part is held, as one machine
  plays a chord on its unlike voices (mGB's POLY channel plays the
  handheld's two pulses and wave so); the noise takes no part.
  **Mode** is Poly, Mono (legato, with glide) or Arp (one voice stepping
  through the held keys every Arp rate ticks, in **Arp order**: up, down,
  up and down without repeating the ends, or as played). The arpeggio is
  the player's: it steps on the first unit's tick, and each step reaches
  every unit at that moment on its own clock, as a link cable carries a
  master's ticks.
- **Waves** for the wave channels are our own harmonic waves
  (`Tools/generate_waves.py`), scaled to each chip's range.
- **Level.** Every part and shape arrives at one loudness: that of one
  channel playing a 50 % square at full volume at -24 dBFS RMS, measured
  as ITU-R BS.1770's K-weighted mean square over a held C3, C4 and C5
  (`Source/DSP/LevelTrims.h`, by `Tools/RebootLevels.cpp`). The trim is a
  gain after the machine, a fader on the mixing desk: the chip, its DACs,
  its mixer and its output stage play as they do, and the steps of Level,
  velocity and the envelope keep their own sizes. A change of part or shape
  moves the trim across one block. Volume trims from there, moving over 64
  samples when it changes so that turning it does not step the output
  (ours), and the portfolio's output stage keeps every sample under
  -1.5 dBFS.

### Controls

| Control | Range | What it does |
| --- | --- | --- |
| Board | Handheld, Console | the machine |
| Revision | per board (above) | the machine's model |
| Expansion | none, A to E | the Famicom's cartridge chip |
| Expansion channels | 1 to 8 | how many channels expansion C runs |
| Bias | 0 to 127 | the console's held DMC level |
| Path | Pin, Line | the chips' DACs and mixer, or the machine's output stage |
| Boards | 1 to 8 | the machines played at once |
| Part, Shape | per board | the channels the voice plays, and their duty, wave or noise mode |
| Level, Sustain | 0 to 15 | the chip's volume steps, and the held share of them |
| Attack, Decay, Release | 0 to 5000 ms | the envelope's times |
| Transpose, Fine, Mod | ±24 semitones, ±50 cents, 0 to 63 | pitch, and the disk unit's modulation depth |
| Mode | Poly, Mono, Arp | how keys take voices |
| Arp rate | 1 to 8 ticks | ticks an arpeggio step holds, without Sync |
| Vibrato, rate, delay | 0 to 100 cents, 0.5 to 12 Hz, 0 to 1000 ms | written at the frame; the mod wheel adds up to 50 cents; the rate without Sync |
| Glide | 0 to 1000 ms | a Mono legato note's slide, without Sync |
| Bend range | 0 to 12 semitones | the pitch wheel |
| Velocity | 0 to 1 | how far a key's velocity scales the level |
| Double, Detune | Off, On; 0 to 50 cents | a second set of units on the right; Detune pulls the two sets' crystals apart |
| Volume | -24 to +6 dB | our trim after the machines |
| Poly channels | Part, Chip | Poly on the part's channels, or the chip's other pitched channels too once the part's are held |
| Arp order | Up, Down, Up and down, As played | the arpeggio's order |
| Arp retrigger | Off, On | whether each arpeggio step starts the envelope again |
| Driver rate | 1 to 8 ticks | the driver's ticks a video frame |
| Envelope | Driver, Chip, Loop | who steps the volume: the driver each tick, or the channel's own envelope unit from its trigger, looping where the unit can |
| Sweep, Sweep pace | -7 to 7; 1 to 8 | the pulses' pitch sweep from each note's start (+ up, − down, a step's size 8 minus the shift), and its steps apart |
| Sides | Both, Spread | the handheld's channels on both sides, or its first pulse left and its second right |
| Load | Line input, Headphones | what the handheld's socket drives on Line |
| Sync | Off, On | Arp rate, Vibrato rate and Glide as note values at the song's tempo, on the driver's nearest ticks |
| Arp sync rate | 1/64, 1/32T, 1/32, 1/16T, 1/16, 1/8T, 1/8, 1/4 | an arpeggio step, with Sync |
| Vibrato sync rate | 1/1, 1/2, 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T | a vibrato cycle, with Sync |
| Glide sync time | Off, 1/64, 1/32, 1/16, 1/8, 1/4, 1/2, 1/1 | a glide, with Sync |
| Oversampling | 1x, 2x, 4x, 8x | setup, not a control: the machines' rate over the host's (`Engine::Prepare`) |

The control ids, ranges and defaults are `Source/DSP/RebootControls.h`, which
every front end reads; the factory presets are `Source/DSP/RebootPresets.h`.
A front end may show one **Sound** list in place of Board, Expansion and Part
(`kSounds`, written by `SetSound`; ours): Handheld Pulse, Wave and Noise,
Console Pulse, Triangle and Noise, and the cartridges' A Pulse, A Saw,
B Pulse, C Wave, D Disk and E Square, every part the machines offer once.
A cartridge's sound picks the Console revision whose slot carries it.

## Known gaps

- **Analog stages without a source are not modelled.** The handheld's
  amplifier and speaker: the amplifier's gain and clipping point are not
  published, and neither is anything of the speaker's response. Load's
  headphones are an estimate, 32 Ohm with nothing in series, on the
  original's 100 uF (reported from repair kits' parts lists, not a traced
  drawing); the Color's socket is taken as the original's (sibling).
- **The handheld.** The original's zombie writes, CH3's 6-T-cycle
  trigger delay, the 2-T-cycle DMG Wave RAM retrigger corruption window and
  the 50 us DAC-off decay are reported from SameBoy (Expat/MIT) rather than
  our own hardware capture. SameBoy notes that `$x0` zombie writes "seem to
  be non-deterministic" on the original;
  Pan Docs notes that DAC decay varies between handheld models. The Color's corners are one unit's,
  read from a published picture of its waveform rather than from its
  samples, and fits within 20 % of the best error span 132 to 216 Hz and
  36 to 66 Hz; neither capacitor's value is published. The same recordings
  put a Pocket, on the Color's amplifier chip, at 96 and 25 Hz, 4.5 dB
  under the original at 71 Hz, near kathoc's 2.8 dB from his own recording
  of a Pocket (note.com, 2026-08-29).
- **The console.** The APU mixer is the wiki's approximation: Brad
  Taylor's 2A03 reference reports, from others' unnamed measurements, that
  with $4011 at $7F "the triangle & noise channel outputs operate at only
  57% total volume", where the formulas give about 45 %. The PAL
  console's line stage is the NTSC front-loader's, assumed; blargg's
  step-response fit to a front-loader also found a 442 Hz high-pass that
  no component on the drawing accounts for (lidnariq, NESdev forum t=4946)
  and the wiki no longer lists, so it is left out. The MMC5 is
  mixed linearly at the APU pulse's step (our reading of "equivalent in
  volume"); the 163's level is the middle of a reported range and its
  polarity unstated; the disk unit's page still has open questions. The
  5B's level and its amplifier are one cartridge's measurements, and its
  amplifier is the hard knee rainwarrior published: he wrote that it
  needs "a soft knee curve" but gave none, and that the curve's control
  sees "a different, weaker highpass" than the output, which his later
  filter list (12, 20 and 12 Hz) does not separate; the 5B's polarity
  against the APU is unstated. The clip_5b table in nes-audio-tests, his
  measured mixes of the 5B's three squares, carries this knee as its
  model: the measurements sit within 0.55 dB of it from volume 12 up and
  start to compress about half a dB before the threshold, a softer onset
  no published curve gives. The MMC5 board's amplifier is taken as
  inverting with a virtual-ground input; an older copy of the wiki's
  drawing shows its inputs the other way round. The survey thread (NESdev
  forum t=17741) also puts one VRC6 cartridge 1.1 dB under the APU's
  square and the 163's cartridges from 11.6 to 19.2 dB over it, against
  the wiki's figures used here. NES 2.0 sorts the 163's boards into three
  measured bands, 11 to 13, 16 to 17 and 18 to 19.5 dB (submappers 3 to
  5), which the boards' resistors do not predict; ours is the first.
- **Ours, by design.** The sound drivers, the handheld's `NR32` timer ISR
  below 32 Hz and FDS/N163 multi-cycle high-octave wavetable folding, the noise
  pitch rule (a key picks the noise's colour), the period nearest in pitch, the
  envelope's times and the Arp's restart of it, the output stages settled at
  reset, the 5 Hz Pin blocker, the level trims and their reference, the
  driver's ticks a frame, the player that spreads notes over units and
  oversampling's decimator are ours, not the hardware's.
- **Units are estimates.** The level (5 %) and corner (10 %) spreads are
  typical parts' tolerances, not measurements of units; no one has
  published how far two handhelds or consoles differ. The handheld's
  nominal unit keeps the documents' clock where Pan Docs reports real ones
  run 50 to 70 ppm slow, and its spread is that report's 20 ppm range. The
  warm-ups are ours. Detune's pull reaches 25 cents, 1.5 %, which no
  crystal's trimmer reaches: it stands for a unit built on another
  crystal.
- **The chips' own envelopes, as the driver keeps them.** Envelope Chip's
  paces, its decay past Sustain to 0, its releases and the 5B's envelope
  going to the newest note are ours. The driver counts a unit's steps from
  its trigger, which can be a step from the chip's own sequencer, and
  decides from that count when a note has ended. The VRC6's and the 163's
  channels, the triangle and the handheld's wave channel have no envelope
  unit and stay under the driver.
- **The level trims' limits.** The trims match a part's single notes, at
  full level with Bias 0 and Mod 0, by K-weighted loudness; whether that or
  plain RMS (Reason's meter) is the better match for these tones has not
  been decided by ear. Notes a part hands to its fallback keep the board's
  own balance against it (the handheld's wave channel under its pulses, the
  console's triangle under its pulses), and chords on the console's
  nonlinear mixer and the 5B's amplifier sum as the hardware sums them, not
  as their trimmed single notes would.
- **Not measured.** No value here is our own capture of real hardware yet;
  every analog figure is reported. No one has listened to these demos
  against the machines.
- **The FM chips, the SID, OPM and OPLL are not here.** The FM console's
  YM2612/YM3438 waits on public evidence of its output stage; the SID's
  filter differs from chip to chip.

## Build

CMake 3.22 or newer and a C++20 compiler; the engine itself is C++17.

```sh
cmake -S . -B build-dsp -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-dsp --parallel 2
ctest --test-dir build-dsp -L unit --output-on-failure
```

`Reboot.Chips` checks every core against the documents' own formulas (duty,
periods, envelope shapes, noise registers jumped against single steps) and
that a core gives the same output however its run is cut and after any
silence; `Reboot.Boards` the pitch, range and level of every board against
independent formulas; `Reboot.Engine` the playing layer; `Reboot.ParameterReach`
that every control reaches held and new notes; `Reboot.Levels` (`audio`)
that every part and shape arrives within 0.05 dB of the reference
loudness (`build-dsp/RebootLevels --write Source/DSP/LevelTrimData.h`
measures the table again after a change to a chip, a board or the
measure). `Reboot.ParameterMatrix`
(`fuzz`) and `Reboot.DspFuzz` (Clang, `-DREBOOT_BUILD_FUZZER=ON`) push every
control to its extremes and the engine's input to garbage.

## Licensing

Reboot is source-available under the PolyForm Noncommercial License 1.0.0
with an additional permission for official builds; see [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Game Boy, NES, Famicom
and the chip names are trademarks of
their owners, named here only to say which hardware is emulated; Reboot is
not affiliated with or endorsed by any of them.
