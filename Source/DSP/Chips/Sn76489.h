#pragma once

// The SN76489 family: three square tones and a noise generator. Two
// variants: the PSG inside Sega's video chips (Master System, Game Gear: a
// 16-bit noise register tapped at bits 0 and 3) and Texas Instruments'
// SN76489 (a 15-bit register tapped at bits 0 and 1, output inverted).
//
// Sources: SMS Power, "SN76489" (Maxim's notes, read 2026-11-17) for the
// register interface, counters and noise; MAME's sn76496.cpp (BSD-3-Clause,
// Nicola Salmoria) for the per-variant register widths, taps, reset state
// and the counter rule (a counter of 0 toggles on every clock); nothing of
// MAME's code is copied.
//
// Clocked in master cycles; the counters move every 16th (the chip's
// internal divider). Out() is each channel's output bit, Attenuation() its
// 4-bit attenuation (15 = off); the board's DAC turns those into levels.

#include "Lfsr.h"
#include "Timer.h"

#include <cstdint>

namespace protocodus::reboot::psg {

enum class Variant { Sega, Ti };

struct Taps {
    std::uint32_t feedback, tap1, tap2;
    bool inverted, segaReset;
};
constexpr Taps kTaps[2] = {
    { 0x8000, 0x01, 0x08, true, true },     // Sega PSG
    { 0x4000, 0x01, 0x02, true, false },    // SN76489
};

constexpr std::uint32_t Shift(std::uint32_t s, const Taps& t, bool white) {
    const bool fb = ((s & t.tap1) != 0) != (white && (s & t.tap2) != 0);
    return (s >> 1) | (fb ? t.feedback : 0u);
}
constexpr std::uint32_t SegaWhite(std::uint32_t s) { return Shift(s, kTaps[0], true); }
constexpr std::uint32_t SegaPeriodic(std::uint32_t s) { return Shift(s, kTaps[0], false); }
constexpr std::uint32_t TiWhite(std::uint32_t s) { return Shift(s, kTaps[1], true); }
constexpr std::uint32_t TiPeriodic(std::uint32_t s) { return Shift(s, kTaps[1], false); }
inline constexpr LfsrJump kJump[2][2] = {
    { LfsrJump(SegaPeriodic), LfsrJump(SegaWhite) },
    { LfsrJump(TiPeriodic), LfsrJump(TiWhite) },
};

constexpr std::int64_t kDivider = 16;

class Chip {
public:
    enum Channel { kTone1, kTone2, kTone3, kNoise, kChannels };

    void Reset(Variant variant) noexcept {
        *this = Chip {};
        variant_ = static_cast<int>(variant);
        const Taps& t = kTaps[variant_];
        // MAME: the Sega PSG starts with every channel off and register 3
        // latched; the TI part at full volume with register 0 latched, its
        // tone periods at 0x400 (a written 0 means 0x400 there).
        for (int c = 0; c < 4; ++c) {
            attenuation_[c] = t.segaReset ? 15 : 0;
            reg_[2 * c + 1] = attenuation_[c];
        }
        latched_ = t.segaReset ? 3 : 0;
        for (int c = 0; c < 3; ++c)
            SetTonePeriod(c, 0);
        noise_.period = PeriodCycles(32);
        shift_ = t.feedback;
        Update();
    }

    [[nodiscard]] std::int64_t Now() const noexcept { return now_; }
    [[nodiscard]] int Out(int channel) const noexcept { return out_[channel]; }
    [[nodiscard]] int Attenuation(int channel) const noexcept { return attenuation_[channel]; }
    [[nodiscard]] bool Inverted() const noexcept { return kTaps[variant_].inverted; }
    [[nodiscard]] std::uint8_t Stereo() const noexcept { return stereo_; }

    template <class Sink>
    void Run(std::int64_t to, Sink& sink) {
        for (;;) {
            std::int64_t next = to;
            for (int c = 0; c < 3; ++c)
                if (live_[c])
                    next = Earliest(next, tone_[c].due);
            if (live_[kNoise])
                next = Earliest(next, noise_.due);
            if (next >= to)
                break;
            now_ = next;
            for (int c = 0; c < 3; ++c)
                if (live_[c] && tone_[c].due == now_) {
                    flip_[c] ^= 1;
                    tone_[c].due += tone_[c].period;
                }
            if (live_[kNoise] && noise_.due == now_) {
                shift_ = Shift(shift_, kTaps[variant_], White());
                noise_.due += noise_.period;
            }
            if (Update())
                sink(now_);
        }
        now_ = to;
    }

    // A byte written to the PSG's port at Now().
    template <class Sink>
    void Write(std::uint8_t data, Sink& sink) {
        CatchUpAll();
        int r;
        if (data & 0x80) {
            r = (data >> 4) & 7;
            latched_ = r;
            reg_[r] = static_cast<std::uint16_t>((reg_[r] & 0x3F0) | (data & 0x0F));
        } else {
            r = latched_;
            // SMS Power: a data byte fills a tone's high six bits, or the
            // low four of any other register.
            if (r == 0 || r == 2 || r == 4)
                reg_[r] = static_cast<std::uint16_t>((reg_[r] & 0x0F) | ((data & 0x3F) << 4));
            else
                reg_[r] = static_cast<std::uint16_t>((reg_[r] & 0x3F0) | (data & 0x0F));
        }
        switch (r) {
        case 0: case 2: case 4:
            SetTonePeriod(r >> 1, reg_[r]);
            if (r == 4 && (reg_[6] & 3) == 3)
                noise_.period = PeriodCycles(2 * ticks_[2]);
            break;
        case 1: case 3: case 5: case 7:
            attenuation_[r >> 1] = reg_[r] & 0x0F;
            break;
        case 6: {
            const int n = reg_[6] & 3;
            noise_.period = PeriodCycles(n == 3 ? 2 * ticks_[2] : 32 << n);
            // Any write to the noise register resets the shift register.
            shift_ = kTaps[variant_].feedback;
            break;
        }
        default:
            break;
        }
        if (Update())
            sink(now_);
    }

    // The Game Gear's stereo port: bits 7-4 send noise and tones 3-1 left,
    // bits 3-0 right.
    template <class Sink>
    void WriteStereo(std::uint8_t data, Sink& sink) {
        CatchUpAll();
        stereo_ = data;
        if (Update())
            sink(now_);
    }

private:
    [[nodiscard]] bool White() const noexcept { return (reg_[6] & 4) != 0; }
    static std::int64_t PeriodCycles(int ticks) noexcept { return kDivider * (ticks < 1 ? 1 : ticks); }

    void SetTonePeriod(int c, int value) noexcept {
        // MAME: a period of 0 toggles every clock on the Sega PSG and counts
        // 0x400 on the TI part.
        ticks_[c] = value != 0 || kTaps[variant_].segaReset ? value : 0x400;
        tone_[c].period = PeriodCycles(ticks_[c]);
    }

    void CatchUpAll() noexcept {
        for (int c = 0; c < 3; ++c)
            if (!live_[c])
                flip_[c] ^= static_cast<std::uint8_t>(tone_[c].CatchUp(now_) & 1);
        if (!live_[kNoise])
            if (const std::int64_t k = noise_.CatchUp(now_))
                shift_ = kJump[variant_][White() ? 1 : 0].Advance(shift_, static_cast<std::uint64_t>(k));
    }

    bool Update() noexcept {
        bool changed = false;
        for (int c = 0; c < kChannels; ++c) {
            const bool live = attenuation_[c] != 15;
            if (live && !live_[c]) {
                if (c < 3)
                    flip_[c] ^= static_cast<std::uint8_t>(tone_[c].CatchUp(now_) & 1);
                else if (const std::int64_t k = noise_.CatchUp(now_))
                    shift_ = kJump[variant_][White() ? 1 : 0].Advance(shift_, static_cast<std::uint64_t>(k));
            }
            live_[c] = live;
            const int bit = c < 3 ? flip_[c] : static_cast<int>(shift_ & 1);
            const int code = live ? bit : 0;
            changed |= code != out_[c] || attenuation_[c] != shownAttenuation_[c];
            out_[c] = static_cast<std::uint8_t>(code);
            shownAttenuation_[c] = attenuation_[c];
        }
        changed |= stereo_ != shownStereo_;
        shownStereo_ = stereo_;
        return changed;
    }

    int variant_ = 0;
    std::int64_t now_ = 0;
    std::uint16_t reg_[8] {};
    int latched_ = 0;
    Divider tone_[3];
    int ticks_[3] {};
    Divider noise_;
    std::uint8_t flip_[3] {};
    std::uint32_t shift_ = 0;
    std::uint8_t attenuation_[kChannels] {}, shownAttenuation_[kChannels] {};
    std::uint8_t out_[kChannels] {};
    bool live_[kChannels] {};
    std::uint8_t stereo_ = 0xFF, shownStereo_ = 0xFF;
};

} // namespace protocodus::reboot::psg
