#pragma once

// Reboot's player controls, shared by every front end: the property or
// parameter id, the range in the units the engine takes, the default and the
// mapping from a normalised 0..1 position. Front ends store and automate the
// normalised positions; the engine takes the values (Engine::SetControls).
//
// Ids and names are generic: the boards are the Handheld and the Console,
// and the Console's cartridge chips are expansions A to E. Never rename or
// reuse an id, and append new controls at the end: saved sessions and Rack
// patches recall by them. (0.4.0 renumbered them all once, with the owner's
// leave: no session or patch of 0.3.0 carries over.)

#include <array>
#include <cmath>
#include <cstddef>

namespace protocodus::reboot {

enum Control : int {
    // The machines.
    kBoard,             // 0 Handheld, 1 Console
    kRevision,          // the board's revision (README, How it works); past the board's last, its last
    kExpansion,         // the Console's cartridge chip: 0 none, then A to E
    kExpansionChannels, // how many channels expansion C runs, 1 to 8
    kBias,              // the Console's held DMC level, 0 to 127
    kPath,              // 0 Pin (the chips' DACs and mixer), 1 Line (the machine's output stage)
    kBoards,            // machines played at once, 1 to 8
    // The voice.
    kPart,              // which of the board's parts (Machine::PartOf), 0 to 4
    kShape,             // the part's duty, wave or noise mode, 0 to 7
    kLevel,             // the chip's volume steps, 0 to 15
    kAttack,            // ms from silence to the level; under a frame, at once
    kDecay,             // ms from the level down to the sustain level
    kSustain,           // the held level, 0 to 15 of the level
    kRelease,           // ms from where the key leaves it down to silence; under a frame, at once
    kTranspose,         // semitones
    kFine,              // cents
    kMod,               // the expansion D wave's modulation depth, 0 to 63
    // Playing.
    kMode,              // 0 Poly, 1 Mono, 2 Arp
    kArpRate,           // the driver's ticks a step of the arpeggio holds (a tick is a video frame at Driver rate 1)
    kVibratoDepth,      // cents either way; the mod wheel adds up to 50 more
    kVibratoRate,       // Hz
    kVibratoDelay,      // ms from a note's start before the vibrato begins
    kGlide,             // ms a Mono legato note takes to reach its pitch
    kBendRange,         // semitones the pitch wheel bends either way
    kVelocity,          // 0 every note at the level .. 1 level times velocity
    // The output.
    kDouble,            // 0 one set of units; 1 a second set of the same board too, the first left and the second right
    kDetune,            // cents the two sets' crystals are pulled apart with Double, half of it either way
    kVolume,            // output level, dB: our trim after the machines (README, How it works)
    // Added in 0.6.0, each at the value that plays as 0.5.2 did.
    kPolyChannels,      // Poly's channels: 0 the part's; 1 the chip's other pitched channels too, once the part's are held
    kArpOrder,          // the arpeggio's order: 0 up, 1 down, 2 up and down, 3 as played
    kArpRetrigger,      // 1 each step of the arpeggio starts the envelope again; 0 it moves the pitch only
    kDriverRate,        // the driver's ticks a video frame, 1 to 8: its envelope, vibrato, glide and arpeggio move that often
    // Added in 0.7.0, each at the value that plays as 0.6.0 did.
    kEnvelope,          // 0 the driver writes every volume step; 1 the channel's own envelope unit plays them where it has one; 2 that, looping where the unit can
    kSweep,             // the pulse sweep's step from a note's start, -7 to 7: + up, - down, the size 8 - its shift; 0 none
    kSweepPace,         // the sweep's steps apart, 1 to 8 of its unit's ticks
    kSides,             // the handheld's sides: 0 every channel to both, 1 pulse 1 left and pulse 2 right
    kLoad,              // what the handheld's socket drives on Line: 0 a line input, 1 headphones of 32 Ohm
    // Added in 0.8.0, each at the value that plays as 0.7.0 did.
    kSync,              // 1 Arp rate, Vibrato rate and (from 0.9.0) Glide as note values at the song's tempo (SetTempo), on the driver's ticks nearest them
    kArpSync,           // Arp rate with Sync, a step of: 1/64, 1/32T, 1/32, 1/16T, 1/16, 1/8T, 1/8, 1/4
    kVibratoSync,       // Vibrato rate with Sync, a cycle of: 1/1, 1/2, 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T
    // Added in 0.9.0; without Sync it changes nothing.
    kGlideSync,         // Glide with Sync, a time of: off, 1/64, 1/32, 1/16, 1/8, 1/4, 1/2, 1/1
    kControlCount
};

// kCubic: value = min + (max - min) n^3, for times whose short end needs
// the knob's travel.
enum class Curve { kLinear, kLog, kInteger, kStepped, kCubic };

struct ControlInfo {
    const char* id;    // stable parameter / property id
    const char* name;
    double min, max, initial;
    int steps;         // > 0 for a switch of `steps` values from min
    Curve curve;
    const char* unit;
};

inline constexpr std::array<ControlInfo, kControlCount> kControlInfo { {
    { "board", "Board", 0.0, 1.0, 0.0, 2, Curve::kStepped, "" },
    { "revision", "Revision", 0.0, 2.0, 0.0, 3, Curve::kStepped, "" },
    { "expansion", "Expansion", 0.0, 5.0, 0.0, 6, Curve::kStepped, "" },
    { "expansion_channels", "Expansion channels", 1.0, 8.0, 4.0, 8, Curve::kStepped, "" },
    { "bias", "Bias", 0.0, 127.0, 0.0, 0, Curve::kInteger, "" },
    { "path", "Path", 0.0, 1.0, 1.0, 2, Curve::kStepped, "" },
    { "boards", "Boards", 1.0, 8.0, 2.0, 8, Curve::kStepped, "" },
    { "part", "Part", 0.0, 4.0, 0.0, 5, Curve::kStepped, "" },
    { "shape", "Shape", 0.0, 7.0, 2.0, 8, Curve::kStepped, "" },
    { "level", "Level", 0.0, 15.0, 12.0, 0, Curve::kInteger, "" },
    { "attack", "Attack", 0.0, 5000.0, 0.0, 0, Curve::kCubic, "ms" },
    { "decay", "Decay", 0.0, 5000.0, 400.0, 0, Curve::kCubic, "ms" },
    { "sustain", "Sustain", 0.0, 15.0, 10.0, 0, Curve::kInteger, "" },
    { "release", "Release", 0.0, 5000.0, 250.0, 0, Curve::kCubic, "ms" },
    { "transpose", "Transpose", -24.0, 24.0, 0.0, 0, Curve::kInteger, "semitones" },
    { "fine", "Fine", -50.0, 50.0, 0.0, 0, Curve::kLinear, "cents" },
    { "mod", "Mod", 0.0, 63.0, 0.0, 0, Curve::kInteger, "" },
    { "mode", "Mode", 0.0, 2.0, 0.0, 3, Curve::kStepped, "" },
    { "arp_rate", "Arp rate", 1.0, 8.0, 2.0, 0, Curve::kInteger, "frames" },
    { "vibrato_depth", "Vibrato", 0.0, 100.0, 0.0, 0, Curve::kLinear, "cents" },
    { "vibrato_rate", "Vibrato rate", 0.5, 12.0, 5.5, 0, Curve::kLog, "Hz" },
    { "vibrato_delay", "Vibrato delay", 0.0, 1000.0, 0.0, 0, Curve::kLinear, "ms" },
    { "glide", "Glide", 0.0, 1000.0, 0.0, 0, Curve::kLinear, "ms" },
    { "bend_range", "Bend range", 0.0, 12.0, 2.0, 0, Curve::kInteger, "semitones" },
    { "velocity", "Velocity", 0.0, 1.0, 0.5, 0, Curve::kLinear, "" },
    { "double", "Double", 0.0, 1.0, 0.0, 2, Curve::kStepped, "" },
    { "detune", "Detune", 0.0, 50.0, 8.0, 0, Curve::kLinear, "cents" },
    { "volume", "Volume", -24.0, 6.0, 0.0, 0, Curve::kLinear, "dB" },
    { "poly_channels", "Poly channels", 0.0, 1.0, 0.0, 2, Curve::kStepped, "" },
    { "arp_order", "Arp order", 0.0, 3.0, 0.0, 4, Curve::kStepped, "" },
    { "arp_retrigger", "Arp retrigger", 0.0, 1.0, 1.0, 2, Curve::kStepped, "" },
    { "driver_rate", "Driver rate", 1.0, 8.0, 1.0, 8, Curve::kStepped, "ticks" },
    { "envelope", "Envelope", 0.0, 2.0, 0.0, 3, Curve::kStepped, "" },
    { "sweep", "Sweep", -7.0, 7.0, 0.0, 0, Curve::kInteger, "" },
    { "sweep_pace", "Sweep pace", 1.0, 8.0, 4.0, 8, Curve::kStepped, "" },
    { "sides", "Sides", 0.0, 1.0, 0.0, 2, Curve::kStepped, "" },
    { "load", "Load", 0.0, 1.0, 0.0, 2, Curve::kStepped, "" },
    { "sync", "Sync", 0.0, 1.0, 0.0, 2, Curve::kStepped, "" },
    { "arp_sync", "Arp sync rate", 0.0, 7.0, 4.0, 8, Curve::kStepped, "" },
    { "vibrato_sync", "Vibrato sync rate", 0.0, 7.0, 5.0, 8, Curve::kStepped, "" },
    { "glide_sync", "Glide sync time", 0.0, 7.0, 2.0, 8, Curve::kStepped, "" },
} };

// The value of control `index` at normalised position n (clamped to 0..1).
// Front ends call it on control changes, not per sample.
inline double FromNormalized(int index, double n) {
    const ControlInfo& c = kControlInfo[static_cast<std::size_t>(index)];
    n = n == n ? (n < 0.0 ? 0.0 : n > 1.0 ? 1.0 : n) : 0.0;
    switch (c.curve) {
    case Curve::kStepped: return c.min + std::floor(n * (c.steps - 1) + 0.5);
    case Curve::kInteger: return std::floor(c.min + (c.max - c.min) * n + 0.5);
    case Curve::kLog: return c.min * std::pow(c.max / c.min, n);
    case Curve::kCubic: return c.min + (c.max - c.min) * n * n * n;
    case Curve::kLinear: break;
    }
    return c.min + (c.max - c.min) * n;
}

inline double ToNormalized(int index, double value) {
    const ControlInfo& c = kControlInfo[static_cast<std::size_t>(index)];
    value = value == value ? (value < c.min ? c.min : value > c.max ? c.max : value) : c.initial;
    if (c.curve == Curve::kLog)
        return std::log(value / c.min) / std::log(c.max / c.min);
    if (c.curve == Curve::kCubic)
        return std::cbrt((value - c.min) / (c.max - c.min));
    return (value - c.min) / (c.max - c.min);
}

// The value of every control, in Control order.
struct Controls {
    std::array<double, kControlCount> value {};
    Controls() {
        for (int i = 0; i < kControlCount; ++i)
            value[static_cast<std::size_t>(i)] = kControlInfo[static_cast<std::size_t>(i)].initial;
    }
    double& operator[](int index) { return value[static_cast<std::size_t>(index)]; }
    double operator[](int index) const { return value[static_cast<std::size_t>(index)]; }
};

// The one Sound list a front end may show in place of Board, Expansion and
// Part (owner, 2026-11-23): each sound is a board, the Console's cartridge
// chip and one of the parts Machine::PartOf gives them, and every part the
// machines offer is one sound. A cartridge's sound plays only on the Console
// revision whose slot carries it (Console::kFamicom), so picking one picks
// that revision too. Ours.
struct SoundInfo {
    const char* name;
    int board, expansion, part;
};

inline constexpr int kCartridgeRevision = 1;

inline constexpr std::array<SoundInfo, 12> kSounds { {
    { "Handheld Pulse", 0, 0, 0 }, { "Handheld Wave", 0, 0, 1 }, { "Handheld Noise", 0, 0, 2 },
    { "Console Pulse", 1, 0, 0 }, { "Console Triangle", 1, 0, 1 }, { "Console Noise", 1, 0, 2 },
    { "A Pulse", 1, 1, 3 }, { "A Saw", 1, 1, 4 }, { "B Pulse", 1, 2, 3 },
    { "C Wave", 1, 3, 3 }, { "D Disk", 1, 4, 3 }, { "E Square", 1, 5, 3 },
} };

// Writes sound `index` (clamped to the list) into c's Board, Expansion and
// Part, and for a cartridge's sound its Revision; the other controls stay.
inline void SetSound(Controls& c, int index) {
    const SoundInfo& s = kSounds[static_cast<std::size_t>(index < 0 ? 0 : index >= static_cast<int>(kSounds.size())
                                                                                ? static_cast<int>(kSounds.size()) - 1 : index)];
    c[kBoard] = s.board;
    c[kExpansion] = s.expansion;
    c[kPart] = s.part;
    if (s.expansion != 0)
        c[kRevision] = kCartridgeRevision;
}

// The sound c's Board, Expansion and Part play, or -1 when they are none of
// the list's (a cartridge plugged in under one of the Console's own parts).
inline int SoundOf(const Controls& c) {
    for (std::size_t i = 0; i < kSounds.size(); ++i)
        if (kSounds[i].board == c[kBoard] && kSounds[i].expansion == c[kExpansion] && kSounds[i].part == c[kPart])
            return static_cast<int>(i);
    return -1;
}

} // namespace protocodus::reboot
