#pragma once

// Reboot's player controls, shared by every front end: the property or
// parameter id, the range in the units the engine takes, the default and the
// mapping from a normalised 0..1 position. Front ends store and automate the
// normalised positions; the engine takes the values (Engine::SetControls).
//
// Ids and names are generic: the boards are the Handheld, the Console, the
// PSG and the Micro, and the Console's cartridge chips are expansions A to E.
// Never rename or reuse an id, and append new controls at the end: saved
// sessions and Rack patches recall by them.

#include <array>
#include <cmath>
#include <cstddef>

namespace protocodus::reboot {

enum Control : int {
    kBoard,             // 0 Handheld, 1 Console, 2 PSG, 3 Micro
    kRevision,          // the board's revision (README, How it works); past the board's last, its last
    kExpansion,         // the Console's cartridge chip: 0 none, then A to E
    kExpansionChannels, // how many channels expansion C runs, 1 to 8
    kBias,              // the Console's held DMC level, 0 to 127
    kPath,              // 0 Pin (the chips' DACs and mixer), 1 Line (the machine's output stage)
    kBoards,            // machines played at once, 1 to 8
    kMode,              // 0 Poly, 1 Mono, 2 Arp
    kArpRate,           // frames a step of the arpeggio holds
    kVibratoDepth,      // cents either way; the mod wheel adds up to 50 more
    kVibratoRate,       // Hz
    kVibratoDelay,      // ms from a note's start before the vibrato begins
    kGlide,             // ms a Mono legato note takes to reach its pitch
    kBendRange,         // semitones the pitch wheel bends either way
    kVelocity,          // 0 every note at its layer's level .. 1 level times velocity
    kVolume,            // output level, dB: our trim after the machines (README, How it works)
    kAOn, kAPart, kAShape, kALevel, kAAttack, kADecay, kASustain, kARelease, kATranspose, kAFine, kAMod,
    kBOn, kBPart, kBShape, kBLevel, kBAttack, kBDecay, kBSustain, kBRelease, kBTranspose, kBFine, kBMod,
    kDouble,            // 0 one set of machines; 1 a second set too, the first left and the second right
    kDetune,            // cents between the two sets with Double, half of it either way
    kControlCount
};

// A layer's controls, as offsets from kAOn or kBOn.
enum LayerControl : int {
    kOn,        // the layer plays
    kPart,      // which of the board's parts (Machine::PartOf), 0 to 4
    kShape,     // the part's duty, wave, noise mode or envelope shape, 0 to 7
    kLevel,     // the chip's volume steps, 0 to 15
    kAttack,    // frames a volume step takes on the way up; 0 starts at full
    kDecay,     // frames a step takes down to the sustain level; 0 goes at once
    kSustain,   // the held level, 0 to 15 of the level
    kRelease,   // frames a step takes down after the key; 0 stops at once
    kTranspose, // semitones
    kFine,      // cents
    kMod,       // the expansion D wave's modulation depth, 0 to 63
    kLayerControls
};
constexpr int kLayers = 2;
constexpr int kLayerBase[kLayers] = { kAOn, kBOn };

enum class Curve { kLinear, kLog, kInteger, kStepped };

struct ControlInfo {
    const char* id;    // stable parameter / property id
    const char* name;
    double min, max, initial;
    int steps;         // > 0 for a switch of `steps` values from min
    Curve curve;
    const char* unit;
};

inline constexpr std::array<ControlInfo, kControlCount> kControlInfo { {
    { "board", "Board", 0.0, 3.0, 0.0, 4, Curve::kStepped, "" },
    { "revision", "Revision", 0.0, 2.0, 0.0, 3, Curve::kStepped, "" },
    { "expansion", "Expansion", 0.0, 5.0, 0.0, 6, Curve::kStepped, "" },
    { "expansion_channels", "Expansion channels", 1.0, 8.0, 4.0, 8, Curve::kStepped, "" },
    { "bias", "Bias", 0.0, 127.0, 0.0, 0, Curve::kInteger, "" },
    { "path", "Path", 0.0, 1.0, 1.0, 2, Curve::kStepped, "" },
    { "boards", "Boards", 1.0, 8.0, 2.0, 8, Curve::kStepped, "" },
    { "mode", "Mode", 0.0, 2.0, 0.0, 3, Curve::kStepped, "" },
    { "arp_rate", "Arp rate", 1.0, 8.0, 2.0, 0, Curve::kInteger, "frames" },
    { "vibrato_depth", "Vibrato", 0.0, 100.0, 0.0, 0, Curve::kLinear, "cents" },
    { "vibrato_rate", "Vibrato rate", 0.5, 12.0, 5.5, 0, Curve::kLog, "Hz" },
    { "vibrato_delay", "Vibrato delay", 0.0, 1000.0, 0.0, 0, Curve::kLinear, "ms" },
    { "glide", "Glide", 0.0, 1000.0, 0.0, 0, Curve::kLinear, "ms" },
    { "bend_range", "Bend range", 0.0, 12.0, 2.0, 0, Curve::kInteger, "semitones" },
    { "velocity", "Velocity", 0.0, 1.0, 0.5, 0, Curve::kLinear, "" },
    { "volume", "Volume", -24.0, 6.0, 0.0, 0, Curve::kLinear, "dB" },
    { "a_on", "A On", 0.0, 1.0, 1.0, 2, Curve::kStepped, "" },
    { "a_part", "A Part", 0.0, 4.0, 0.0, 5, Curve::kStepped, "" },
    { "a_shape", "A Shape", 0.0, 7.0, 2.0, 8, Curve::kStepped, "" },
    { "a_level", "A Level", 0.0, 15.0, 12.0, 0, Curve::kInteger, "" },
    { "a_attack", "A Attack", 0.0, 30.0, 0.0, 0, Curve::kInteger, "frames" },
    { "a_decay", "A Decay", 0.0, 30.0, 0.0, 0, Curve::kInteger, "frames" },
    { "a_sustain", "A Sustain", 0.0, 15.0, 15.0, 0, Curve::kInteger, "" },
    { "a_release", "A Release", 0.0, 30.0, 2.0, 0, Curve::kInteger, "frames" },
    { "a_transpose", "A Transpose", -24.0, 24.0, 0.0, 0, Curve::kInteger, "semitones" },
    { "a_fine", "A Fine", -50.0, 50.0, 0.0, 0, Curve::kLinear, "cents" },
    { "a_mod", "A Mod", 0.0, 63.0, 0.0, 0, Curve::kInteger, "" },
    { "b_on", "B On", 0.0, 1.0, 0.0, 2, Curve::kStepped, "" },
    { "b_part", "B Part", 0.0, 4.0, 0.0, 5, Curve::kStepped, "" },
    { "b_shape", "B Shape", 0.0, 7.0, 2.0, 8, Curve::kStepped, "" },
    { "b_level", "B Level", 0.0, 15.0, 12.0, 0, Curve::kInteger, "" },
    { "b_attack", "B Attack", 0.0, 30.0, 0.0, 0, Curve::kInteger, "frames" },
    { "b_decay", "B Decay", 0.0, 30.0, 0.0, 0, Curve::kInteger, "frames" },
    { "b_sustain", "B Sustain", 0.0, 15.0, 15.0, 0, Curve::kInteger, "" },
    { "b_release", "B Release", 0.0, 30.0, 2.0, 0, Curve::kInteger, "frames" },
    { "b_transpose", "B Transpose", -24.0, 24.0, 0.0, 0, Curve::kInteger, "semitones" },
    { "b_fine", "B Fine", -50.0, 50.0, 0.0, 0, Curve::kLinear, "cents" },
    { "b_mod", "B Mod", 0.0, 63.0, 0.0, 0, Curve::kInteger, "" },
    { "double", "Double", 0.0, 1.0, 0.0, 2, Curve::kStepped, "" },
    { "detune", "Detune", 0.0, 50.0, 8.0, 0, Curve::kLinear, "cents" },
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
    case Curve::kLinear: break;
    }
    return c.min + (c.max - c.min) * n;
}

inline double ToNormalized(int index, double value) {
    const ControlInfo& c = kControlInfo[static_cast<std::size_t>(index)];
    value = value == value ? (value < c.min ? c.min : value > c.max ? c.max : value) : c.initial;
    if (c.curve == Curve::kLog)
        return std::log(value / c.min) / std::log(c.max / c.min);
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

} // namespace protocodus::reboot
