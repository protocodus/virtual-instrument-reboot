#pragma once

// Reboot's engine: one voice playing its units (up to eight of one board),
// the playing layer that writes their registers (ours, README How it
// works), the level trim that brings every part to one loudness, and the
// portfolio's output stage.
//
// The playing layer is a sound driver of our own, and a player above it. A
// note's writes land on the cycle its MIDI event maps to; everything that
// moves while a note sounds (the envelope's volume steps, vibrato, glide,
// the pitch wheel) is written at the unit's own frame, as a game's driver
// ran on its video frame. Every unit is a machine with its own crystal,
// power-on moment and parts (Boards/Board.h, UnitTraits), and its driver
// counts its own frames from its own power-on. The player decides which
// unit's channel plays which key; its arpeggio steps on the first unit's
// frame, and each step goes out to every unit at that moment, as a link
// cable carries a master's ticks. With Sync the arpeggio's steps and the
// vibrato's cycle are note values at the song's tempo, still on the
// driver's ticks: each step on the tick nearest its time, so none is off by
// more than half a tick and the steps keep the tempo. Pitch is always a
// period register, so the chip's own quantisation, out-of-tune top notes
// and stepped vibrato are what is heard. The envelope's times are in milliseconds and move the
// volume a fraction of a step each frame; the chip is written the whole
// step it reaches.
//
// Oversampling renders the units at 2, 4 or 8 times the host rate and
// decimates them back (Render/Decimator.h). Double is a second set of
// units of the same board, set up the same and given the same commands,
// the first set on the left and the second on the right (owner,
// 2026-11-18: "double is just having two devices of the same kind with the
// same setup and wiring them to separate channels"). What parts the sides
// is what parts two units: their crystals, their power-on moments and their
// parts' values, and Detune, a pull on the two sets' crystals half of it
// either way.
//
// Real time: Process, Midi, the note calls and SetControls neither allocate
// nor lock. Prepare is setup. A change of board, revision, expansion,
// expansion channels, machine count or Double resets the units; then the
// held keys play again. A change of mode or Part plays the held keys again
// on the units as they are.

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
    // The song's tempo in beats (quarter notes) a minute, taken into 1 to
    // 999; 120 until a front end sets it. Real time.
    void SetTempo(double bpm);
    // While the song plays, its position in beats at the start of the next
    // Process call; negative or NaN while it is stopped. With Sync, a key
    // that starts an arpeggio then takes the line of its note value's grid
    // nearest it as its step's start, so the steps fall on the song's grid
    // however early or late the key. Real time.
    void SetSongPosition(double beats);
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
    // What channel `channel` of the first set's machine `board` holds.
    struct Holding {
        bool busy;
        int key;
        Voice voice;
    };
    [[nodiscard]] Holding HoldingAt(int board, int channel) const noexcept {
        const Slot& s = slots_[board * Machine::kMaxChannels + channel];
        return { s.busy, s.key, s.voice };
    }

private:
    // Scoped: the controls kAttack and kDecay share the names.
    enum class Stage { Attack, Decay, Sustained, Released };

    // What the driver on one set's unit holds for a note: it moves on that
    // unit's own frames.
    struct Program {
        Stage stage = Stage::Attack;
        double env = 0.0;        // the envelope, 0 to 15 volume steps
        double from = 0.0;       // where the release began
        double glide = 0.0;      // semitones still to go
        int glideLeft = 0;       // frames to go
        int frames = 0;          // frames since the note began
        double phase = 0.0;      // vibrato, in cycles
        bool ended = false;      // its release has run out and its channel is stopped
        bool full = false;       // the envelope is the channel's whole volume, not a share of the level and touch
        ChannelSettings written; // what the channel was last sent
        // Envelope Chip: the channel's own envelope unit runs the note, and
        // the driver keeps count of it from the trigger it gave (BeginChip).
        bool chip = false;
        bool restart = false;    // the next write triggers the channel and its envelope again
        bool chipUp = false, chipLoop = false;
        bool chipFull = false;   // the unit's volume is not the level's (it starts from its top or climbs to it)
        int chipTop = 15;        // the unit's top volume (EnvelopeUnit::top)
        int chipFrom = 0;        // the volume the unit started from
        int chipPace = 0;        // the unit's pace code
        double chipStep = 0.0;   // the board's cycles a step takes at that pace
        std::int64_t chipAt = 0; // the unit's cycle at the trigger
    };

    // A channel of a machine, the same channel of its Double unit, and the
    // note they play.
    struct Slot {
        bool busy = false;
        bool gate = false;       // the key still holds it
        bool fallback = false;   // borrowing another channel because the selected part cannot reach the pitch
        bool started = false;    // the channel has been started for this note
        int key = 0, velocity = 0;
        Voice voice = Voice::Pulse;
        std::uint64_t age = 0;   // when it began, or was freed
        Program set[2];          // the first set's unit, then the Double set's
    };

    struct Key {
        int key = 0, velocity = 0;
        bool down = false;       // false: held only by the pedal
        bool waitingForPitch = false;  // Poly note-on fell outside both register ranges
        std::uint64_t struck = 0;      // when it was struck: its place in Arp order's "as played"
    };

    // Slot s is on machine s / kMaxChannels, channel s % kMaxChannels; with
    // Double, the same channel of machine kMaxBoards + s / kMaxChannels too.
    // A machine's place in machines_ is its unit's serial number.
    static int Channel(int slot) noexcept { return slot % Machine::kMaxChannels; }
    static int BoardOf(int slot) noexcept { return slot / Machine::kMaxChannels; }
    static int UnitOf(int slot, int set) noexcept { return set * kMaxBoards + BoardOf(slot); }
    [[nodiscard]] int Sets() const noexcept { return double_ ? 2 : 1; }
    [[nodiscard]] bool Active(int unit) const noexcept { return unit % kMaxBoards < boards_ && unit / kMaxBoards < Sets(); }

    void Apply(bool forceReset);
    void Times();
    void Revoice(bool reset);
    void Strike(Key& note);
    void RetryHeldPitches();
    void Lift(int key);
    void ReleaseKey(int key);
    void FollowHeld();
    int StartNote(int key, int velocity, bool maySteal = true);
    void Retarget(int key, int velocity, bool glide);
    void Restart(Slot& n) const noexcept;
    void ReleaseSlot(int slot);
    void EndSet(int slot, int set);
    void End(int slot);
    void Settle(Program& p) const noexcept;
    void BeginChip(int slot, int set);
    [[nodiscard]] int ChipVolume(const Program& p, int unit) const noexcept;
    void ToDriver(int slot, int set);
    void FreeUnit(int slot, int set);
    void StepEnvelope(Program& p) const noexcept;
    [[nodiscard]] double Pitch(const Slot& n, int set) const;
    [[nodiscard]] double Gain(const Slot& n, int set) const noexcept;
    bool Compose(int slot, int set, ChannelSettings& s) const;
    void Send(int slot, int set, const ChannelSettings& s, bool start);
    bool HandOff(int slot);
    void WriteSet(int slot, int set);
    void Write(int slot);
    // Each set's crystal pull, in Timebase's units: half the detune either way.
    [[nodiscard]] std::int64_t Pull(int set) const noexcept;
    void Pace(int unit) noexcept;
    [[nodiscard]] std::int64_t NextTick(int unit) const noexcept {
        return (tickNum_[unit] + periodDen_ - 1) / periodDen_;
    }
    void Tick(int unit, std::int64_t at);
    std::int64_t FirstStep() const noexcept;
    bool ArpDue(std::int64_t at) noexcept;
    [[nodiscard]] std::int64_t ArpRank(const Key& k) const noexcept;
    [[nodiscard]] const Key* ArpSeek(std::int64_t after, int sign) const noexcept;
    const Key& NextArpKey(std::int64_t after) noexcept;
    void ArpAnchor() noexcept;
    [[nodiscard]] int HeldIndex(int key) const noexcept;

    // The first set's units, then from kMaxBoards the Double set's.
    Machine machines_[2 * kMaxBoards];
    MachineSettings settings_;
    Slot slots_[kSlots];
    Key held_[128];
    int heldCount_ = 0;
    int mono_ = -1;                     // Mono and Arp: the note

    Controls controls_;
    int boards_ = 6;
    Path path_ = Path::Line;
    int part_ = 0, shape_ = 0, level_ = 0, sustain_ = 0;
    int transpose_ = 0, mod_ = 0;
    double fine_ = 0.0;
    double attack_ = 0.0, decay_ = 0.0, release_ = 0.0;   // in frames
    double trim_ = 1.0;                 // LevelTrims.h, for the part and shape
    // One native Rack quantum at the host rate. Count internal samples so
    // automation remains continuous through smaller or split render blocks.
    static constexpr int kTrimRampFrames = 64;
    float trimStart_ = 1.0f, trimStep_ = 0.0f;
    int trimFramesLeft_ = 0;
    double frameHz_ = 60.0, vibratoStep_ = 0.0;
    int vibratoDelay_ = 0, glideFrames_ = 0;
    int arpKey_ = 0, arpCount_ = 0;     // the key the arpeggio last played, and the ticks since
    std::int64_t arpRank_ = 0;          // that key's place in the order (ArpRank)
    bool arpUp_ = true;                 // up and down: the way it is going
    std::uint64_t struck_ = 0;
    // A frame of every unit is periodNum_ / periodDen_ of its cycles; each
    // unit's next falls at tickNum_ / periodDen_, counted from its power-on.
    std::int64_t tickNum_[2 * kMaxBoards] {}, periodNum_ = 1, periodDen_ = 1;
    // Sync, in 2^-kSongBits host samples: the arpeggio's next step is due
    // arpDue_ after the next Process call starts, arpStep_ apart, and a tick
    // takes it when it is due less than halfTick_ after it.
    static constexpr int kSongBits = 32;
    bool sync_ = false;
    int arpSync_ = 4, vibratoSync_ = 5, glideSync_ = 2;
    double tempo_ = 120.0;
    double song_ = -1.0;                // the song's position in beats, negative while it is stopped
    std::int64_t arpDue_ = 0, arpStep_ = 0, halfTick_ = 0;

    int rate_ = 48000;
    int oversampling_ = 1;
    bool double_ = false;
    double detune_ = 0.0;
    int mode_ = 0, arpRate_ = 2, bendRange_ = 2;
    int arpOrder_ = 0, ticks_ = 1;
    bool arpRetrigger_ = true;
    int envelope_ = 0;                  // Envelope: 0 driver, 1 chip, 2 chip looping
    int sweep_ = 0, sweepPace_ = 4;
    bool spread_ = false;               // Sides
    bool chip_ = false;                 // Poly channels: the chip's other pitched parts too
    double vibratoDepth_ = 0.0, velocity_ = 0.5, gain_ = 0.0;
    // Volume's ramp: from gainFrom_ by gainStep_ a host sample, gainLeft_ to go.
    static constexpr int kGainRampFrames = 64;
    double gainFrom_ = 0.0, gainStep_ = 0.0;
    int gainLeft_ = 0;
    double bend_ = 0.0, wheel_ = 0.0, tune_ = 0.0;
    bool pedal_ = false;
    bool retryPitch_ = false;
    std::uint64_t age_ = 0;

    StereoDecimator decimator_;
    // The machines' left and right, then the Double set's, at the machines'
    // rate; the bus's left and right are 0 and 1 after the fold.
    float bus_[4][StepBuffer::kMaxFrames] {};
    OutputStage<2> stage_;
};

} // namespace protocodus::reboot
