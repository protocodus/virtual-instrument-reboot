#pragma once

// First-order sections for the boards' output stages, run at the host rate
// after the band-limited steps (linear stages commute with them).
//
// An RC corner is discretised by the bilinear transform with its frequency
// prewarped, so the digital corner is the analog one at every host rate.
// The Game Boy's coupling capacitor is Pan Docs' own discrete form instead
// (ChargeHighPass), because its constant is what the evidence gives.
// State is double: the lowest corners are a few hertz at 192 kHz.

#include <cmath>

namespace protocodus::reboot {

class FirstOrder {
public:
    enum class Kind { Pass, HighPass, LowPass };

    // Setup, not real time: std::tan is a library call under -fno-builtin.
    void Design(Kind kind, double cornerHz, double sampleRate) {
        kind_ = kind;
        if (kind == Kind::Pass) {
            b0_ = 1.0, b1_ = 0.0, a1_ = 0.0;
            return;
        }
        // A corner at or past Nyquist (the console's 14 kHz at a 22 kHz
        // rate) would turn the prewarp's tangent negative: such a corner is
        // taken just under Nyquist.
        if (cornerHz > 0.45 * sampleRate)
            cornerHz = 0.45 * sampleRate;
        const double k = std::tan(3.141592653589793 * cornerHz / sampleRate);
        a1_ = (k - 1.0) / (k + 1.0);
        if (kind == Kind::HighPass) {
            b0_ = 1.0 / (1.0 + k);
            b1_ = -b0_;
        } else {
            b0_ = k / (1.0 + k);
            b1_ = b0_;
        }
    }

    void Reset() noexcept { x1_ = 0.0, y1_ = 0.0; }

    double Process(double x) noexcept {
        const double y = b0_ * x + b1_ * x1_ - a1_ * y1_;
        x1_ = x;
        y1_ = y;
        return y;
    }

    [[nodiscard]] Kind kind() const noexcept { return kind_; }

private:
    Kind kind_ = Kind::Pass;
    double b0_ = 1.0, b1_ = 0.0, a1_ = 0.0;
    double x1_ = 0.0, y1_ = 0.0;
};

// Pan Docs, Audio details, Obscure Behavior: out = in - capacitor;
// capacitor = in - out * factor, with factor 0.999958 per 4194304 Hz clock on
// the original handheld and 0.998943 on the later ones, raised to
// 4194304 / rate at another rate.
class ChargeHighPass {
public:
    void Design(double factorPerClock, double sampleRate) {
        factor_ = std::pow(factorPerClock, 4194304.0 / sampleRate);
    }
    void Reset() noexcept { capacitor_ = 0.0; }
    double Process(double in) noexcept {
        const double out = in - capacitor_;
        capacitor_ = in - out * factor_;
        return out;
    }
    [[nodiscard]] double factor() const noexcept { return factor_; }

private:
    double factor_ = 1.0;
    double capacitor_ = 0.0;
};

} // namespace protocodus::reboot
