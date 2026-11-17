# Reboot

Reboot is a sound-chip synthesizer: the sound hardware of four 8-bit
machines, emulated from the published specifications of their chips and
played from a keyboard. The handheld board is the APU from the Game Boy and
Game Boy Color; the console board the 2A03 from the NES and Famicom (the
2A07 on the PAL console), with the Famicom's cartridge chips beside it:
Konami's VRC6, Nintendo's MMC5, Namco's 163, the Famicom Disk System's wave
unit and Sunsoft's 5B; the PSG board the SN76489 in Sega's Master System and
Game Gear and in Texas Instruments' computer; the micro board the
AY-3-8912 and the beeper from the ZX Spectrum 128K, and the 48K's beeper
alone. Every chip is an integer core that runs on its machine's own clock,
and a keyboard plays it through register writes, as a game's sound driver
did, so the chip's own pitch steps, volume steps and quirks are what is
heard. This repository holds the JUCE-free C++17 engine, its tools and its
tests. Reboot is being built as a Rack Extension for Reason, which plays this
engine; a plug-in front end is not built yet, and this repository has no
release.

## Audio demos

Eight of the sixteen factory presets, each playing a phrase written for it
(`Tools/write_preset_midi.py`, committed in `Docs/audio/midi/`) through the
shipping engine as the preset loads: a square lead on the handheld's pulse
with a delayed vibrato, an arpeggio on the Game Boy Color's pulse, two
detuned pulses gliding on the console, the VRC6's sawtooth, the disk unit's
modulated wave on four consoles, the PSG's periodic-noise bass under its own
tone, the AY's envelope buzzing as a bass, and a melody on the Spectrum's
one-bit beeper.

<!-- audio-demos-begin -->

| # | Preset | Demo | Length | Peak |
| --- | --- | --- | --- | --- |
| 1 | Square Lead | [01-square-lead.wav](Docs/audio/01-square-lead.wav) | 9.7 s | -11.4 dBFS |
| 3 | Pocket Arp | [03-pocket-arp.wav](Docs/audio/03-pocket-arp.wav) | 10.4 s | -19.1 dBFS |
| 6 | Pulse Glide | [06-pulse-glide.wav](Docs/audio/06-pulse-glide.wav) | 9.0 s | -10.1 dBFS |
| 7 | Saw Lead | [07-saw-lead.wav](Docs/audio/07-saw-lead.wav) | 8.4 s | -15.5 dBFS |
| 8 | Disk Organ | [08-disk-organ.wav](Docs/audio/08-disk-organ.wav) | 10.2 s | -4.8 dBFS |
| 10 | Periodic Bass | [10-periodic-bass.wav](Docs/audio/10-periodic-bass.wav) | 8.3 s | -19.1 dBFS |
| 13 | Buzz Bass | [13-buzz-bass.wav](Docs/audio/13-buzz-bass.wav) | 8.3 s | -14.9 dBFS |
| 14 | One-Bit Melody | [14-one-bit-melody.wav](Docs/audio/14-one-bit-melody.wav) | 8.4 s | -14.2 dBFS |

<!-- audio-demos-end -->

`build-dsp/RenderRebootPresetDemos Docs/audio/midi <out-dir>` renders them,
48 kHz 24-bit stereo, with the `demos.json` from which `python3
Tools/update_readme_blocks.py demos --readme README.md --manifest
<out-dir>/demos.json` writes the table. There is no main build yet, so the
WAVs are rendered and committed by hand after a change that alters them.

## Version history

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
(**Pin**), then Volume and the portfolio's output stage.

```
keys ─▶ playing layer ─▶ register writes at chip cycles ─▶ chip core (integer, event to event)
     ─▶ DAC codes ─▶ board DAC and mixer ─▶ steps ─▶ band-limited at the host rate
     ─▶ Pin: 5 Hz DC blocker │ Line: the machine's output stage ─▶ Volume ─▶ limiter at -1.5 dBFS
```

**Where every value comes from.** Each mechanism carries one of these tags
in the code beside it, and nothing is filled in with a plausible number: a
stage that no source describes is left out and listed under Known gaps.

| Tag | Meaning |
| --- | --- |
| spec | stated by the maker's documents or an authoritative technical reference (Pan Docs, the NESdev Wiki, SMS Power, the AY data manual, World of Spectrum's tech notes) |
| reported | a measurement or fit someone else published, cited beside the code; it ships labelled until our own measurement replaces it |
| ours | the playing layer and the conventions that let a keyboard play the chips; never claimed as hardware |

No die-level netlist was read, no emulator's code was copied, and no
copyleft or non-commercial source was read. Ayumi (MIT), MAME's ay8910 and
sn76496 (BSD-3-Clause) and the documents above were read for the rules they
state; the code is our own.

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

### The PSG (Sega Master System and Game Gear, TI computer)

- **Chip (spec):** SMS Power's SN76489 notes and MAME's sn76496 (for the
  per-variant widths and taps): three square tones and a noise generator on
  3579545 Hz, counters moving every 16th clock, a period of 0 counting as
  1 on Sega's parts and 1024 on TI's, Sega's 16-bit noise register tapped
  at bits 0 and 3 and TI's 15-bit one at 0 and 1. Revisions: the Sega
  console (mono), the Sega handheld (the same chip with its stereo
  register) and the TI computer.
- **DAC (reported, disputed for Sega):** 2 dB a step of attenuation, 15
  silent, the four channels summed. Below the tones' lowest note the board
  plays the chip's own bass trick: periodic noise clocked by tone 3 at its
  rate, tone 3 silent.
- **Line:** no line stage is known for any of the three, so Line renders as
  Pin. The Sega console is the Master System 1's: SMS Power reports its
  "proper 2dB step gapping", while the Master System 2 "clips due to a
  hardware design mistake" (an added pull-up, TmEE's scope photos, NESdev
  forum t=15562) at a level no source gives.

### The micro (ZX Spectrum 128K, 48K)

- **AY-3-8912 (spec, DAC reported):** the AY data manual's registers,
  mixer and envelope shapes, with Ayumi's counter rule and 17-bit noise
  register (MAME's ay8910 agrees), on half the 128K's 3546900 Hz. The
  envelope runs as the YM2149's 32 steps; the AY's DAC, Ayumi's table, gives
  each pair of codes one level, which is the AY's 16-step output.
- **Beeper (spec, levels reported):** the ULA's port $FE bits that drive
  pin 28, with World of Spectrum's measured pin voltages for each EAR and
  MIC state on an Issue 2 and an Issue 3. Notes run the ROM's BEEPER loop:
  the first OUT 209 - 12 (L mod 4) T-states after the call, then one every
  4 HL + 118, and a key-up lets the loop finish its cycle, as BEEPER does.
- **ULA contention (spec):** the ROM is not contended but port $FE is, so
  each OUT waits as World of Spectrum's 48K and 128K references give it
  (the N:1, C:3 pattern; 6, 5, 4, 3, 2, 1, 0, 0 T-states through the first
  128 of each screen line, from T-state 14335 of the 48K's frame and 14361
  of the 128K's). The loop runs slower and unevenly while the screen is
  drawn, 50 times a second, as on the machine.
- The loop's HL is ours: the value nearest in pitch on the loop's own
  timing, not the BEEP command's 437500 / f - 30.125, which assumes 3.5 MHz
  and plays 23 cents sharp on the 128K. Neither counts the contention,
  which leaves the top octave flat by up to 9 cents on the 48K and 16 on
  the 128K. Revisions: the 128K, the 48K Issue 3 and the 48K Issue 2.
- **Line:** no speaker or socket stage is modelled, so Line renders as Pin.
  The 128K's service manual gives one part of its TV sound: the modulator
  takes the ULA through 68 kOhm and 1 uF (R112, C123) and the AY through
  39 kOhm and 1 uF (R132, C127); the AY's output level and how its three
  pins are joined are not published, so the mix stays our convention
  (Known gaps).

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
- **Double** runs a second set of machines that takes every register write
  of the first, with its note half the Detune up while the first plays half
  of it down. The first set plays on the left and the second on the right,
  each folded to mono at the level one side of a single set has: two
  machines side by side, as chip musicians play two handhelds live. Here
  the two sets share one clock and start together, so with no detune each
  side is the single set's sound, bit for bit; the detune is ours. Where it
  takes a note out of a channel's reach, the second set plays the first
  set's period. A change of Double resets the machines.
- **CPU** on the development container (Release, GCC 13.3), six held keys
  on each preset at 48 kHz: 0.16 % of a core on average and 0.9 % at worst
  (Disk Organ's four consoles) at 1x; 1.1 % and 2.4 % at 8x; with Double,
  0.28 % and 1.8 % at 1x, 1.5 % and 3.9 % at 8x. These are not Reason's
  figures.

### The playing layer (ours)

The layer that turns keys into register writes is a sound driver of our own,
and it only writes values the chips accept.

- **Pitch** is a period register, chosen as the one nearest in pitch (in
  ratio, not in count), so the chip's quantisation and out-of-tune top notes
  are its own. A key below a part's reach goes to its board's fallback: the
  handheld's wave channel below the pulses' 64 Hz, extended by more counts
  than its 11-bit register holds down to A0 (ours); the console's triangle
  below the pulses' 54.6 Hz; the PSG's periodic-noise bass below its tones'
  109 Hz.
- **Frames.** Everything that moves while a note sounds (the envelope's
  volume steps, vibrato, glide, the pitch wheel, the host's master tuning,
  the arpeggio) is written at the machine's frame, as a game's driver ran on its video frame: 59.73 Hz
  on the handheld, 60.10 or 50.01 Hz on the console, 59.92 Hz on the PSG,
  50.02 or 50.08 Hz on the micro. A note's own start and end land on its
  sample.
- **Envelopes** step the chip's volume one step every Attack, Decay or
  Release frames, through the chip's own volume register (the handheld's
  by the zombie write); Level and velocity scale that register too. On the
  handheld's and the console's linear DACs a step is a fixed amplitude; on
  the PSG's, the AY's and the 5B's logarithmic ones it is a fixed number of
  decibels, so the same envelope falls faster in level there, as a driver's
  did. The micro's Buzz part plays the AY's own envelope.
- **Voices.** Up to eight machines of one board play at once (**Boards**);
  a note takes the free channel of its layer's part, on any of them, that
  was freed longest ago, else the oldest released note's, else the oldest
  note's. Two layers, A and B, each choose a
  part, shape, level, envelope, transpose and fine tune. **Mode** is Poly,
  Mono (legato, with glide) or Arp (one voice stepping through the held
  keys every Arp rate frames).
- **Waves** for the wave channels are our own harmonic waves
  (`Tools/generate_waves.py`), scaled to each chip's range.
- **Level.** One channel playing a 50 % square at full volume reads -24
  dBFS RMS on every board; Volume trims from there, and the portfolio's
  output stage keeps every sample under -1.5 dBFS.

### Controls

| Control | Range | What it does |
| --- | --- | --- |
| Board | Handheld, Console, PSG, Micro | the machine |
| Revision | per board (above) | the machine's model |
| Expansion | none, A to E | the Famicom's cartridge chip |
| Expansion channels | 1 to 8 | how many channels expansion C runs |
| Bias | 0 to 127 | the console's held DMC level |
| Path | Pin, Line | the chips' DACs and mixer, or the machine's output stage |
| Boards | 1 to 8 | machines played at once |
| Double, Detune | Off, On; 0 to 50 cents | a second set of machines on the right, its notes Detune above the first set's on the left |
| Mode | Poly, Mono, Arp | how keys take voices |
| Arp rate | 1 to 8 frames | frames an arpeggio step holds |
| Vibrato, rate, delay | 0 to 100 cents, 0.5 to 12 Hz, 0 to 1000 ms | written at the frame; the mod wheel adds up to 50 cents |
| Glide | 0 to 1000 ms | a Mono legato note's slide |
| Bend range | 0 to 12 semitones | the pitch wheel |
| Velocity | 0 to 1 | how far a key's velocity scales the level |
| Volume | -24 to +6 dB | our trim after the machines |
| A and B: On, Part, Shape | per board | the layer, the channels it plays, their duty, wave, noise mode or envelope shape |
| A and B: Level, Attack, Decay, Sustain, Release | 0 to 15, 0 to 30 frames a step | the envelope in the chip's volume steps |
| A and B: Transpose, Fine, Mod | ±24 semitones, ±50 cents, 0 to 63 | pitch, and the disk unit's modulation depth |
| Oversampling | 1x, 2x, 4x, 8x | setup, not a control: the machines' rate over the host's (`Engine::Prepare`) |

The control ids, ranges and defaults are `Source/DSP/RebootControls.h`, which
every front end reads; the factory presets are `Source/DSP/RebootPresets.h`.

## Known gaps

- **Analog stages without a source are not modelled.** The handheld's
  amplifier, headphone socket and speaker; any line stage of the PSG boards
  and the micro; the micro's speaker. Their Line renders as Pin. What is
  published of them gives topologies, not a model: the original handheld's
  and the Game Gear's headphone sockets couple through 100 uF, whose corner
  is the load's (about 50 Hz into 32 Ohm headphones, under 1 Hz into a line
  input); the
  Master System 1 buffers the VDP's sound pin with one transistor and a
  series capacitor whose values are not legible in the scanned manuals;
  the amplifiers' gains and clipping points are not published.
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
- **The PSG.** Two volume tables are published for Sega's consoles and they
  disagree; we use SMS Power's 2 dB steps. TmEE (NESdev forum t=15562)
  describes Sega's PSG as NMOS into a 2.2 kOhm sink, so channels summed
  high "flatten out" and lose their equal spacing, but gives no curve; the
  channels here sum linearly. The Master System 2's output clipping is not
  modelled. The Game Gear's sound board (837-7998, its EU maintenance
  manual) is a TDA2822M with 100 uF and 47 uF capacitors around it; the
  manual has board drawings and parts lists but no circuit diagram, and
  the TDA2822M's data sheet gives no internal resistor values, so its
  corners cannot be worked out.
- **The micro.** The AY's three outputs are tied through resistors and
  interact on the real board; here they sum independently. The AY's pins
  are unbuffered NMOS stages into a shared load, which would make them
  compress together; the grey +2's parts list has 1.8 kOhm resistors beside
  the AY (R49 to R51), but no 128K's values or measured mix is published. The 128K's
  beeper pin and its mix with the AY are unsourced: it takes the Issue 3's
  levels, and by our convention the beeper's swing equals one AY channel at
  full volume. Pin 28 falls slowly after EAR goes low (World of Spectrum:
  180 to 2800 T-states to cross 0.70 V, by how long it was high), which
  World of Spectrum puts down to the capacitors between pin 28 and the EAR
  and MIC sockets (R36 680 Ohm, C32 100 nF, R37 1 kOhm and C35 10 nF on
  the EAR side, from the 48K service manual's parts list). No published
  model of that network fits the timings, so the pin steps at once.
  Some machines run every ULA timing one T-state later than the
  references; ours are the references'.
- **Ours, by design.** The playing layer, the handheld's wave counts past
  2048, the noise pitch rule (a key picks the noise's colour), the period
  nearest in pitch, the beeper's HL, the frame's phase at reset, the 5 Hz
  Pin blocker, the per-board
  level reference, Double's detune and oversampling's decimator are ours,
  not the hardware's.
- **Presets are not level-matched.** The demos span about -35 to -17 dBFS
  whole-file RMS, from a periodic-noise bass and a Color arpeggio to a
  four-console disk organ. A preset Volume trim from a level tool is to come.
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
that every control reaches held and new notes. `Reboot.ParameterMatrix`
(`fuzz`) and `Reboot.DspFuzz` (Clang, `-DREBOOT_BUILD_FUZZER=ON`) push every
control to its extremes and the engine's input to garbage.

## Licensing

Reboot is source-available under the PolyForm Noncommercial License 1.0.0
with an additional permission for official builds; see [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Game Boy, NES, Famicom,
Master System, Game Gear, ZX Spectrum and the chip names are trademarks of
their owners, named here only to say which hardware is emulated; Reboot is
not affiliated with or endorsed by any of them.
