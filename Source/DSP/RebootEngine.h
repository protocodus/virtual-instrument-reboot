#pragma once

// Reboot's engine: two layers, each playing its own machines (up to eight of
// one board), the playing layer that writes their registers (ours, README
// How it works) and the portfolio's output stage.
//
// The playing layer is a sound driver of our own. A note's writes land on
// the cycle its MIDI event maps to; everything that moves while a note
// sounds (the envelope's volume steps, vibrato, glide, the pitch wheel, the
// arpeggio) is written at the machine's frame, as a game's driver ran on
// its video frame. Each layer moves on its own machines' frame. Pitch is
// always a period register, so the chip's own quantisation, out-of-tune top
// notes and stepped vibrato are what is heard.
//
// Oversampling renders the machines at 2, 4 or 8 times the host rate and
// decimates them back (Render/Decimator.h); Double gives each layer a second
// set of machines that mirrors every register write of the first, detuned,
// the first sets on the left and the second on the right (ours, README How
// it works).
//
// Real time: Process, Midi, the note calls and SetControls neither allocate
// nor lock. Prepare is setup. A change of a layer's board, revision,
// expansion, expansion channels, machine count or On resets that layer's
// machines, and a change of Double resets both; then the held keys play
// again. A change of mode or a layer's Part plays the held keys again on the
// machines as they are. A layer that is off runs no machines.

#include "Driver/Machine.h"
#include "OutputStage.h"
#include "Render/Decimator.h"
#include "RebootControls.h"

#include <array>
#include <cstdint>

namespace protocodus::reboot {

class Engine {
public:
    static constexpr int kMaxBoards = 8;
    static constexpr int kGroupSlots = kMaxBoards * Machine::kMaxChannels;   // a layer's channels
    static constexpr int kSlots = kLayers * kGroupSlots;
    // Ours: one channel at full volume, a 50 % square, reads -24 dBFS RMS
    // on every board before Volume, so a four-channel chord at the
    // factory levels sits near -9 VU on Reason's meter.
    static constexpr double kReferenceRms = 0.063095734448019;

    // The highest rate the machines run at: oversampling is halved until
    // they run at or under it (Prepare). Past it, the steps' band limit and
    // the boards' analog stages are already far above hearing, and only the
    // CPU would grow.
    static constexpr int kMaxInternalRate = 384000;

    // Setup, not real time. The rate is taken to the nearest whole hertz;
    // `oversampling` is 1, 2, 4 or 8 (another value, the largest of those
    // under it, and at least 1).
    void Prepare(double sampleRate, int oversampling = 1);

    void SetControls(const Controls& controls);   // any values: each is taken into its range
    [[nodiscard]] const Controls& GetControls() const noexcept { return controls_; }

    // A channel message (any MIDI channel), applied at the start of the next
    // Process call.
    void Midi(const std::uint8_t* bytes, int size);
    void NoteOn(int key, int velocity);
    void NoteOff(int key);
    void SetSustain(bool down);
    void SetBend(double amount);   // -1 .. 1
    void SetWheel(double amount);  // 0 .. 1
    // The host's master tuning, -100 .. 100 cents, on every note, held ones
    // too; no MIDI message carries it.
    void SetTune(double cents);
    void ReleaseAll();             // every key up, the pedal up
    void Panic();                  // every channel silent at once
    // A host's audio reset: every key and the pedal up, and every machine,
    // filter and the output stage back at rest, as Prepare leaves them.
    // Real time; the controls, wheels and tuning stay as they are.
    void Reset();

    // Writes `frames` stereo samples.
    void Process(float* left, float* right, int frames);

    // The step buffer's kernel centre at the machines' rate, the
    // decimator's stages, then the output stage's look-ahead: whole host
    // samples at every factor (Tools/generate_halfband.py).
    [[nodiscard]] int LatencySamples() const noexcept {
        return blep::kDelay / oversampling_ + Decimator::DelayFor(oversampling_) + stage_.DelaySamples();
    }
    // The factor the machines run at, after kMaxInternalRate.
    [[nodiscard]] int Oversampling() const noexcept { return oversampling_; }
    [[nodiscard]] int Boards(int layer) const noexcept { return groups_[layer].boards; }
    [[nodiscard]] const Machine& MachineAt(int layer, int board) const noexcept {
        return groups_[layer].machines[board];
    }
    // Notes holding a channel, released ones included.
    [[nodiscard]] int Sounding() const noexcept;

private:
    // Scoped: the layer controls kAttack and kDecay share the names.
    enum class Stage { Attack, Decay, Sustained, Released };

    struct Layer {
        bool on = false;
        int part = 0, shape = 0, level = 0;
        int attack = 0, decay = 0, sustain = 0, release = 0;
        int transpose = 0, mod = 0;
        double fine = 0.0;
    };

    // A channel of a machine, and the note it plays.
    struct Slot {
        bool busy = false;
        bool gate = false;       // the key still holds it
        bool started = false;    // the channel has been started for this note
        int key = 0, velocity = 0, layer = 0;
        Voice voice = Voice::Pulse;
        Stage stage = Stage::Attack;
        int step = 0, count = 0;
        double glide = 0.0;      // semitones still to go
        int glideLeft = 0;       // frames to go
        int frames = 0;          // frames since the note began
        double phase = 0.0;      // vibrato, in cycles
        std::uint64_t age = 0;   // when it began, or was freed
        int claim = -1;          // the slot it takes over as well, or -1
        int claimedBy = -1;      // the slot taking it over, or -1
        ChannelSettings written;
        ChannelSettings twin;    // what the Double set's channel was last sent
    };

    struct Key {
        int key = 0, velocity = 0;
        bool down = false;       // false: held only by the pedal
    };

    // A layer's machines, the Double set's, and the frame they keep.
    struct Group {
        Machine machines[kMaxBoards];
        Machine twins[kMaxBoards];
        MachineSettings settings;
        int boards = 2;
        Path path = Path::Line;
        double swing = 1.0;      // the output scale that brings the board to kReferenceRms
        double frameHz = 60.0, vibratoStep = 0.0;
        int vibratoDelay = 0, glideFrames = 0;
        int arpIndex = 0, arpCount = 0;
        std::int64_t tickNum = 0, periodNum = 1, periodDen = 1;
    };

    // Slot s is layer s / kGroupSlots's, on its machine (s % kGroupSlots) /
    // kMaxChannels, channel s % kMaxChannels.
    static int Channel(int slot) noexcept { return slot % Machine::kMaxChannels; }
    static int LayerOf(int slot) noexcept { return slot / kGroupSlots; }
    static int BoardOf(int slot) noexcept { return slot % kGroupSlots / Machine::kMaxChannels; }
    Machine& MachineOf(int slot) noexcept { return groups_[LayerOf(slot)].machines[BoardOf(slot)]; }
    const Machine& MachineOf(int slot) const noexcept { return groups_[LayerOf(slot)].machines[BoardOf(slot)]; }
    Machine& TwinOf(int slot) noexcept { return groups_[LayerOf(slot)].twins[BoardOf(slot)]; }

    // `resets` holds a bit for each layer whose machines start again.
    static constexpr int kAllLayers = (1 << kLayers) - 1;
    void Apply(int resets, bool revoice);
    void Revoice(int resets);
    void Strike(int key, int velocity);
    void Lift(int key);
    int StartNote(int layer, int key, int velocity, double glide);
    void Retarget(int slot, int key, int velocity, bool glide);
    void ReleaseSlot(int slot);
    void End(int slot);
    void Settle(Slot& n) const noexcept;
    void StepEnvelope(Slot& n) const noexcept;
    [[nodiscard]] double Pitch(const Slot& n, double cents = 0.0) const;
    // Cents each set plays off the note: the first set down, the Double set up.
    [[nodiscard]] double Spread() const noexcept { return double_ ? detune_ / 2.0 : 0.0; }
    [[nodiscard]] int Volume(const Slot& n) const noexcept;
    bool Compose(int slot, ChannelSettings& s, double cents = 0.0) const;
    void Send(int slot, const ChannelSettings& s, bool start);
    void Write(int slot);
    void Tick(int layer);
    [[nodiscard]] int ArpKey(int index) const noexcept;
    [[nodiscard]] int HeldIndex(int key) const noexcept;

    Group groups_[kLayers];
    Slot slots_[kSlots];
    Key held_[128];
    int heldCount_ = 0;
    int mono_[kLayers] = { -1, -1 };    // Mono and Arp: each layer's note

    Controls controls_;
    Layer layers_[kLayers];
    int rate_ = 48000;
    int oversampling_ = 1;
    bool double_ = false;
    double detune_ = 0.0;
    int mode_ = 0, arpRate_ = 2, bendRange_ = 2;
    double vibratoDepth_ = 0.0, velocity_ = 0.5, gain_ = 0.0;
    double bend_ = 0.0, wheel_ = 0.0, tune_ = 0.0;
    bool sustain_ = false;
    std::uint64_t age_ = 0;

    Decimator decimators_[2];
    // The layers' sum, left and right, at the machines' rate.
    float bus_[2][StepBuffer::kMaxFrames] {};
    // One layer's machines' left and right, then its Double set's.
    float layerBus_[4][StepBuffer::kMaxFrames] {};
    OutputStage<2> stage_;
};

} // namespace protocodus::reboot
