#pragma once

// Oversampling's way back to the host rate: one to three 2:1 halfband
// stages (HalfbandKernel.h, Tools/generate_halfband.py). Each stage keeps the
// odd input samples, so with the kernel's half length of 33 it delays its
// output by exactly 16 of its output samples, and the chain by a whole number
// of host samples at every factor (Engine::LatencySamples).

#include "HalfbandKernel.h"

#include <cstring>

namespace protocodus::reboot {

class Halfband {
public:
    static constexpr int kMaxIn = 256;

    void Reset() noexcept { std::memset(x_, 0, sizeof x_); }

    // Filters 2 * frames input samples into `frames` output samples, `out`
    // may be `in`.
    void Process(const float* in, float* out, int frames) noexcept {
        constexpr int kHistory = 2 * halfband::kHalfLength;
        float* x = x_ + kHistory;
        std::memcpy(x, in, sizeof(float) * static_cast<std::size_t>(2 * frames));
        for (int j = 0; j < frames; ++j) {
            // The window ends at input 2j + 1; its centre is kHalfLength back.
            const float* c = x + 2 * j + 1 - halfband::kHalfLength;
            double y = 0.5 * c[0];
            for (int k = 0; k < halfband::kSideTaps; ++k)
                y += halfband::kSide[k] * (static_cast<double>(c[-(2 * k + 1)]) + c[2 * k + 1]);
            out[j] = static_cast<float>(y);
        }
        std::memmove(x_, x_ + 2 * frames, sizeof(float) * kHistory);
    }

private:
    float x_[2 * halfband::kHalfLength + kMaxIn] {};
};

// Brings `factor` (1, 2, 4 or 8) times the host rate down to it, in place.
class Decimator {
public:
    // The delay in host samples the stages add: 16 output samples a stage.
    static constexpr int DelayFor(int factor) noexcept {
        return factor >= 8 ? 28 : factor >= 4 ? 24 : factor >= 2 ? 16 : 0;
    }

    void Prepare(int factor) noexcept {
        stages_ = factor >= 8 ? 3 : factor >= 4 ? 2 : factor >= 2 ? 1 : 0;
        Reset();
    }

    void Reset() noexcept {
        for (Halfband& h : halves_)
            h.Reset();
    }

    // `frames` host samples from frames * 2^stages input samples in `buffer`.
    void Process(float* buffer, int frames) noexcept {
        for (int s = 0; s < stages_; ++s)
            halves_[s].Process(buffer, buffer, frames << (stages_ - 1 - s));
    }

private:
    Halfband halves_[3];
    int stages_ = 0;
};

// After identical stereo input has filled every FIR history, the filtered
// channels are identical too. Run a single FIR chain until stereo returns.
class StereoDecimator {
public:
    void Prepare(int factor) noexcept {
        factor_ = factor;
        left_.Prepare(factor);
        right_.Prepare(factor);
        equalFrames_ = kFlush;
    }

    void Reset() noexcept {
        left_.Reset();
        right_.Reset();
        equalFrames_ = kFlush;
    }

    void Process(float* left, float* right, int frames) noexcept {
        if (factor_ == 1)
            return;
        const bool equal = std::memcmp(left, right, sizeof(float) * static_cast<std::size_t>(frames * factor_)) == 0;
        if (equal && equalFrames_ == kFlush) {
            left_.Process(left, frames);
            std::memcpy(right, left, sizeof(float) * static_cast<std::size_t>(frames));
            return;
        }
        if (!equal) {
            if (equalFrames_ == kFlush)
                right_ = left_;
            equalFrames_ = 0;
        } else {
            equalFrames_ += frames;
            if (equalFrames_ > kFlush)
                equalFrames_ = kFlush;
        }
        left_.Process(left, frames);
        right_.Process(right, frames);
    }

private:
    // The oldest input at the last output is less than 66 host samples
    // back even with three stages: 66 * (1/2 + 1/4 + 1/8).
    static constexpr int kFlush = 2 * halfband::kHalfLength;
    int factor_ = 1;
    int equalFrames_ = kFlush;
    Decimator left_, right_;
};

} // namespace protocodus::reboot
