#pragma once

// A board's master clock against the host's sample clock, in exact integers.
//
// The clock is num/den Hz (the 2A03's 19687500/11 Hz is not a whole number),
// off nominal by a trim in 0.01 ppm (a unit's own crystal, and a pull on
// it), and the host rate a whole number of hertz. With T = kTrimUnit, cycle c
// falls at c * rate * den * T / (num * (T + trim)) samples. Measured from the
// block's start in units of 1/(num * (T + trim)) of a sample, cycle c sits at
// f0 + (c - c0) * rate * den * T, where c0 is the block's first cycle and f0
// its offset: every quantity stays inside 64 bits, so nothing drifts or
// overflows however long the host plays. (The largest are a block of
// StepBuffer::kMaxFrames samples, 256 * 53203425 * 1.03e8 = 1.4e18 on the PAL
// console with the trim 3 % fast, and a remainder shifted 7 bits in
// Position, 7e17.)

#include <cstdint>

namespace protocodus::reboot {

class Timebase {
public:
    // A trim of 1 is one part in kTrimUnit of the clock: 0.01 ppm.
    static constexpr std::int64_t kTrimUnit = 100000000;

    // Setup: restarts at cycle 0 on the block boundary.
    void Set(std::int64_t clockNum, std::int64_t clockDen, int sampleRate, std::int64_t trim = 0) noexcept {
        num_ = clockNum;
        perSample_ = clockNum * (kTrimUnit + trim);
        perCycle_ = static_cast<std::int64_t>(sampleRate) * clockDen * kTrimUnit;
        c0_ = 0;
        f0_ = 0;
    }

    // Retunes the clock from the block's start on, as a trimmer turned
    // while it runs: the block's first cycle keeps its place in time.
    void Trim(std::int64_t trim) noexcept {
        const std::int64_t next = num_ * (kTrimUnit + trim);
        if (next == perSample_)
            return;
        f0_ = static_cast<std::int64_t>(static_cast<double>(f0_) * (static_cast<double>(next) / static_cast<double>(perSample_)));
        perSample_ = next;
    }

    // Starts the block at `cycle`, on a sample boundary: a unit that has
    // run before the host's first sample.
    void Restart(std::int64_t cycle) noexcept {
        c0_ = cycle;
        f0_ = 0;
    }

    // The first cycle of the block.
    [[nodiscard]] std::int64_t Start() const noexcept { return c0_; }

    // The first cycle at or after `frame` samples into the block.
    [[nodiscard]] std::int64_t CycleAt(int frame) const noexcept {
        const std::int64_t target = static_cast<std::int64_t>(frame) * perSample_ - f0_;
        if (target <= 0)
            return c0_;
        return c0_ + (target + perCycle_ - 1) / perCycle_;
    }

    // The first cycle at or after `position`, in 2^-subBits samples from
    // the block's start: the cycle another unit's moment falls on.
    [[nodiscard]] std::int64_t CycleAtPosition(std::int64_t position, int subBits) const noexcept {
        const std::int64_t mask = (std::int64_t { 1 } << subBits) - 1;
        const std::int64_t whole = position >> subBits, sub = position & mask;
        // position * perSample / 2^subBits rounded up, the product taken in
        // two halves to stay inside 64 bits.
        const std::int64_t target = whole * perSample_ + sub * (perSample_ >> subBits)
            + ((sub * (perSample_ & mask) + mask) >> subBits) - f0_;
        if (target <= 0)
            return c0_;
        return c0_ + (target + perCycle_ - 1) / perCycle_;
    }

    // Where `cycle` (of this block) falls, in 2^-subBits samples from the
    // block's start, rounded down. The division is long, in two steps of
    // half the bits, so the remainder's shift stays inside 64 bits.
    [[nodiscard]] std::int64_t Position(std::int64_t cycle, int subBits) const noexcept {
        const std::int64_t units = f0_ + (cycle - c0_) * perCycle_;
        const int half = subBits / 2;
        std::int64_t q = units / perSample_, r = units % perSample_;
        r <<= half;
        q = (q << half) + r / perSample_;
        r %= perSample_;
        r <<= subBits - half;
        return (q << (subBits - half)) + r / perSample_;
    }

    // Moves the block start `frames` samples on.
    void Advance(int frames) noexcept {
        const std::int64_t next = CycleAt(frames);
        f0_ += (next - c0_) * perCycle_ - static_cast<std::int64_t>(frames) * perSample_;
        c0_ = next;
    }

private:
    std::int64_t num_ = 1;
    std::int64_t perSample_ = 1; // num * (T + trim): one sample in the units above
    std::int64_t perCycle_ = 1;  // rate * den * T: one cycle in the same units
    std::int64_t c0_ = 0;
    std::int64_t f0_ = 0;
};

} // namespace protocodus::reboot
