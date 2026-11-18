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
    }

    // A change of `delta` (in 2^-kLevelBits) at `position` (in 2^-kSubBits
    // samples from the block's start, under kMaxFrames samples).
    void Add(std::int64_t position, std::int32_t delta) noexcept {
        if (delta == 0)
            return;
        const int sample = static_cast<int>(position >> kSubBits);
        const int sub = static_cast<int>(position & (kSamples - 1));
        const int phase = sub >> kFracBits;
        const int frac = sub & ((1 << kFracBits) - 1);
        const std::int32_t* a = blep::kTable[phase];
        const std::int32_t* b = blep::kTable[phase + 1];
        const std::int64_t wa = static_cast<std::int64_t>(delta) * ((1 << kFracBits) - frac);
        const std::int64_t wb = static_cast<std::int64_t>(delta) * frac;
        std::int64_t* out = diff_ + sample;
        for (int j = 0; j < blep::kTaps; ++j)
            out[j] += wa * a[j] + wb * b[j];
    }

    // Integrates `frames` samples into out (added to it, times gain) and
    // moves the kernel tails to the front.
    void Read(float* out, int frames, double gain) noexcept {
        const double scale = gain / static_cast<double>(std::int64_t { 1 } << (kLevelBits + blep::kUnitBits + kFracBits));
        for (int i = 0; i < frames; ++i) {
            sum_ += diff_[i];
            out[i] += static_cast<float>(static_cast<double>(sum_) * scale);
        }
        std::memmove(diff_, diff_ + frames, sizeof(std::int64_t) * blep::kTaps);
        std::memset(diff_ + blep::kTaps, 0, sizeof(std::int64_t) * static_cast<std::size_t>(frames));
    }

    // Lands every step in flight at once, and returns the level it leaves.
    double Land() noexcept {
        for (std::int64_t& d : diff_) {
            sum_ += d;
            d = 0;
        }
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
    std::int64_t diff_[kMaxFrames + blep::kTaps] {};
    std::int64_t sum_ = 0;
};

// A level in the buffer's integer units, from a float level.
inline std::int32_t QuantizeLevel(float level) noexcept {
    const float scaled = level * static_cast<float>(1 << StepBuffer::kLevelBits);
    return static_cast<std::int32_t>(scaled < 0.0f ? scaled - 0.5f : scaled + 0.5f);
}

} // namespace protocodus::reboot
