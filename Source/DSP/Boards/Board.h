#pragma once

// What every board blackbox shares: the step mix from a chip's DAC codes to
// its board's buses, the Pin path's DC blocker, and a cycle-scheduled CPU bus
// queue so multi-register writes arrive at realistic bus cycle intervals.
//
// A board is one physical machine: its chips on its master clock, their DACs,
// its mixer, its CPU bus, and its output stage (README, How it works). It knows
// nothing about MIDI, notes, voices, or waveforms; the sound driver talks to a
// board exclusively through CPU bus writes (`Write(address, value)`) and time
// advancement (`RunTo(cycle)`).

#include "../Render/Filters.h"
#include "../Render/StepBuffer.h"
#include "../Render/Timebase.h"

#include <cstdint>

namespace protocodus::reboot {

// Which output the board renders: the chip's pins (its DAC and mixer) or the
// machine's line output (the board's own stage after them).
enum class Path : std::uint8_t { Pin, Line };

// The Pin path has no stage of the machine's, but a DAC sum carries the
// chips' DC offsets (a Game Boy channel at rest sits at +1); a first-order
// high-pass at 5 Hz takes them out. Ours, under every board's Pin path.
constexpr double kPinBlockerHz = 5.0;

// A single CPU bus write at a scheduled master clock cycle.
struct BusWrite {
    std::int64_t cycle = 0;
    std::uint16_t address = 0;
    std::uint8_t value = 0;
};

// Fixed-capacity circular queue of scheduled CPU bus writes.
template <int Capacity = 1024>
class BusWriteQueue {
public:
    void Clear() noexcept {
        head_ = 0;
        count_ = 0;
        nextCycle_ = 0;
        lastAddress_ = 0xFFFF;
        lastValue_ = 0;
        hasLast_ = false;
    }

    void ResetTiming(std::int64_t now) noexcept {
        nextCycle_ = now;
        lastAddress_ = 0xFFFF;
        lastValue_ = 0;
        hasLast_ = false;
    }

    [[nodiscard]] bool Empty() const noexcept { return count_ == 0; }
    [[nodiscard]] bool Full() const noexcept { return count_ == Capacity; }
    [[nodiscard]] bool HasLast() const noexcept { return hasLast_; }
    [[nodiscard]] std::uint16_t LastAddress() const noexcept { return lastAddress_; }
    [[nodiscard]] std::uint8_t LastValue() const noexcept { return lastValue_; }

    [[nodiscard]] bool CanExecuteNow(std::int64_t now) const noexcept {
        return count_ == 0 && nextCycle_ <= now;
    }

    void MarkImmediate(std::int64_t now, int stepCycles, std::uint16_t address, std::uint8_t value) noexcept {
        nextCycle_ = now + stepCycles;
        lastAddress_ = address;
        lastValue_ = value;
        hasLast_ = true;
    }

    void Schedule(std::int64_t now, int stepCycles, std::uint16_t address, std::uint8_t value) noexcept {
        if (Full())
            return;
        const std::int64_t due = nextCycle_ > now ? nextCycle_ : now;
        nextCycle_ = due + stepCycles;
        lastAddress_ = address;
        lastValue_ = value;
        hasLast_ = true;
        const int tail = (head_ + count_) % Capacity;
        ring_[tail] = { due, address, value };
        ++count_;
    }

    [[nodiscard]] const BusWrite& Front() const noexcept { return ring_[head_]; }

    BusWrite Pop() noexcept {
        const BusWrite w = ring_[head_];
        head_ = (head_ + 1) % Capacity;
        --count_;
        return w;
    }

    [[nodiscard]] std::int64_t LastCycle() const noexcept {
        if (count_ == 0)
            return nextCycle_;
        const int tail = (head_ + count_ - 1) % Capacity;
        return ring_[tail].cycle;
    }

private:
    BusWrite ring_[Capacity] {};
    int head_ = 0, count_ = 0;
    std::int64_t nextCycle_ = 0;
    std::uint16_t lastAddress_ = 0xFFFF;
    std::uint8_t lastValue_ = 0;
    bool hasLast_ = false;
};

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

} // namespace protocodus::reboot
