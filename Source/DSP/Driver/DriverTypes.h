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
