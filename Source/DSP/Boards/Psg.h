#pragma once

// The PSG board: the SN76489 family (Chips/Sn76489.h) on a 3579545 Hz clock
// and its volume DAC.
//
// Revisions: the Sega console (the PSG in its video chip, mono), the Sega
// handheld (the same PSG with its stereo register at port $06) and the TI
// computer (Texas Instruments' SN76489: 15-bit noise register).
//
// DAC (SMS Power, "SN76489", read 2026-11-17; reported, and disputed for
// Sega's consoles: README, Known gaps): each channel's output bit, 0 or +1,
// times its volume, 2 dB down a step of attenuation, 15 silent; the four
// summed. MAME's sn76496.cpp negates the whole output of these parts, as
// Inverted() reports. No line stage is known for any of the three, so Line
// renders as Pin (README, Known gaps).
//
// Below the tones' lowest note (A2 - 10 cents, at a period of 1023) the
// board's bass is the chip's own trick: periodic noise clocked by tone 3
// at tone 3's rate, tone 3 itself silent. A 16-bit register (Sega) cycles
// every 16 shifts, a 15-bit one (TI) every 15, down to 6.8 Hz (SMS Power).

#include "../Chips/Sn76489.h"
#include "Board.h"

#include <cstdint>

namespace protocodus::reboot {

class Psg {
public:
    enum Revision { kSegaConsole, kSegaHandheld, kTiComputer, kRevisions };
    static constexpr int kChannels = psg::Chip::kChannels;
    static constexpr std::int64_t kClock = 3579545;
    static constexpr std::int64_t kFrameCycles = 228 * 262;   // NTSC video: 59.92 Hz

    void Prepare(int sampleRate) {
        rate_ = sampleRate;
        DesignPinBlocker(pinBlocker_[0], sampleRate);
        DesignPinBlocker(pinBlocker_[1], sampleRate);
    }

    void Reset(int revision) noexcept {
        revision_ = revision;
        psg_.Reset(revision == kTiComputer ? psg::Variant::Ti : psg::Variant::Sega);
        tb_.Set(kClock, 1, rate_);
        mix_.Clear();
        pinBlocker_[0].Reset();
        pinBlocker_[1].Reset();
        for (Shadow& s : shadow_)
            s = Shadow {};
        Sink sink { this };
        for (int c = 0; c < kChannels; ++c)
            psg_.Write(static_cast<std::uint8_t>(0x90 | (c << 5) | 15), sink);
        if (revision == kSegaHandheld)
            psg_.WriteStereo(0xFF, sink);
    }

    static constexpr Voice VoiceOf(int channel) noexcept { return channel < 3 ? Voice::Tone : Voice::Noise; }

    [[nodiscard]] std::int64_t CycleAt(int frame) const noexcept { return tb_.CycleAt(frame); }

    void RunTo(std::int64_t cycle) {
        Sink sink { this };
        psg_.Run(cycle, sink);
    }

    // Tones: the 10-bit period N of clock / (32 N). Noise: the rate code
    // (0-2) whose shift rate, clock / (512 << n), is nearest `hz` times 32
    // (ours: the key picks the noise's colour), or with shape 1 nearest `hz`
    // times the periodic cycle. Bass: tone 3's period for a periodic cycle.
    [[nodiscard]] bool PeriodFor(int channel, Voice voice, double hz, int shape, std::int64_t& period) const noexcept {
        if (hz <= 0.0)
            return false;
        const double clock = static_cast<double>(kClock);
        if (channel < 3) {
            period = NearestCount(clock / (32.0 * hz));
            return period >= 1 && period <= 1023;
        }
        const double cycle = revision_ == kTiComputer ? 15.0 : 16.0;
        if (voice == Voice::Bass) {
            period = NearestCount(clock / (32.0 * cycle * hz));
            return period >= 1 && period <= 1023;
        }
        const double target = hz * (shape ? cycle : 32.0);
        int best = 0;
        double bestRatio = 0.0;
        for (int n = 0; n < 3; ++n) {
            const double rate = clock / static_cast<double>(512 << n);
            const double ratio = rate > target ? rate / target : target / rate;
            if (bestRatio == 0.0 || ratio < bestRatio)
                bestRatio = ratio, best = n;
        }
        period = best;
        return true;
    }

    void Start(int channel, const ChannelSettings& s) { Write(channel, s, true); }
    void Change(int channel, const ChannelSettings& s) { Write(channel, s, false); }
    void Stop(int channel) {
        ChannelSettings s;
        s.voice = shadow_[channel].voice;
        s.period = shadow_[channel].period;
        s.shape = shadow_[channel].shape;
        Write(channel, s, false);
    }

    void Render(int frames, Path, float* left, float* right) {
        RunTo(tb_.CycleAt(frames));
        float bus[2][StepBuffer::kMaxFrames] {};
        mix_.Read(0, bus[0], frames);
        mix_.Read(1, bus[1], frames);
        for (int i = 0; i < frames; ++i) {
            left[i] += static_cast<float>(pinBlocker_[0].Process(bus[0][i]));
            right[i] += static_cast<float>(pinBlocker_[1].Process(bus[1][i]));
        }
        tb_.Advance(frames);
    }

    [[nodiscard]] const psg::Chip& Chip() const noexcept { return psg_; }

private:
    struct Shadow {
        bool active = false;
        Voice voice = Voice::Tone;
        std::int64_t period = -1;
        int volume = -1;
        int shape = -1;
    };

    struct Sink {
        Psg* board;
        void operator()(std::int64_t cycle) const { board->Changed(cycle); }
    };

    static int Clamp15(int v) noexcept { return v < 0 ? 0 : v > 15 ? 15 : v; }

    void TonePeriod(int c, std::int64_t period, Sink& sink) {
        psg_.Write(static_cast<std::uint8_t>(0x80 | (c << 5) | (period & 15)), sink);
        psg_.Write(static_cast<std::uint8_t>((period >> 4) & 0x3F), sink);
    }

    void Attenuate(int c, int volume, Sink& sink) {
        psg_.Write(static_cast<std::uint8_t>(0x90 | (c << 5) | (15 - volume)), sink);
    }

    void Write(int channel, const ChannelSettings& s, bool start) {
        Shadow& sh = shadow_[channel];
        const bool trigger = start || !sh.active || s.voice != sh.voice;
        const int volume = Clamp15(s.volume);
        Sink sink { this };
        if (channel < 3) {
            if (trigger || s.period != sh.period)
                TonePeriod(channel, s.period, sink);
            if (trigger || volume != sh.volume)
                Attenuate(channel, volume, sink);
        } else if (s.voice == Voice::Bass) {
            // Tone 3 silent at the bass's period, the noise periodic on
            // tone 3's rate ($06 = 3; writing it restarts the register).
            if (trigger) {
                Attenuate(2, 0, sink);
                psg_.Write(0xE3, sink);
            }
            if (trigger || s.period != sh.period)
                TonePeriod(2, s.period, sink);
            if (trigger || volume != sh.volume)
                Attenuate(3, volume, sink);
        } else {
            const int code = (s.shape ? 0 : 4) | static_cast<int>(s.period & 3);
            if (trigger || s.period != sh.period || s.shape != sh.shape)
                psg_.Write(static_cast<std::uint8_t>(0xE0 | code), sink);
            if (trigger || volume != sh.volume)
                Attenuate(3, volume, sink);
        }
        sh.active = true;
        sh.voice = s.voice;
        sh.period = s.period;
        sh.volume = volume;
        sh.shape = s.shape;
    }

    void Changed(std::int64_t cycle) noexcept {
        // 2 dB a step: 10^(-a / 10).
        static constexpr double kVolume[16] = {
            1.0, 0.7943282347242815, 0.6309573444801932, 0.5011872336272722,
            0.3981071705534972, 0.3162277660168379, 0.2511886431509580, 0.1995262314968879,
            0.1584893192461113, 0.1258925411794167, 0.1, 0.0794328234724281,
            0.0630957344480193, 0.0501187233627272, 0.0398107170553497, 0.0,
        };
        double side[2] = { 0.0, 0.0 };
        const std::uint8_t stereo = revision_ == kSegaHandheld ? psg_.Stereo() : 0xFF;
        for (int c = 0; c < kChannels; ++c) {
            const double level = psg_.Out(c) * kVolume[psg_.Attenuation(c)];
            if (stereo & (0x10 << c))
                side[0] += level;
            if (stereo & (0x01 << c))
                side[1] += level;
        }
        const double sign = psg_.Inverted() ? -1.0 : 1.0;
        const float levels[2] = { static_cast<float>(sign * side[0]), static_cast<float>(sign * side[1]) };
        mix_.Set(0, tb_.Position(cycle, StepBuffer::kSubBits), levels);
    }

    int rate_ = 48000;
    int revision_ = kSegaConsole;
    psg::Chip psg_;
    Timebase tb_;
    StepMix<1, 2> mix_;
    FirstOrder pinBlocker_[2];
    Shadow shadow_[kChannels];
};

} // namespace protocodus::reboot
