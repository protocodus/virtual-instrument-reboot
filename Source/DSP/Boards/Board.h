#pragma once

// What every board shares: the playing layer's view of a channel, the step
// mix from a chip's DAC codes to its board's buses, and the Pin path's DC
// blocker.
//
// A board is one machine: its chips on its master clock, their DACs, its
// mixer and its output stage (README, How it works). The playing layer
// talks to a board only through register writes at chip cycles, which the
// board's Start/Change/Stop make from a ChannelSettings.

#include "../Render/Filters.h"
#include "../Render/StepBuffer.h"
#include "../Render/Timebase.h"

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

// A note's settings in the chip's own units. The playing layer computes
// period with the board's PeriodFor for the same voice, keeps volume on the
// common 0-15 scale (the board maps it to the chip's steps), and passes the
// part's shape and modulation unchanged.
struct ChannelSettings {
    Voice voice = Voice::Pulse;   // what the part plays on the channel
    std::int64_t period = 0;
    int volume = 0;
    int shape = 0;
    int mod = 0;
};

// Which output the board renders: the chip's pins (its DAC and mixer) or the
// machine's line output (the board's own stage after them).
enum class Path : std::uint8_t { Pin, Line };

// The Pin path has no stage of the machine's, but a DAC sum carries the
// chips' DC offsets (a Game Boy channel at rest sits at +1); a first-order
// high-pass at 5 Hz takes them out. Ours, under every board's Pin path.
constexpr double kPinBlockerHz = 5.0;

// The sources' levels onto the board's buses. A source's level is
// quantised once to the step buffer's units, so the difference between two
// quantised totals is exact and the buses never drift.
template <int Sources, int Buses>
class StepMix {
public:
    void Clear() noexcept {
        for (StepBuffer& b : buses_)
            b.Clear();
        for (auto& row : last_)
            for (std::int32_t& v : row)
                v = 0;
    }

    // Source `s`'s levels on each bus from `position` on (in 2^-14 samples).
    void Set(int s, std::int64_t position, const float (&levels)[Buses]) noexcept {
        for (int b = 0; b < Buses; ++b) {
            const std::int32_t q = QuantizeLevel(levels[b]);
            buses_[b].Add(position, q - last_[s][b]);
            last_[s][b] = q;
        }
    }

    void Read(int bus, float* out, int frames) noexcept { buses_[bus].Read(out, frames, 1.0); }

    // Bus `bus`'s steps in flight landed at once; the level they leave.
    double Land(int bus) noexcept { return buses_[bus].Land(); }

    [[nodiscard]] bool Settled() const noexcept {
        for (int s = 0; s < Sources; ++s)
            for (int b = 0; b < Buses; ++b)
                if (last_[s][b] != 0)
                    return false;
        for (const StepBuffer& bus : buses_)
            if (bus.Settled() != 0)
                return false;
        return true;
    }

private:
    StepBuffer buses_[Buses];
    std::int32_t last_[Sources][Buses] {};
};

// A first-order section that may be switched out, for output stages whose
// sections differ between a board's revisions.
inline void DesignPinBlocker(FirstOrder& f, double sampleRate) {
    f.Design(FirstOrder::Kind::HighPass, kPinBlockerHz, sampleRate);
}

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
