# Working conventions

## Architecture

This is Reboot's canonical repository: a sound-chip synthesizer whose engine
lives in `Source/DSP/`: the chip cores (`Chips/`), the boards that put them
in their machines (`Boards/`), the band-limited renderer (`Render/`), the
machine interface and waves (`Driver/`), and the playing layer and controls
(`RebootEngine.*`, `RebootControls.h`, `RebootPresets.h`). There is no JUCE
plug-in, editor or release yet: Reboot is a Rack-first instrument (standard
P4), so `Source/` holds only `DSP/`, ready for a plug-in to be added beside
it. Its Reason Rack Extension is built in the private
`protocodus/reason-rack-extensions` repository as a thin adapter that mounts
this repository as the pinned submodule `Examples/Reboot/Instrument` and
compiles `Source/DSP/RebootEngine.cpp` directly. DSP and playing-model
changes land here first; the Rack pin then advances. Reason property
mapping, MIDI/CV, routing, host latency, patches and panels stay in the
adapter.

Repository layout, naming, CI, versions and the `Docs/` taxonomy follow the
Protocodus portfolio standard kept in the project, not in this repository;
`python3 Tools/check_repo_standard.py <checkout>` in the Rack repository
reports how a checkout stands against it.

- The DSP version is the product version: `project(Reboot VERSION X.Y.Z)` in
  `CMakeLists.txt`. Any DSP or behaviour change bumps it, and versions only go
  up; a Rack device takes the same `X.Y.Z` plus its own stage suffix.
- `Source/DSP/` is JUCE-free and C++17 (the Rack toolchain); the
  `RebootDSPCxx17` target proves it with the Rack's `-fno-builtin` and
  `-fno-exceptions`. No allocation, file I/O, locks or exceptions after
  `Prepare`, no libm call on the per-sample path, and every namespace-scope
  constant `constexpr` (`Reboot.NoLoadTimeGlobals` asks Clang).
- Front ends share `Source/DSP/RebootControls.h` (the controls, their ids,
  ranges and normalised mapping) and `Source/DSP/RebootPresets.h` (the
  factory bank). Never rename or reuse a control id, and append new controls
  at the end: saved sessions and Rack patches recall by them. When a plug-in
  is added, its `BUNDLE_ID` and CLAP id are `cz.protocodus.reboot`, and they
  and its plug-in codes never change once a build reaches users.
- This repository is public. Never add Reason/Jukebox SDK material, secrets,
  ROM images, game music data, recordings or unreleased commercial material.

## Documentation layout

`README.md` is the single product document. It opens with the heading and a
one-paragraph intro, the editor screenshot, **Audio demos**, a concise
**Version history** ledger and **Downloads**; after a `---` separator come
**How it works**, **Known gaps**, **Build** and **Licensing**. Long per-version
narratives go to `Docs/release/` and are linked.

Everything else in `Docs/` has one home, and nothing sits loose there except
`decisions.md`, the by-ear decision log described below:

| Folder | Holds |
| --- | --- |
| `audio/` | rendered demos, per-fix previews, committed listening tests |
| `screenshots/` | editor images |
| `guide/` | user, install and preset guides, parameter meaning |
| `research/` | modelling research, references, design notes, experiments |
| `validation/` | audits, test results, host validation |
| `benchmarks/` | CPU and performance measurements |
| `plans/` | improvement plans, requirements, open work |
| `release/` | release checklists, readiness, release notes, asset provenance |

Name documents in lowercase kebab-case, `<topic>-YYYY-MM-DD.md` for dated
work. Moving a document is a rename only: keep its content and date, and update
every link to it in the same commit. Legal files (`LICENSE`,
`THIRD_PARTY_NOTICES.md`, `PRIVACY.md`) stay at the root; upstream licence
texts live in `ThirdParty/`.

Research the code depends on — a specific value, a manual page, a measurement,
a derivation — belongs in a comment **beside the code that uses it**, not only
in a document that drifts away from it. A measurement protocol belongs at the
head of the tool that enforces it.

These README parts are written by the build, never by hand:

- The demo table between `<!-- audio-demos-begin -->` and
  `<!-- audio-demos-end -->`, rendered from one
  `Docs/audio/midi/<NN>-<preset-slug>.mid` per curated preset demo (at most 8,
  each preset showing a different side of the instrument, or every preset of a
  bank of 8 or fewer; change a source only on purpose, in its own commit) and
  committed by the main build as `Docs/audio/<NN>-<preset-slug>.wav`, 8–12 s
  of 48 kHz 24-bit audio.
- The block between `<!-- build-download-begin -->` and
  `<!-- build-download-end -->`, rewritten by the main build, which also
  commits the README screenshot under `Docs/screenshots/`.

Version history is a ledger of what changed about the product — what it
sounds like, what it exposes, what a session must know. Pure refactors,
deduplications and test-coverage additions belong in git history, not in it.

Per-fix audio previews are **one-time review evidence**: render them while the
change is under review, then leave them frozen. Do not re-render them when
later engine changes make them stale.

## A–Z listening tests

This is a physical model with no owned calibration captures, so there are
decisions the measurements cannot close. When a change is audible and the
physics admits more than one defensible option, **render the options as WAVs,
letter them, and let the user decide the direction by ear.** A listening test is
a legitimate way to choose between candidates. It is not a way to fit a number —
see the limit below.

**When to run one.** Whenever any of these is true:

- two or more formulations are each derivable and the model cannot say which
  describes the instrument (which radiation law a body obeys, which of two
  reference geometries a term should be measured against);
- a change is audible and the open question is whether it is an improvement
  rather than whether it is correct;
- a calibrated constant has to be re-pinned and more than one pinning is
  defensible;
- the user asks for one.

Do not run one to decide something a measurement already decides. If a number
settles it, quote the number.

**How to build one.**

- **A is always the shipping engine**, unchanged. B, C, … Z are the candidates.
  So "A against B" is always "what it does now against what it would do".
- **Letters only in the filenames.** `A.wav`, `B.wav`, … carry no hint of the
  mechanism, so the listener is not primed by the label. Write the key down at
  the same time, in a separate `key.md` the user can read after deciding, and
  say in the handover that the key exists and is unread-by-design.
- **Change one thing.** Identical MIDI, seed, sample rate, block size, length
  and controls across every letter; only the mechanism under test differs.
- **Level-match, and say how.** Match on whole-file RMS unless level is itself
  what is being judged, and state the measure and the trims applied. Louder
  reads as better, and an unmatched set tests loudness.
- **Give the same material to every letter**, and pick material that exposes the
  thing under test — a stroke that drives the mechanism hard, not a general
  demo. Keep it short enough to A/B repeatedly.
- Render through the shipping signal path, never through an offline
  approximation of it.

**Where they live.** Listening tests are decision artefacts, not review
evidence. Render them to a working directory and send them to the user; do not
commit them by default — a 26-file set per decision would bloat the repository.
Commit a set only when the user asks, under
`Docs/audio/listening-tests/<date>-<topic>/` with its `key.md`, and freeze it
on merge exactly like the per-fix previews above.

**What gets committed is the verdict.** Record in `Docs/decisions.md` what was
being decided, what each letter was, which letter the user chose, and what that
choice licensed. A direction chosen by ear is recorded as chosen by ear — never
written up as though a measurement had settled it.

**The limit.** A listening test chooses between candidates that are each already
defensible on their own physics. It does not license drawing a constant, fitting
a curve, or keeping a formulation the measurements reject. If every candidate
needs an invented number, the answer is still that none of them ships.

## Demo songs

The README demos play the committed MIDI in `Docs/audio/midi/` (More demos in
`more/`), written only by `Tools/write_preset_midi.py`; `<Name>.DemoMidiSources`
fails when the files and the script differ. Change a phrase in the script, on
purpose, and commit it with its regenerated `.mid` files on their own. Never
edit a `.mid` by hand or rewrite a phrase because the DSP changed. The demos
play each preset as a freshly loaded plug-in does, with no per-demo trims.
The WAVs and the README table come only from the main build. Use the
`portfolio-demo-songs` skill when choosing presets, writing phrases or changing
the renderer.

## Promotion and Shop material

Promo videos, Shorts, their music, stems and thumbnails are review
deliverables: render them to a dated folder outside the checkout, hand them
over, and never commit them. Their sound comes from the shipping engine
playing original MIDI, and every claim on screen or in the copy is one How it
works supports. Use the `portfolio-youtube-promo` skill for a 16:9 YouTube
promo and `portfolio-shorts` for YouTube Shorts and TikTok. Reason Shop copy
and images belong to the Rack repository's `rack-shop-content` skill, and
panel artwork to its `rack-panel-design` skill. Uploading or publishing any of
it is the owner's step.

## Testing

Tests follow the portfolio testing standard (U1–U13); the `portfolio-testing`
skill has the procedures. These hold in every session:

- Every CTest test is `<Name>.<Suite>` with exactly one tier label: `unit`,
  `audio`, `render`, `fuzz` or `perf`.
- A test is never skipped, disabled, loosened or given a longer timeout to
  get green. Diagnose a failure against the baseline first; when a fixture or
  limit was wrong, fix it and say why beside it, in the same commit.
- Checks run in Release builds: use the file's always-on helper, never a bare
  `assert()`. Seeds, rates and block sizes are fixed.
- No test outside the `perf` tier passes or fails on time, and `perf` never
  runs in a Debug or sanitized build. CPU figures are development-machine
  figures, never Reason or DAW CPU.
- A fixed fuzz crash's input goes in `Tests/Fixtures/fuzz/`, which
  `<Name>.DspFuzzReplay` replays in the `unit` tier.
- No parameter goes stale: `<Name>.ParameterReach` (U11) proves every change,
  from the editor or the host, reaches held and new notes after long play.
  Declare a parameter `NewNotes` or `Exempt` only where the design says so,
  with the reason beside it; a reach failure is a bug, not an exemption.
- At the default settings, which start at the lowest oversampling (U13), no
  block of a demo or the stress phrase should take more than 10% of one core
  on the CI runner (U12). `<Name>.CpuBudget` warns above that and never fails
  on it (owner, 2026-11-10); a product above it keeps a Known gap.
- A DSP change is done when the suites it touches, the `unit`, `audio` and
  `perf` tiers and `<Name>DSPCxx17` pass.

## Build and verify

```sh
cmake -S . -B build-dsp -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DREBOOT_REQUIRE_PYTHON_TESTS=ON
cmake --build build-dsp --parallel 2
ctest --test-dir build-dsp -L unit --output-on-failure
```

Every test is `Reboot.<Suite>`: `Chips` (each core against its documents'
formulas, partition invariance and exact catch-up after silence), `Boards`
(pitch, range and level of every board and revision), `Engine` (the playing
layer), `ParameterReach` (every control changed while a chord is held and
twice more by automation after long play, `RebootParameterMatrixTests
--reach`; `--case <control id>` runs one; the rules and their reasons are
`ReachFor` in `Tests/ParameterMatrixTests.cpp`), `NoLoadTimeGlobals`,
`DspFuzzReplay`, `BlepKernel`, `Waves` and `DemoMidiSources` (`unit`),
`AudioDemos` (`audio`, every demo rendered twice), `ParameterMatrix`
(`fuzz`, one rotation; without `--rotation` every rate and block size) and,
with `-DREBOOT_BUILD_FUZZER=ON` and Clang, `DspFuzz` (`fuzz`), and
`CpuBudget` (`perf`, Release without sanitizers only).
`-DREBOOT_ENABLE_SANITIZERS=ON` builds everything under ASan + UBSan. Select
the suites that cover a change with `-R`, then run the `unit`, `audio` and
`perf` tiers and build `RebootDSPCxx17` before a substantive DSP change is
called done.

There is no main build yet: after a change that alters the demos, render
them (`build-dsp/RenderRebootPresetDemos Docs/audio/midi <out-dir>`), copy
the WAVs to `Docs/audio/`, rewrite the README table with
`Tools/update_readme_blocks.py demos` and commit them.

## Reboot specifics

- **Evidence, and nothing invented.** Every mechanism carries a tag beside
  its code: **spec** (the maker's documents or an authoritative technical
  reference), **reported** (someone else's published measurement or fit,
  cited) or **ours** (the playing layer and the conventions a keyboard
  needs), and two more for what fills a gap until better material comes
  (owner, 2026-11-17: "use your judgement or similar / siblings units to
  fill the gaps before we have better materials"): **sibling** (a value a
  closely related unit or board documents, borrowed with the reason it
  transfers) and **estimate** (a typical part or an engineering judgement,
  with its reasoning beside it). Prefer spec, then reported, then sibling,
  then estimate. A gap nothing defensible can fill, such as a clipping
  level or a curve no one has drawn, stays out and stays listed under
  README, Known gaps; every sibling and estimate is listed there too, so it
  is replaced when a source arrives. A listening test may choose between
  candidates, never invent a number.
- **Clean room.** Read the documents, and permissively licensed code (MIT,
  BSD, ISC, CC0) for the rules it states; copy no one's code. Never read
  GPL, LGPL or non-commercial emulator source. A ROM disassembly read for a
  timing fact is acceptable, and says so beside the value.
- **Names.** Real chip, console and computer names appear only in this
  repository's README, documents and code comments, where provenance needs
  them (owner, 2026-11-17). The Rack device's names, panel, patches and Shop
  copy are generic: the Handheld, the Console, the PSG and the Micro, and
  expansions A to E; never another maker's marque, model number or trademark.
- **The playing layer only writes what the chip accepts**, at times the
  chip allows, and never adds an oscillator, filter or envelope the board
  does not have. Pitch is always a period register.
- **Cores are integer and event-driven.** A core runs from event to event,
  gives the same output however its run is cut, and catches a silent
  channel up exactly (`Reboot.Chips` holds all three). A new mechanism keeps
  all three and gets its check there.
- `Source/DSP/Render/BlepKernel.h`, `Source/DSP/Render/HalfbandKernel.h`
  and `Source/DSP/Driver/Waves.h` are generated by `Tools/generate_blep.py`,
  `Tools/generate_halfband.py` and `Tools/generate_waves.py` and checked by
  `Reboot.BlepKernel`, `Reboot.HalfbandKernel` and `Reboot.Waves`; change
  them only through their generators. `Source/DSP/OutputStage.h`,
  `Tools/PresetDemoRenderer.h`, `Tools/update_readme_blocks.py`,
  `Tests/ParameterMatrix.h`, `Tests/FuzzHarness.h` and
  `scripts/validate-plugins.sh`, and the Documentation layout, A–Z listening
  tests, Demo songs, Promotion and Shop material and Testing sections above,
  are byte-identical copies of the portfolio masters in
  `reason-rack-extensions/Tools/portfolio/`. Change them there, never here.
- **Out of scope for now:** the FM chips (YM2612/YM3438), the SID, OPM and
  OPLL. The FM core joins only if public evidence carries it to the same
  standard as the rest.
