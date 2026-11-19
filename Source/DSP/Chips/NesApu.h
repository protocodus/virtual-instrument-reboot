#pragma once

// The 2A03's sound (NTSC consoles) and the 2A07's (PAL): two pulses, a
// triangle, a noise channel and the delta-modulation channel, clocked in CPU
// cycles. Everything here is from the NESdev Wiki's APU pages (APU, APU
// Pulse, APU Triangle, APU Noise, APU DMC, APU Envelope, APU Sweep, APU
// Length Counter, APU Frame Counter), read 2026-11-17; the page each rule
// comes from is named beside it.
//
// The core is integers only and jumps from event to event (a timer's
// reload, a frame-counter step, a register write); it does not visit every
// CPU cycle. A channel that cannot be heard is not visited at all, and its
// timer, sequencer and noise register are caught up exactly when it can be.
//
// Ordering within one CPU cycle: Run(to) processes every event before `to`;
// a write at `to` then applies before the events at `to`.

#include "Lfsr.h"
#include "Timer.h"

#include <cstdint>

namespace protocodus::reboot::nes {

enum class Region { Ntsc, Pal };

// CPU clock: the NTSC master 236.25 MHz / 11 divided by 12, and the PAL
// master 26.6017125 MHz divided by 16 (NESdev, Cycle reference chart).
struct Clock {
    std::int64_t numerator;
    std::int64_t denominator;
};
constexpr Clock kCpuClock[2] = { { 19687500, 11 }, { 53203425, 32 } };

// APU Length Counter: the length load table, indexed by bits 7-3.
constexpr std::uint8_t kLength[32] = {
    10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
};

// APU Pulse: the four duty sequences as the sequencer reads them, step 0
// first; the sequencer counts down (0, 7, 6, ... 1).
constexpr std::uint8_t kDuty[4][8] = {
    { 0, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 1, 1, 0, 0, 0, 0, 0 },
    { 0, 1, 1, 1, 1, 0, 0, 0 },
    { 1, 0, 0, 1, 1, 1, 1, 1 },
};

// APU Triangle: the 32-step sequence.
constexpr std::uint8_t kTriangleSteps[32] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};

// APU Noise: timer periods in CPU cycles, NTSC then PAL.
constexpr std::uint16_t kNoisePeriod[2][16] = {
    { 4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068 },
    { 4, 8, 14, 30, 60, 88, 118, 148, 188, 236, 354, 472, 708, 944, 1890, 3778 },
};

// APU DMC: rate periods in CPU cycles, NTSC then PAL.
constexpr std::uint16_t kDmcPeriod[2][16] = {
    { 428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54 },
    { 398, 354, 316, 298, 276, 236, 210, 198, 176, 148, 132, 118, 98, 78, 66, 50 },
};

// APU Frame Counter: the CPU cycle of each step from the sequence's start,
// whether it clocks the quarter frame (envelopes, triangle linear counter)
// and the half frame (length counters, sweeps), and the sequence's length.
struct FrameSequence {
    std::int32_t at[5];
    bool quarter[5];
    bool half[5];
    int steps;
    std::int32_t length;
};
constexpr FrameSequence kFrame[2][2] = {
    { { { 7457, 14913, 22371, 29829, 0 }, { true, true, true, true, false }, { false, true, false, true, false }, 4, 29830 },
      { { 7457, 14913, 22371, 29829, 37281 }, { true, true, true, false, true }, { false, true, false, false, true }, 5, 37282 } },
    { { { 8313, 16627, 24939, 33253, 0 }, { true, true, true, true, false }, { false, true, false, true, false }, 4, 33254 },
      { { 8313, 16627, 24939, 33253, 41565 }, { true, true, true, false, true }, { false, true, false, false, true }, 5, 41566 } },
};

// APU Noise: feedback is bit 0 XOR bit 1, or bit 6 in mode 1, into bit 14.
constexpr std::uint32_t NoiseStepLong(std::uint32_t s) { return (s >> 1) | (((s ^ (s >> 1)) & 1u) << 14); }
constexpr std::uint32_t NoiseStepShort(std::uint32_t s) { return (s >> 1) | (((s ^ (s >> 6)) & 1u) << 14); }
inline constexpr LfsrJump kNoiseJump[2] = { LfsrJump(NoiseStepLong), LfsrJump(NoiseStepShort) };

class Apu {
public:
    enum Channel { kPulse1, kPulse2, kTriangle, kNoise, kDmc, kChannels };

    void Reset(Region region) noexcept {
        const std::uint8_t* rom = rom_;
        *this = Apu {};
        rom_ = rom;
        pulse_[0].negateOnes = true;
        region_ = static_cast<int>(region);
        noise_.timer.period = kNoisePeriod[region_][0];
        dmc_.timer.period = kDmcPeriod[region_][0];
        frameDue_ = kFrame[region_][0].at[0];
        Update();
    }

    // The DMC's memory, CPU $8000-$FFFF; nullptr reads zeros.
    void SetMemory(const std::uint8_t* rom32k) noexcept { rom_ = rom32k; }

    [[nodiscard]] std::int64_t Now() const noexcept { return now_; }
    [[nodiscard]] int Out(int channel) const noexcept { return out_[channel]; }
    [[nodiscard]] int RegionIndex() const noexcept { return region_; }

    // A CPU read of $4015 at Now() (NESdev, APU Status).
    [[nodiscard]] std::uint8_t ReadStatus() noexcept {
        CatchUpAll();
        return static_cast<std::uint8_t>((pulse_[0].length > 0 ? 1 : 0) | (pulse_[1].length > 0 ? 2 : 0)
                                         | (triangle_.length > 0 ? 4 : 0) | (noise_.length > 0 ? 8 : 0)
                                         | (dmc_.bytesLeft > 0 ? 16 : 0));
    }

    // Processes every event before `to`; sink(cycle) follows any change of
    // the output codes.
    template <class Sink>
    void Run(std::int64_t to, Sink& sink) {
        for (;;) {
            std::int64_t next = Earliest(to, Earliest(frameDue_, resetDue_));
            for (int i = 0; i < 2; ++i)
                if (pulse_[i].live)
                    next = Earliest(next, pulse_[i].timer.due);
            if (triangle_.live)
                next = Earliest(next, triangle_.timer.due);
            if (noise_.live)
                next = Earliest(next, noise_.timer.due);
            if (dmc_.live)
                next = Earliest(next, dmc_.timer.due);
            if (next >= to)
                break;
            now_ = next;
            if (resetDue_ == now_)
                FrameReset();
            if (frameDue_ == now_)
                FrameStep();
            for (int i = 0; i < 2; ++i) {
                Pulse& p = pulse_[i];
                if (p.live && p.timer.due == now_) {
                    p.step = (p.step - 1) & 7;
                    p.timer.due += p.timer.period;
                }
            }
            if (triangle_.live && triangle_.timer.due == now_) {
                triangle_.step = (triangle_.step + 1) & 31;
                triangle_.timer.due += triangle_.timer.period;
            }
            if (noise_.live && noise_.timer.due == now_) {
                noise_.shift = noise_.mode ? NoiseStepShort(noise_.shift) : NoiseStepLong(noise_.shift);
                noise_.timer.due += noise_.timer.period;
            }
            if (dmc_.live && dmc_.timer.due == now_) {
                DmcClock();
                dmc_.timer.due += dmc_.timer.period;
            }
            if (Update())
                sink(now_);
        }
        now_ = to;
    }

    // A CPU write to $4000-$4017 at Now().
    template <class Sink>
    void Write(std::uint16_t address, std::uint8_t value, Sink& sink) {
        CatchUpAll();
        switch (address) {
        case 0x4000: case 0x4004: {
            Pulse& p = pulse_[(address >> 2) & 1];
            p.duty = value >> 6;
            p.halt = (value & 0x20) != 0;
            p.envelope.constant = (value & 0x10) != 0;
            p.envelope.volume = value & 0x0F;
            break;
        }
        case 0x4001: case 0x4005: {
            Pulse& p = pulse_[(address >> 2) & 1];
            p.sweepEnabled = (value & 0x80) != 0;
            p.sweepPeriod = (value >> 4) & 7;
            p.sweepNegate = (value & 0x08) != 0;
            p.sweepShift = value & 7;
            p.sweepReload = true;
            break;
        }
        case 0x4002: case 0x4006: {
            Pulse& p = pulse_[(address >> 2) & 1];
            p.SetPeriod((p.period & 0x700) | value);
            break;
        }
        case 0x4003: case 0x4007: {
            // APU Pulse: the sequencer restarts and the envelope restarts;
            // the timer's divider is not reset.
            Pulse& p = pulse_[(address >> 2) & 1];
            p.SetPeriod((p.period & 0xFF) | ((value & 7) << 8));
            if (enabled_ & (1 << ((address >> 2) & 1)))
                p.length = kLength[value >> 3];
            p.step = 0;
            p.envelope.start = true;
            break;
        }
        case 0x4008:
            triangle_.control = (value & 0x80) != 0;
            triangle_.linearLoad = value & 0x7F;
            break;
        case 0x400A:
            triangle_.SetPeriod((triangle_.period & 0x700) | value);
            break;
        case 0x400B:
            triangle_.SetPeriod((triangle_.period & 0xFF) | ((value & 7) << 8));
            if (enabled_ & 4)
                triangle_.length = kLength[value >> 3];
            triangle_.linearReload = true;
            break;
        case 0x400C:
            noise_.halt = (value & 0x20) != 0;
            noise_.envelope.constant = (value & 0x10) != 0;
            noise_.envelope.volume = value & 0x0F;
            break;
        case 0x400E:
            noise_.mode = (value & 0x80) != 0;
            noise_.timer.period = kNoisePeriod[region_][value & 0x0F];
            break;
        case 0x400F:
            if (enabled_ & 8)
                noise_.length = kLength[value >> 3];
            noise_.envelope.start = true;
            break;
        case 0x4010:
            dmc_.loop = (value & 0x40) != 0;
            dmc_.timer.period = kDmcPeriod[region_][value & 0x0F];
            break;
        case 0x4011:
            dmc_.level = value & 0x7F;
            break;
        case 0x4012:
            dmc_.sampleAddress = static_cast<std::uint16_t>(0xC000 + value * 64);
            break;
        case 0x4013:
            dmc_.sampleLength = static_cast<std::uint16_t>(value * 16 + 1);
            break;
        case 0x4015:
            enabled_ = value & 0x1F;
            if (!(value & 1)) pulse_[0].length = 0;
            if (!(value & 2)) pulse_[1].length = 0;
            if (!(value & 4)) triangle_.length = 0;
            if (!(value & 8)) noise_.length = 0;
            if (!(value & 16)) {
                dmc_.bytesLeft = 0;
            } else if (dmc_.bytesLeft == 0) {
                DmcRestart();
                DmcFill();
            }
            break;
        case 0x4017:
            // APU Frame Counter: the reset follows 3 CPU cycles after a
            // write during an APU cycle, 4 after one between them; APU
            // cycles are the even CPU cycles here.
            frameMode_ = (value & 0x80) ? 1 : 0;
            resetDue_ = now_ + ((now_ & 1) == 0 ? 3 : 4);
            break;
        default:
            break;
        }
        if (Update())
            sink(now_);
    }

private:
    struct Envelope {
        bool start = false, constant = false;
        std::uint8_t volume = 0, divider = 0, decay = 0;
        [[nodiscard]] int Level() const noexcept { return constant ? volume : decay; }
        // APU Envelope.
        void Clock(bool loop) noexcept {
            if (start) {
                start = false;
                decay = 15;
                divider = volume;
            } else if (divider == 0) {
                divider = volume;
                if (decay > 0)
                    --decay;
                else if (loop)
                    decay = 15;
            } else {
                --divider;
            }
        }
    };

    struct Pulse {
        bool negateOnes = false;           // pulse 1 negates by ones' complement
        std::uint8_t duty = 0, step = 0, length = 0;
        bool halt = false;
        Envelope envelope;
        bool sweepEnabled = false, sweepNegate = false, sweepReload = false;
        std::uint8_t sweepPeriod = 0, sweepShift = 0, sweepDivider = 0;
        std::uint16_t period = 0;
        Divider timer { 0, 2 };
        bool live = false;

        void SetPeriod(int t) noexcept {
            period = static_cast<std::uint16_t>(t);
            timer.period = 2 * (t + 1);        // clocked every APU cycle
        }
        // APU Sweep: the target period, computed continuously.
        [[nodiscard]] int Target() const noexcept {
            const int change = period >> sweepShift;
            if (!sweepNegate)
                return period + change;
            return period - change - (negateOnes ? 1 : 0);
        }
        [[nodiscard]] bool Muted() const noexcept { return period < 8 || Target() > 0x7FF; }
        void SweepClock() noexcept {
            if (sweepDivider == 0 && sweepEnabled && sweepShift != 0 && !Muted()) {
                const int target = Target();
                SetPeriod(target < 0 ? 0 : target);
            }
            if (sweepDivider == 0 || sweepReload) {
                sweepDivider = sweepPeriod;
                sweepReload = false;
            } else {
                --sweepDivider;
            }
        }
    };

    struct Triangle {
        bool control = false, linearReload = false;
        std::uint8_t linearLoad = 0, linear = 0, length = 0, step = 0;
        std::uint16_t period = 0;
        Divider timer { 0, 1 };
        bool live = false;
        void SetPeriod(int t) noexcept {
            period = static_cast<std::uint16_t>(t);
            timer.period = t + 1;              // clocked every CPU cycle
        }
    };

    struct Noise {
        bool halt = false, mode = false;
        Envelope envelope;
        std::uint8_t length = 0;
        std::uint32_t shift = 1;
        Divider timer { 0, 4 };
        bool live = false;
    };

    struct Dmc {
        bool loop = false, silence = true, bufferFull = false;
        std::uint8_t level = 0, shifter = 0, bits = 8, buffer = 0;
        std::uint16_t sampleAddress = 0xC000, sampleLength = 1, address = 0xC000, bytesLeft = 0;
        Divider timer { 0, 428 };
        bool live = false;
    };

    void CatchUpAll() noexcept {
        for (Pulse& p : pulse_)
            if (!p.live)
                p.step = static_cast<std::uint8_t>((p.step - p.timer.CatchUp(now_)) & 7);
        if (!triangle_.live)
            triangle_.timer.CatchUp(now_);
        if (!noise_.live) {
            const std::int64_t k = noise_.timer.CatchUp(now_);
            if (k)
                noise_.shift = kNoiseJump[noise_.mode ? 1 : 0].Advance(noise_.shift, static_cast<std::uint64_t>(k));
        }
        if (!dmc_.live) {
            // Silent with nothing to read: only the output cycle's bit
            // counter moves (APU DMC, output unit).
            const std::int64_t k = dmc_.timer.CatchUp(now_);
            dmc_.bits = static_cast<std::uint8_t>(8 - (8 - dmc_.bits + k) % 8);
        }
    }

    void FrameReset() noexcept {
        resetDue_ = kNever;
        CatchUpAll();
        frameStart_ = now_;
        frameIndex_ = 0;
        frameDue_ = frameStart_ + kFrame[region_][frameMode_].at[0];
        if (frameMode_ == 1) {
            QuarterFrame();
            HalfFrame();
        }
    }

    void FrameStep() noexcept {
        CatchUpAll();
        const FrameSequence& f = kFrame[region_][frameMode_];
        if (f.quarter[frameIndex_])
            QuarterFrame();
        if (f.half[frameIndex_])
            HalfFrame();
        if (++frameIndex_ == f.steps) {
            frameIndex_ = 0;
            frameStart_ += f.length;
        }
        frameDue_ = frameStart_ + f.at[frameIndex_];
    }

    void QuarterFrame() noexcept {
        pulse_[0].envelope.Clock(pulse_[0].halt);
        pulse_[1].envelope.Clock(pulse_[1].halt);
        noise_.envelope.Clock(noise_.halt);
        // APU Triangle: the linear counter.
        if (triangle_.linearReload)
            triangle_.linear = triangle_.linearLoad;
        else if (triangle_.linear > 0)
            --triangle_.linear;
        if (!triangle_.control)
            triangle_.linearReload = false;
    }

    void HalfFrame() noexcept {
        for (Pulse& p : pulse_) {
            if (!p.halt && p.length > 0)
                --p.length;
            p.SweepClock();
        }
        if (!triangle_.control && triangle_.length > 0)
            --triangle_.length;
        if (!noise_.halt && noise_.length > 0)
            --noise_.length;
    }

    void DmcRestart() noexcept {
        dmc_.address = dmc_.sampleAddress;
        dmc_.bytesLeft = dmc_.sampleLength;
    }

    // APU DMC, memory reader: fills an empty buffer from the sample.
    void DmcFill() noexcept {
        if (dmc_.bufferFull || dmc_.bytesLeft == 0)
            return;
        dmc_.buffer = rom_ ? rom_[dmc_.address & 0x7FFF] : 0;
        dmc_.bufferFull = true;
        dmc_.address = dmc_.address == 0xFFFF ? 0x8000 : static_cast<std::uint16_t>(dmc_.address + 1);
        if (--dmc_.bytesLeft == 0 && dmc_.loop)
            DmcRestart();
    }

    // APU DMC, output unit: one timer clock.
    void DmcClock() noexcept {
        if (!dmc_.silence) {
            if (dmc_.shifter & 1) {
                if (dmc_.level <= 125)
                    dmc_.level += 2;
            } else if (dmc_.level >= 2) {
                dmc_.level -= 2;
            }
            dmc_.shifter >>= 1;
        }
        if (--dmc_.bits == 0) {
            dmc_.bits = 8;
            if (dmc_.bufferFull) {
                dmc_.silence = false;
                dmc_.shifter = dmc_.buffer;
                dmc_.bufferFull = false;
                DmcFill();
            } else {
                dmc_.silence = true;
            }
        }
    }

    // Recomputes the output codes and which channels can change them;
    // true when a code changed. A channel that wakes is caught up first.
    bool Update() noexcept {
        int codes[kChannels];
        for (int i = 0; i < 2; ++i) {
            Pulse& p = pulse_[i];
            const int level = p.envelope.Level();
            const bool live = !p.Muted() && p.length > 0 && level > 0;
            if (live && !p.live)
                p.step = static_cast<std::uint8_t>((p.step - p.timer.CatchUp(now_)) & 7);
            p.live = live;
            codes[i] = live && kDuty[p.duty][p.step] ? level : 0;
        }
        {
            const bool live = triangle_.linear > 0 && triangle_.length > 0;
            if (live && !triangle_.live)
                triangle_.timer.CatchUp(now_);
            triangle_.live = live;
            codes[kTriangle] = kTriangleSteps[triangle_.step];
        }
        {
            const int level = noise_.envelope.Level();
            const bool live = noise_.length > 0 && level > 0;
            if (live && !noise_.live) {
                const std::int64_t k = noise_.timer.CatchUp(now_);
                if (k)
                    noise_.shift = kNoiseJump[noise_.mode ? 1 : 0].Advance(noise_.shift, static_cast<std::uint64_t>(k));
            }
            noise_.live = live;
            codes[kNoise] = live && !(noise_.shift & 1) ? level : 0;
        }
        {
            const bool live = !dmc_.silence || dmc_.bufferFull || dmc_.bytesLeft > 0;
            if (live && !dmc_.live) {
                const std::int64_t k = dmc_.timer.CatchUp(now_);
                dmc_.bits = static_cast<std::uint8_t>(8 - (8 - dmc_.bits + k) % 8);
            }
            dmc_.live = live;
            codes[kDmc] = dmc_.level;
        }
        bool changed = false;
        for (int i = 0; i < kChannels; ++i) {
            changed |= codes[i] != out_[i];
            out_[i] = static_cast<std::uint8_t>(codes[i]);
        }
        return changed;
    }

    static constexpr std::int64_t kNever = INT64_MAX;

    int region_ = 0;
    std::int64_t now_ = 0;
    Pulse pulse_[2];
    Triangle triangle_;
    Noise noise_;
    Dmc dmc_;
    std::uint8_t enabled_ = 0;
    int frameMode_ = 0, frameIndex_ = 0;
    std::int64_t frameStart_ = 0, frameDue_ = 0, resetDue_ = kNever;
    const std::uint8_t* rom_ = nullptr;
    std::uint8_t out_[kChannels] {};
};

} // namespace protocodus::reboot::nes
