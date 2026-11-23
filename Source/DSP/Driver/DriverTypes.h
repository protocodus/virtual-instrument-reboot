#pragma once

// Shared playing-layer types used by the machine drivers and engine.
// The hardware boards (Boards/) know nothing about these types.

#include <cstdint>

namespace protocodus::reboot {

// What a channel plays, as the playing layer chooses parts by it.
enum class Voice : std::uint8_t {
    Pulse,     // rectangle with a duty register
    Triangle,  // the 2A03's 32-step triangle, no volume
    Wave,      // a channel playing a wave from RAM
    ModWave,   // the disk unit's wave with its modulator
    Saw,       // the VRC6's accumulator
    Tone,      // a square with no duty register (the 5B)
    Noise,
};

// A note's settings in the chip's own units. The driver computes `period` with
// PeriodFor, stores the chip's native volume step in `volume` via
// QuantizeVolume, and passes the part's shape and modulation.
struct ChannelSettings {
    Voice voice = Voice::Pulse;
    std::int64_t period = 0;
    int volume = 0;
    int shape = 0;
    int mod = 0;
    // The channel's own units, where it has them (Machine::EnvelopeOf,
    // Machine::HasSweep); a channel without one plays as if they were 0.
    int pace = 0;         // its envelope unit steps the volume once every `pace` of its steps; 0: the volume stays as written
    bool up = false;      // that envelope climbs; else it falls
    bool loop = false;    // a fallen envelope starts again from the top (the console's)
    int sweep = 0;        // its sweep unit's pitch steps from the start: 1 to 7 a step's size (8 - its shift), + up, - down; 0 none
    int sweepPace = 0;    // the sweep's steps apart, 1 to 8 of the unit's own (a pace past the unit's last is its last)
    bool spread = false;  // the handheld's sides (NR51): pulse 1 left, pulse 2 right, the others both; else every channel both
};

// A channel's own envelope unit, as a driver must know it to play it.
struct EnvelopeUnit {
    int paces = 0;              // pace codes 1 to `paces`; 0: the channel has none
    double cycles = 0.0;        // the board's cycles one volume step takes at pace 1; pace n takes n times as long
    bool climbs = false;        // it can count up as well as down
    bool fromVolume = false;    // it starts from the volume written; else always from 15
    bool loops = false;         // it can start again from 15 when it reaches 0
};

// The whole period register nearest in pitch to an exact real one. Every
// pitch here goes as a count or as its inverse, so the nearer count is the
// nearer in ratio: the lower one while count is under the geometric mean
// of the two. Under 1 the count rounds as before, so a key far over a
// channel's top note stays out of its reach.
inline std::int64_t NearestCount(double count) noexcept {
    if (!(count < 1.0e15))
        return std::int64_t { 1 } << 50;
    if (count < 1.0)
        return count >= 0.5 ? 1 : 0;
    const auto low = static_cast<std::int64_t>(count);
    const double l = static_cast<double>(low);
    return count * count < l * (l + 1.0) ? low : low + 1;
}

} // namespace protocodus::reboot
