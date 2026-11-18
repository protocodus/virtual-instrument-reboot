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
on four consoles.

<!-- audio-demos-begin -->

| # | Preset | Demo | Length | Peak |
| --- | --- | --- | --- | --- |
| 1 | Square Lead | [01-square-lead.wav](Docs/audio/01-square-lead.wav) | 9.7 s | -11.6 dBFS |
| 2 | Wave Bass | [02-wave-bass.wav](Docs/audio/02-wave-bass.wav) | 8.4 s | -13.1 dBFS |
| 3 | Pocket Arp | [03-pocket-arp.wav](Docs/audio/03-pocket-arp.wav) | 10.4 s | -17.2 dBFS |
| 4 | Static Snare | [04-static-snare.wav](Docs/audio/04-static-snare.wav) | 10.8 s | -15.5 dBFS |
| 5 | Triangle Bass | [05-triangle-bass.wav](Docs/audio/05-triangle-bass.wav) | 8.3 s | -13.2 dBFS |
| 6 | Pulse Glide | [06-pulse-glide.wav](Docs/audio/06-pulse-glide.wav) | 9.0 s | -16.8 dBFS |
| 7 | Saw Lead | [07-saw-lead.wav](Docs/audio/07-saw-lead.wav) | 8.4 s | -18.6 dBFS |
| 8 | Disk Organ | [08-disk-organ.wav](Docs/audio/08-disk-organ.wav) | 10.2 s | -8.1 dBFS |

<!-- audio-demos-end -->

`build-dsp/RenderRebootPresetDemos Docs/audio/midi <out-dir>` renders them,
48 kHz 24-bit stereo, with the `demos.json` from which `python3
Tools/update_readme_blocks.py demos --readme README.md --manifest
<out-dir>/demos.json` writes the table. There is no main build yet, so the
WAVs are rendered and committed by hand after a change that alters them.

## Version history

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

**The signal path.** A key becomes register writes on the cycle its MIDI
event falls on. The chip core runs from event to event on its master clock
and gives each channel's DAC code whenever one changes; the board turns the
codes into levels with its DAC and mixer, which is a list of steps in time,
exact to the clock cycle. The steps are band-limited to the host's rate, then
pass either the board's own output stage (**Line**) or only a DC blocker
(**Pin**), then Volume and the portfolio's output stage. A machine chosen
starts as one that has stood at rest, its output stage settled on the level
the console's held triangle step and DMC leave, so a reset or a new Bias at
reset does not thump; a Bias moved while playing steps as a $4011 write
does.

```
keys ─▶ playing layer ─▶ register writes at chip cycles ─▶ chip core (integer, event to event)
     ─▶ DAC codes ─▶ board DAC and mixer ─▶ steps ─▶ band-limited at the host rate
     ─▶ Pin: 5 Hz DC blocker │ Line: the machine's output stage ─▶ Volume ─▶ limiter at -1.5 dBFS
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

No die-level netlist was read, no emulator's code was copied, and no
copyleft or non-commercial source was read. Ayumi (MIT), MAME's ay8910
(BSD-3-Clause) and the documents above were read for the rules they state;
the code is our own.

### The handheld (Game Boy, Game Boy Color)

- **Chip (spec):** Pan Docs (CC0): two pulses with four duties, the first
  with its period sweep, a 32-sample 4-bit wave channel, a noise channel
  with its 15- and 7-bit LFSR, the length counters, volume envelopes and
  the 512 Hz frame sequencer on the gbdev wiki's step map, on the
  4194304 Hz clock. Quirks Pan Docs states for each model: the first duty
  step after the first trigger plays as 0, the duty position survives a
  trigger, a pulse trigger keeps the low two bits of its frequency timer, a
  wave trigger plays the sample buffer's high nibble before index 1, the
  "zombie" volume write every unit agrees on ($08: one step up).
  Revisions: the original (DMG) and the Color (CGB).
- **DACs and mixer (spec):** a DAC on maps 0 to 15 to analog +1 to -1, off
  gives 0; NR51 sends channels to each side and NR50 scales the sides.
- **Line (reported):** Pan Docs' coupling capacitor, held while every DAC
  is off, at corners fitted to Herbert Weixelbaum's recordings
  (herbertweixelbaum.com, "game boy sound comparison": each machine at its
  highest volume into an audio interface, playing a 50 % pulse at 65.41 Hz,
  shown as waveforms; `Tools/fit_handheld_line.py`). The original fits one
  pole at 26 Hz and keeps Pan Docs' factor 0.999958 per clock, 28 Hz. The
  Color's waveform falls away from each edge and undershoots; it fits two
  first-order high-passes, 176 Hz and 47 Hz, the capacitor at 47 Hz and,
  after it, the amplifier, which Ken Shirriff's die analysis of the Color's
  IR3R53N says has "an external feedback capacitor, which causes it to act
  as a high-pass filter". Pan Docs' single 706 Hz pole for the Color fits
  neither. The amplifier's gain, the headphone socket and the speaker are
  not modelled.

### The console (NES, Famicom, PAL NES)

- **Chip (spec):** the NESdev Wiki's APU pages: two pulses with sweep, the
  triangle, the noise channel's two modes, the DMC's held level, the
  envelopes, length counters and the frame counter, in CPU cycles at the
  NTSC 1789773 Hz (19687500 / 11) or the PAL 1662607 Hz, with the PAL
  2A07's own noise and DMC period tables. A pulse period change that crosses the high
  bits writes $4003 or $4007, as a driver must, and the phase reset it
  forces is heard.
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
    (cartridge pin 45), the MMC5's DAC and its pulses each through 15 kOhm
    and 100 nF into the inverting input of the MMC5's amplifier, with
    10 kOhm of feedback, out to pin 46. Everything the console plays is
    high-passed at 106 Hz and scaled by -2/3, taking that input as a
    virtual ground as the drawing's internal reference implies.
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
  duties, the accumulating sawtooth, the halt and 16x/256x frequency
  bits); B the MMC5 (two pulses without sweep; its PCM channel is not
  played); C the Namco 163 (one to
  eight wavetable channels in 128 bytes of RAM, updated one every 15 CPU
  cycles through one DAC, so eight channels whine as the chip does); D the
  disk unit (a 64-step wave, its modulator with its 32-step table, the
  master volume and its own low-pass); E the 5B (the AY core at a 16-cycle
  tick with its 1.5 dB DAC steps). Their levels against the APU are what
  each chip's wiki page reports: the VRC6 at the APU pulse's level and
  inverted, the MMC5 equal and inverted, the 163 12 dB over the APU's
  square (the page gives 11 to 13), the disk unit at 2.4 times a square.
  The 5B's is Brad Smith's survey (nes-audio-tests, db_5b): its square at
  volume 12 measured 1.3 dB under the APU's at full volume, so its loudest
  channel is 7.7 dB over the APU's pulse.

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
- **Double** gives the voice a second set of machines that takes every
  register write of its first, with its note half the Detune up while the first plays half
  of it down. The first set plays on the left and the second on the right,
  each folded to mono at the level one side of a single set has: two
  machines side by side, as chip musicians play two handhelds live. Here
  the two sets share one clock and start together, so with no detune each
  side is the single set's sound, bit for bit; the detune is ours. Where it
  takes a note out of a channel's reach, the second set plays the first
  set's period. A change of Double resets the machines.
- **CPU** on the development container (Release, GCC 13.3), six held keys
  on each preset at 48 kHz, measured on 0.2.3: 0.16 % of a core on average
  and 0.9 % at worst (Disk Organ's four consoles) at 1x; 1.1 % and 2.4 % at
  8x; with Double, 0.28 % and 1.8 % at 1x, 1.5 % and 3.9 % at 8x. These
  are not Reason's figures.

### The playing layer (ours)

The layer that turns keys into register writes is a sound driver of our own,
and it only writes values the chips accept.

- **Pitch** is a period register, chosen as the one nearest in pitch (in
  ratio, not in count), so the chip's quantisation and out-of-tune top notes
  are its own. A key below a part's reach goes to its board's fallback: the
  handheld's wave channel below the pulses' 64 Hz, extended by more counts
  than its 11-bit register holds down to A0 (ours); the console's triangle
  below the pulses' 54.6 Hz.
- **Frames.** Everything that moves while a note sounds (the envelope's
  volume steps, vibrato, glide, the pitch wheel, the host's master tuning,
  the arpeggio) is written at the machines' frame, as a game's driver ran
  on its video frame: 59.73 Hz on the handheld, 60.10 or 50.01 Hz on the
  console. A note's own start and end land on its sample.
- **Envelopes** are times: Attack from silence to full, Decay from full
  down to Sustain, Release from where the key leaves the note down to
  silence. Each frame the envelope moves by the share of its stage that a
  frame is, and the chip's own volume register is written the whole step it
  has reached (the handheld's by the zombie write), so the volume still
  moves in the chip's steps, once a frame, as a driver's did; a time under
  a frame is at once. Level and velocity scale the same register. On the
  handheld's and the console's linear DACs a step is a fixed amplitude; on
  the 5B's logarithmic one it is a fixed number of decibels. The handheld's
  wave channel has four levels and the console's triangle none, so on them
  the envelope moves in those or only gates the note. Mono's legato notes
  keep their envelope; each Arp step starts it again.
- **Voices.** One voice plays up to eight machines of one board
  (**Boards**), with its revision, expansion, bias and path. A note takes
  the free channel of the part, on any of the machines, that was freed
  longest ago, else the oldest released note's, else the oldest note's.
  **Mode** is Poly, Mono (legato, with glide) or Arp (one voice stepping
  through the held keys every Arp rate frames).
- **Waves** for the wave channels are our own harmonic waves
  (`Tools/generate_waves.py`), scaled to each chip's range.
- **Level.** Every part and shape arrives at one loudness: that of one
  channel playing a 50 % square at full volume at -24 dBFS RMS, measured
  as ITU-R BS.1770's K-weighted mean square over a held C3, C4 and C5
  (`Source/DSP/LevelTrims.h`, by `Tools/RebootLevels.cpp`). The trim is a
  gain after the machine, a fader on the mixing desk: the chip, its DACs,
  its mixer and its output stage play as they do, and the steps of Level,
  velocity and the envelope keep their own sizes. A change of part or shape
  moves the trim across one block. Volume trims from there, and the
  portfolio's output stage keeps every sample under -1.5 dBFS.

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
| Arp rate | 1 to 8 frames | frames an arpeggio step holds |
| Vibrato, rate, delay | 0 to 100 cents, 0.5 to 12 Hz, 0 to 1000 ms | written at the frame; the mod wheel adds up to 50 cents |
| Glide | 0 to 1000 ms | a Mono legato note's slide |
| Bend range | 0 to 12 semitones | the pitch wheel |
| Velocity | 0 to 1 | how far a key's velocity scales the level |
| Double, Detune | Off, On; 0 to 50 cents | a second set of machines on the right, its notes Detune above the first set's on the left |
| Volume | -24 to +6 dB | our trim after the machines |
| Oversampling | 1x, 2x, 4x, 8x | setup, not a control: the machines' rate over the host's (`Engine::Prepare`) |

The control ids, ranges and defaults are `Source/DSP/RebootControls.h`, which
every front end reads; the factory presets are `Source/DSP/RebootPresets.h`.

## Known gaps

- **Analog stages without a source are not modelled.** The handheld's
  amplifier, headphone socket and speaker. What is published of them gives
  topologies, not a model: the original handheld's headphone socket couples
  through 100 uF, whose corner is the load's (about 50 Hz into 32 Ohm
  headphones, under 1 Hz into a line input); the amplifier's gain and
  clipping point are not published.
- **The handheld.** The DAC's fade when it turns off ("varies between
  models", Pan Docs), the zombie writes other than $08 and the wave
  channel's start-up delay are not modelled; the commonly used 6-cycle
  delay traces only to a forum post pointing at copyleft code. No published
  source gives the fade's decay times. The Color's corners are one unit's,
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
- **Ours, by design.** The playing layer, the handheld's wave counts past
  2048, the noise pitch rule (a key picks the noise's colour), the period
  nearest in pitch, the envelope's times and the Arp's restart of it, the
  frame's phase at reset, the output stages settled at reset, the 5 Hz Pin
  blocker, the level trims and their
  reference, Double's detune and oversampling's decimator are ours, not the
  hardware's.
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
