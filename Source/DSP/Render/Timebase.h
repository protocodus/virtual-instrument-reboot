#pragma once

// A board's master clock against the host's sample clock, in exact integers.
//
// The clock is num/den Hz (the 2A03's 19687500/11 Hz is not a whole number)
// and the host rate a whole number of hertz, so cycle c falls at
// c * rate * den / num samples. Measured from the block's start in units of
// 1/num of a sample, cycle c sits at f0 + (c - c0) * rate * den, where c0 is
// the block's first cycle and f0 its offset: every quantity stays small, so
// nothing drifts or overflows however long the host plays.

#include <cstdint>

namespace protocodus::reboot {

class Timebase {
public:
    // Setup: restarts at cycle 0 on the block boundary.
    void Set(std::int64_t clockNum, std::int64_t clockDen, int sampleRate) noexcept {
        SetAt(0, clockNum, clockDen, sampleRate);
    }

    // Setup: aligns sample 0 of the block to `startCycle`.
    void SetAt(std::int64_t startCycle, std::int64_t clockNum, std::int64_t clockDen, int sampleRate) noexcept {
        perSample_ = clockNum;
        perCycle_ = static_cast<std::int64_t>(sampleRate) * clockDen;
        c0_ = startCycle;
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

    // Where `cycle` (of this block) falls, in 2^-subBits samples from the
    // block's start, rounded down.
    [[nodiscard]] std::int64_t Position(std::int64_t cycle, int subBits) const noexcept {
        const std::int64_t units = f0_ + (cycle - c0_) * perCycle_;
        return (units << subBits) / perSample_;
    }

    // Moves the block start `frames` samples on.
    void Advance(int frames) noexcept {
        const std::int64_t next = CycleAt(frames);
        f0_ += (next - c0_) * perCycle_ - static_cast<std::int64_t>(frames) * perSample_;
        c0_ = next;
    }

private:
    std::int64_t perSample_ = 1; // num: one sample in units of 1/num sample
    std::int64_t perCycle_ = 1;  // rate * den: one cycle in the same units
    std::int64_t c0_ = 0;
    std::int64_t f0_ = 0;
};

} // namespace protocodus::reboot
