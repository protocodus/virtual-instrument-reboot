#pragma once

// Jumping a noise generator's shift register ahead by any number of steps.
//
// Every noise generator here steps an XOR (or XNOR) of taps into a shift
// register: a linear, or affine, map over GF(2). k steps are that map's
// matrix raised to the k-th power, which the squares below assemble in
// O(log k). The cores use it while a noise channel cannot be heard (its
// volume or length at zero), so that its register is exactly where the
// hardware's would be when it is heard again, without stepping it a few
// hundred thousand times a second in silence.
//
// A state is up to 31 bits; bit 31 stands for the constant 1, so an XNOR's
// inversion is the image of that bit. Tests/ChipTests.cpp checks every jump
// against stepping one at a time.

#include <cstdint>

namespace protocodus::reboot {

using LfsrStep = std::uint32_t (*)(std::uint32_t);

class LfsrJump {
public:
    static constexpr int kPowers = 40;
    static constexpr std::uint32_t kOne = 0x80000000u;

    // The map's square powers, from the step on the basis states. The step
    // must be affine: Step(a ^ b) == Step(a) ^ Step(b) ^ Step(0).
    explicit constexpr LfsrJump(LfsrStep step) : m_ {} {
        const std::uint32_t constant = step(0);
        for (int bit = 0; bit < 31; ++bit)
            m_[0][bit] = step(std::uint32_t { 1 } << bit) ^ constant;
        m_[0][31] = constant | kOne;
        for (int p = 1; p < kPowers; ++p)
            for (int bit = 0; bit < 32; ++bit)
                m_[p][bit] = Apply(m_[p - 1], m_[p - 1][bit]);
    }

    // The state after `steps` steps from `state` (bits 0..30).
    [[nodiscard]] constexpr std::uint32_t Advance(std::uint32_t state, std::uint64_t steps) const {
        std::uint32_t s = state | kOne;
        while (steps >= (std::uint64_t { 1 } << kPowers)) {
            s = Apply(m_[kPowers - 1], s);
            s = Apply(m_[kPowers - 1], s);
            steps -= std::uint64_t { 1 } << kPowers;
        }
        for (int p = 0; steps != 0; ++p, steps >>= 1)
            if (steps & 1)
                s = Apply(m_[p], s);
        return s & ~kOne;
    }

private:
    static constexpr std::uint32_t Apply(const std::uint32_t (&columns)[32], std::uint32_t s) {
        std::uint32_t out = 0;
        for (int bit = 0; s != 0; ++bit, s >>= 1)
            if (s & 1)
                out ^= columns[bit];
        return out;
    }

    std::uint32_t m_[kPowers][32];
};

} // namespace protocodus::reboot
