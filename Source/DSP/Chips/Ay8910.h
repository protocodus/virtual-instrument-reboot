#pragma once

// The AY-3-8910 / AY-3-8912 and Yamaha's YM2149: three square tones, one
// noise generator and one envelope generator shared by the three channels.
// The Sunsoft 5B cartridge chip is this core too, clocked at half the CPU
// clock (Chips/Famicom.h).
//
// Sources: General Instrument's AY-3-8910 data manual (register map, mixer,
// envelope shapes); Ayumi (Peter Sovietov, MIT licence) for the counters'
// rule (count up each tick of clock/8, toggle and restart at the period, a
// period of 0 counting as 1), the noise register (17 bits, bit 0 XOR bit 3
// into bit 16, starting at 1, shifted every second noise period) and the
// envelope as 32 steps per segment; MAME's ay8910.cpp (BSD-3-Clause) agrees
// on the noise and the mixer, (tone | tone off) & (noise | noise off). No
// code of either is copied.
//
// The envelope always runs as the YM2149's 32 steps. The AY-3-8910 has 16,
// each twice as long; its DAC gives each pair of the 32 codes one level
// (the data manual), which is the same output.
//
// Out() gives each channel's 5-bit DAC code: a fixed volume v is code
// 2v + 1, the envelope its step, and a gated-off channel code 0.

#include "Lfsr.h"
#include "Timer.h"

#include <cstdint>

namespace protocodus::reboot::ay {


constexpr std::uint32_t NoiseStep(std::uint32_t s) { return (s >> 1) | (((s ^ (s >> 3)) & 1u) << 16); }
inline constexpr LfsrJump kNoiseJump { NoiseStep };

// The data manual's envelope shapes as Ayumi tabulates them: what each of a
// shape's two segments does (shapes 0-7 behave as 9 and 15).
enum Segment : std::uint8_t { kDown, kUp, kHoldBottom, kHoldTop };
constexpr Segment kShapes[16][2] = {
    { kDown, kHoldBottom }, { kDown, kHoldBottom }, { kDown, kHoldBottom }, { kDown, kHoldBottom },
    { kUp, kHoldBottom }, { kUp, kHoldBottom }, { kUp, kHoldBottom }, { kUp, kHoldBottom },
    { kDown, kDown }, { kDown, kHoldBottom }, { kDown, kUp }, { kDown, kHoldTop },
    { kUp, kUp }, { kUp, kHoldTop }, { kUp, kDown }, { kUp, kHoldBottom },
};

class Chip {
public:
    // tick: master cycles per counter tick, 8 for an AY-3-8910 or a YM2149
    // with SEL high, 16 with SEL low (the 5B). quietCodeOne: the DAC gives
    // codes 0 and 1 the same level (the AY-3-8910's and the 5B's do; the
    // YM2149's does not), so a channel at volume 0 with its envelope off
    // cannot be heard.
    void Reset(std::int64_t tick, bool quietCodeOne) noexcept {
        *this = Chip {};
        tick_ = tick;
        quietCodeOne_ = quietCodeOne;
        for (Divider& t : tone_)
            t.period = tick_;
        noise_.period = 2 * tick_;
        envelope_.period = tick_;
        SegmentStart();
        Update();
    }

    [[nodiscard]] std::int64_t Now() const noexcept { return now_; }
    [[nodiscard]] int Out(int channel) const noexcept { return out_[channel]; }
    [[nodiscard]] std::uint8_t Register(int r) const noexcept { return reg_[r]; }

    template <class Sink>
    void Run(std::int64_t to, Sink& sink) {
        for (;;) {
            std::int64_t next = to;
            for (int c = 0; c < 3; ++c)
                if (toneLive_[c])
                    next = Earliest(next, tone_[c].due);
            if (noiseLive_)
                next = Earliest(next, noise_.due);
            if (envelopeLive_)
                next = Earliest(next, envelope_.due);
            if (next >= to)
                break;
            now_ = next;
            for (int c = 0; c < 3; ++c)
                if (toneLive_[c] && tone_[c].due == now_) {
                    flip_[c] ^= 1;
                    tone_[c].due += tone_[c].period;
                }
            if (noiseLive_ && noise_.due == now_) {
                shift_ = NoiseStep(shift_);
                noise_.due += noise_.period;
            }
            if (envelopeLive_ && envelope_.due == now_) {
                EnvelopeStep();
                envelope_.due += envelope_.period;
            }
            if (Update())
                sink(now_);
        }
        now_ = to;
    }

    // A write of `value` to register `r` (0-15) at Now().
    template <class Sink>
    void Write(int r, std::uint8_t value, Sink& sink) {
        CatchUpAll();
        r &= 15;
        reg_[r] = value;
        switch (r) {
        case 0: case 1: case 2: case 3: case 4: case 5: {
            const int c = r >> 1;
            const int period = (reg_[2 * c] | ((reg_[2 * c + 1] & 0x0F) << 8));
            Retime(tone_[c], (period ? period : 1) * tick_);
            break;
        }
        case 6: {
            const int period = reg_[6] & 0x1F;
            Retime(noise_, 2 * (period ? period : 1) * tick_);
            break;
        }
        case 11: case 12: {
            const int period = reg_[11] | (reg_[12] << 8);
            Retime(envelope_, (period ? period : 1) * tick_);
            break;
        }
        case 13:
            // Writing the shape restarts the envelope (Ayumi resets its
            // counter; the data manual starts the shape at once).
            shape_ = value & 0x0F;
            segment_ = 0;
            SegmentStart();
            envelope_.due = TickAfter(now_) + envelope_.period - tick_;
            break;
        default:
            break;
        }
        if (Update())
            sink(now_);
    }

private:
    [[nodiscard]] std::int64_t TickAfter(std::int64_t cycle) const noexcept { return (cycle + tick_ - 1) / tick_ * tick_; }

    // A counter compared with its period: a new period ends the count as
    // soon as the count reaches it, which may be the next tick.
    void Retime(Divider& d, std::int64_t period) noexcept {
        const std::int64_t last = d.due - d.period;
        d.period = period;
        const std::int64_t due = last + period;
        const std::int64_t soonest = TickAfter(now_);
        d.due = due > soonest ? due : soonest;
    }

    void SegmentStart() noexcept {
        const Segment s = kShapes[shape_][segment_];
        level_ = (s == kDown || s == kHoldTop) ? 31 : 0;
    }

    void EnvelopeStep() noexcept {
        switch (kShapes[shape_][segment_]) {
        case kDown:
            if (--level_ < 0) {
                segment_ ^= 1;
                SegmentStart();
            }
            break;
        case kUp:
            if (++level_ > 31) {
                segment_ ^= 1;
                SegmentStart();
            }
            break;
        default:
            break;
        }
    }

    [[nodiscard]] bool EnvelopeMoving() const noexcept {
        const Segment s = kShapes[shape_][segment_];
        return s == kDown || s == kUp;
    }

    void CatchUpTone(int c) noexcept { flip_[c] ^= static_cast<std::uint8_t>(tone_[c].CatchUp(now_) & 1); }
    void CatchUpNoise() noexcept {
        if (const std::int64_t k = noise_.CatchUp(now_))
            shift_ = kNoiseJump.Advance(shift_, static_cast<std::uint64_t>(k));
    }
    void CatchUpEnvelope() noexcept {
        std::int64_t k = envelope_.CatchUp(now_);
        // Every shape repeats within 64 steps once past its first segment,
        // and a one-shot shape holds within 32.
        if (k > 96)
            k = 64 + k % 64;
        for (; k > 0 && EnvelopeMoving(); --k)
            EnvelopeStep();
    }

    void CatchUpAll() noexcept {
        for (int c = 0; c < 3; ++c)
            if (!toneLive_[c])
                CatchUpTone(c);
        if (!noiseLive_)
            CatchUpNoise();
        if (!envelopeLive_)
            CatchUpEnvelope();
    }

    bool Update() noexcept {
        const std::uint8_t mixer = reg_[7];
        bool toneLive[3], noiseLive = false, envelopeLive = false;
        for (int c = 0; c < 3; ++c) {
            const std::uint8_t volume = reg_[8 + c];
            const bool useEnvelope = (volume & 0x10) != 0;
            const bool audible = useEnvelope || (volume & 0x0F) != 0 || !quietCodeOne_;
            toneLive[c] = audible && !(mixer & (1 << c));
            noiseLive |= audible && !(mixer & (8 << c));
            envelopeLive |= useEnvelope;
        }
        envelopeLive &= EnvelopeMoving();
        for (int c = 0; c < 3; ++c) {
            if (toneLive[c] && !toneLive_[c])
                CatchUpTone(c);
            toneLive_[c] = toneLive[c];
        }
        if (noiseLive && !noiseLive_)
            CatchUpNoise();
        noiseLive_ = noiseLive;
        if (envelopeLive && !envelopeLive_)
            CatchUpEnvelope();
        envelopeLive_ = envelopeLive && EnvelopeMoving();

        bool changed = false;
        const int noise = static_cast<int>(shift_ & 1);
        for (int c = 0; c < 3; ++c) {
            const std::uint8_t volume = reg_[8 + c];
            const int gate = (flip_[c] | ((mixer >> c) & 1)) & (noise | ((mixer >> (3 + c)) & 1));
            const int level = (volume & 0x10) ? level_ : 2 * (volume & 0x0F) + 1;
            const int code = gate ? level : 0;
            changed |= code != out_[c];
            out_[c] = static_cast<std::uint8_t>(code);
        }
        return changed;
    }

    std::int64_t tick_ = 8;
    std::int64_t now_ = 0;
    std::uint8_t reg_[16] {};
    Divider tone_[3];
    Divider noise_;
    Divider envelope_;
    std::uint8_t flip_[3] {};
    std::uint32_t shift_ = 1;
    int shape_ = 0, segment_ = 0, level_ = 0;
    bool toneLive_[3] {}, noiseLive_ = false, envelopeLive_ = false;
    bool quietCodeOne_ = true;
    std::uint8_t out_[3] {};
};

} // namespace protocodus::reboot::ay
