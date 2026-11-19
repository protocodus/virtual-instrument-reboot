#pragma once

// Reboot's engine: one voice playing its machines (up to eight of one
// board), the playing layer that writes their registers (ours, README How it
// works), the level trim that brings every part to one loudness, and the
// portfolio's output stage.
//
// The playing layer is a sound driver of our own. A note's writes land on
// the cycle its MIDI event maps to; everything that moves while a note
// sounds (the envelope's volume steps, vibrato, glide, the pitch wheel, the
// arpeggio) is written at the machine's frame, as a game's driver ran on
// its video frame. Pitch is always a period register, so the chip's own
// quantisation, out-of-tune top notes and stepped vibrato are what is heard.
// The envelope's times are in milliseconds and move the volume a fraction
// of a step each frame; the chip is written the whole step it reaches.
//
// Oversampling renders the machines at 2, 4 or 8 times the host rate and
// decimates them back (Render/Decimator.h); Double gives a second set of
// machines that mirrors every register write of the first, detuned, the
// first set on the left and the second on the right (ours, README How it
// works).
//
// Real time: Process, Midi, the note calls and SetControls neither allocate
// nor lock. Prepare is setup. A change of board, revision, expansion,
// expansion channels, machine count or Double resets the machines; then the
// held keys play again. A change of mode or Part plays the held keys again
// on the machines as they are.

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
    static constexpr int kSlots = kMaxBoards * Machine::kMaxChannels;
    // Ours: every part arrives at the loudness of one channel at full
    // volume, a 50 % square, at -24 dBFS RMS before Volume (LevelTrims.h),
    // so a four-channel chord at the factory levels sits near -9 VU on
    // Reason's meter.
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
    [[nodiscard]] int Boards() const noexcept { return boards_; }
    [[nodiscard]] const Machine& MachineAt(int board) const noexcept { return machines_[board]; }
    // The gain from the machines' output to the bus (LevelTrims.h).
    [[nodiscard]] double Trim() const noexcept { return trim_; }
    // Notes holding a channel, released ones included.
    [[nodiscard]] int Sounding() const noexcept;

private:
    // Scoped: the controls kAttack and kDecay share the names.
    enum class Stage { Attack, Decay, Sustained, Released };

    // A channel of a machine, and the note it plays.
    struct Slot {
        bool busy = false;
        bool gate = false;       // the key still holds it
        bool started = false;    // the channel has been started for this note
        int key = 0, velocity = 0;
        Voice voice = Voice::Pulse;
        Stage stage = Stage::Attack;
        double env = 0.0;        // the envelope, 0 to 15 volume steps
        double from = 0.0;       // where the release began
        double glide = 0.0;      // semitones still to go
        int glideLeft = 0;       // frames to go
        int frames = 0;          // frames since the note began
        double phase = 0.0;      // vibrato, in cycles
        std::uint64_t age = 0;   // when it began, or was freed
        ChannelSettings written;
        ChannelSettings twin;    // what the Double set's channel was last sent
    };

    struct Key {
        int key = 0, velocity = 0;
        bool down = false;       // false: held only by the pedal
    };

    // Slot s is on machine s / kMaxChannels, channel s % kMaxChannels.
    static int Channel(int slot) noexcept { return slot % Machine::kMaxChannels; }
    static int BoardOf(int slot) noexcept { return slot / Machine::kMaxChannels; }

    void Apply(bool reset, bool revoice);
    void Revoice(bool reset);
    void Strike(int key, int velocity);
    void Lift(int key);
    int StartNote(int key, int velocity);
    void Retarget(int slot, int key, int velocity, bool glide);
    void Restart(Slot& n) const noexcept;
    void ReleaseSlot(int slot);
    void End(int slot);
    void Settle(Slot& n) const noexcept;
    void StepEnvelope(Slot& n) const noexcept;
    [[nodiscard]] double Pitch(const Slot& n, double cents = 0.0) const;
    // Cents each set plays off the note: the first set down, the Double set up.
    [[nodiscard]] double Spread() const noexcept { return double_ ? detune_ / 2.0 : 0.0; }
    [[nodiscard]] double Gain(const Slot& n) const noexcept;
    bool Compose(int slot, ChannelSettings& s, double cents = 0.0) const;
    void Send(int slot, const ChannelSettings& s, bool start);
    void Write(int slot);
    void Tick();
    [[nodiscard]] int ArpKey(int index) const noexcept;
    [[nodiscard]] int HeldIndex(int key) const noexcept;

    Machine machines_[kMaxBoards];
    Machine twins_[kMaxBoards];
    MachineSettings settings_;
    Slot slots_[kSlots];
    Key held_[128];
    int heldCount_ = 0;
    int mono_ = -1;                     // Mono and Arp: the note

    Controls controls_;
    int boards_ = 2;
    Path path_ = Path::Line;
    int part_ = 0, shape_ = 0, level_ = 0, sustain_ = 0;
    int transpose_ = 0, mod_ = 0;
    double fine_ = 0.0;
    double attack_ = 0.0, decay_ = 0.0, release_ = 0.0;   // in frames
    double trim_ = 1.0;                 // LevelTrims.h, for the part and shape
    double trimHeard_ = 1.0;            // the trim the last block ended at
    double frameHz_ = 60.0, vibratoStep_ = 0.0;
    int vibratoDelay_ = 0, glideFrames_ = 0;
    int arpIndex_ = 0, arpCount_ = 0;
    std::int64_t tickNum_ = 0, periodNum_ = 1, periodDen_ = 1;

    int rate_ = 48000;
    int oversampling_ = 1;
    bool double_ = false;
    double detune_ = 0.0;
    int mode_ = 0, arpRate_ = 2, bendRange_ = 2;
    double vibratoDepth_ = 0.0, velocity_ = 0.5, gain_ = 0.0;
    double bend_ = 0.0, wheel_ = 0.0, tune_ = 0.0;
    bool pedal_ = false;
    std::uint64_t age_ = 0;

    Decimator decimators_[2];
    // The machines' left and right, then the Double set's, at the machines'
    // rate; the bus's left and right are 0 and 1 after the fold.
    float bus_[4][StepBuffer::kMaxFrames] {};
    OutputStage<2> stage_;
};

} // namespace protocodus::reboot
