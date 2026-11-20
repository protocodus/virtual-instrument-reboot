#pragma once

// Band-limited rendering of a piecewise-constant signal.
//
// A board's mixed output is exactly a list of (time, level) steps (README, How
// it works). Each step adds the band-limited impulse of BlepKernel.h, scaled
// by the level's change, into a difference buffer; integrating the buffer
// gives the steps band-limited to the host rate. Levels are integers in units
// of 2^-kLevelBits and every kernel phase sums to exactly 2^kUnitBits, so the
// integral returns to the exact level after every step, however many: no
// drift, and the same bits on every processor.
//
// Times are in 1/16384 of a host sample from the start of the block (8 bits
// pick the kernel phase, 6 interpolate to the next). The output is
// blep::kDelay samples late.

#include "BlepKernel.h"

#include <cstdint>
#include <cstring>

namespace protocodus::reboot {

class StepBuffer {
public:
    static constexpr int kMaxFrames = 256;
    static constexpr int kSubBits = 14;           // position bits under a sample
    static constexpr int kFracBits = kSubBits - blep::kPhaseBits;
    static constexpr int kLevelBits = 16;
    static constexpr std::int64_t kSamples = std::int64_t { 1 } << kSubBits;

    void Clear() noexcept {
        std::memset(diff_, 0, sizeof diff_);
        sum_ = 0;
        pending_ = 0;
    }

    // A change of `delta` (in 2^-kLevelBits) at `position` (in 2^-kSubBits
    // samples from the block's start, under kMaxFrames samples).
    void Add(std::int64_t position, std::int32_t delta) noexcept {
        if (delta == 0)
            return;
        const int sample = static_cast<int>(position >> kSubBits);
        if (sample + blep::kTaps > pending_)
            pending_ = sample + blep::kTaps;
        const int sub = static_cast<int>(position & (kSamples - 1));
        const int phase = sub >> kFracBits;
        const int frac = sub & ((1 << kFracBits) - 1);
        const std::int32_t* a = blep::kTable[phase];
        const std::int32_t* b = blep::kTable[phase + 1];
        // Kernel taps are signed Q20 and the two weights sum to 64, so the
        // interpolated coefficient fits in int32. Widen only the final
        // level multiplication; the integer convolution stays exact.
        const int weightA = (1 << kFracBits) - frac;
        std::int64_t* out = diff_ + sample;
        for (int j = 0; j < blep::kTaps; ++j) {
            const std::int32_t coefficient = weightA * a[j] + frac * b[j];
            out[j] += static_cast<std::int64_t>(delta) * coefficient;
        }
    }

    // Integrates `frames` samples into out (added to it, times gain) and
    // moves the kernel tails to the front.
    void Read(float* out, int frames, double gain) noexcept {
        const double scale = gain / static_cast<double>(std::int64_t { 1 } << (kLevelBits + blep::kUnitBits + kFracBits));
        const int count = frames < pending_ ? frames : pending_;
        for (int i = 0; i < count; ++i) {
            sum_ += diff_[i];
            out[i] += static_cast<float>(static_cast<double>(sum_) * scale);
        }
        const float settled = static_cast<float>(static_cast<double>(sum_) * scale);
        for (int i = count; i < frames; ++i)
            out[i] += settled;
        Advance(count);
    }

    // Writes each sample's band-limited step increment (without integrating into
    // sum_) and advances the kernel tail, for feeding leaky/RC residual decays
    // without unbounded accumulator growth.
    void ReadDelta(float* out, int frames, double gain = 1.0) noexcept {
        const double scale = gain / static_cast<double>(std::int64_t { 1 } << (kLevelBits + blep::kUnitBits + kFracBits));
        const int count = frames < pending_ ? frames : pending_;
        for (int i = 0; i < count; ++i)
            out[i] = static_cast<float>(static_cast<double>(diff_[i]) * scale);
        const float zero = static_cast<float>(0.0 * scale);
        for (int i = count; i < frames; ++i)
            out[i] = zero;
        Advance(count);
    }

    // Lands every step in flight at once, and returns the level it leaves.
    double Land() noexcept {
        for (std::int64_t& d : diff_) {
            sum_ += d;
            d = 0;
        }
        pending_ = 0;
        return static_cast<double>(sum_) / static_cast<double>(std::int64_t { 1 } << (kLevelBits + blep::kUnitBits + kFracBits));
    }

    // The level the integral has settled to, once no step is in flight.
    [[nodiscard]] std::int64_t Settled() const noexcept {
        std::int64_t total = sum_;
        for (int j = 0; j < blep::kTaps; ++j)
            total += diff_[j];
        return total >> (blep::kUnitBits + kFracBits);
    }

private:
    // Cells from pending_ onward are known zeros. Only the part touched by
    // recent BLEP edges needs to be integrated, shifted or cleared.
    void Advance(int count) noexcept {
        if (count == 0)
            return;
        pending_ -= count;
        if (pending_ > 0)
            std::memmove(diff_, diff_ + count, sizeof(std::int64_t) * static_cast<std::size_t>(pending_));
        std::memset(diff_ + pending_, 0, sizeof(std::int64_t) * static_cast<std::size_t>(count));
    }

    int pending_ = 0;
    std::int64_t diff_[kMaxFrames + blep::kTaps] {};
    std::int64_t sum_ = 0;
};

// A level in the buffer's integer units, from a float level.
inline std::int32_t QuantizeLevel(float level) noexcept {
    const float scaled = level * static_cast<float>(1 << StepBuffer::kLevelBits);
    return static_cast<std::int32_t>(scaled < 0.0f ? scaled - 0.5f : scaled + 0.5f);
}

} // namespace protocodus::reboot
