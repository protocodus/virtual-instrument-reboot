#pragma once

// The handheld board: the Game Boy's APU (Chips/GameBoyApu.h) on its
// 4194304 Hz clock, its four DACs, its stereo mixer and master volume, and
// the coupling capacitor after them.
//
// Revisions: Original (DMG) and Color (CGB), which differ in the APU's
// quirks.
//
// DACs (Pan Docs, Audio details, read 2026-11-17; spec): a DAC is on when
// NRx2 & $F8 is not 0 (NR30 bit 7 for the wave channel); on, it maps digital
// 0 to 15 linearly to analog +1 to -1 ("a disabled channel outputs 0, which
// an enabled DAC will dutifully convert into analog 1"); off, it gives 0.
// The mixer adds the channels NR51 sends to each side and scales each side
// by NR50's volume + 1 over 8. Pin is that sum. Line adds the capacitor:
// Pan Docs' out = in - capacitor, capacitor = in - out * factor, factor
// 0.999958 per clock (reported), a corner near 28 Hz, with the output 0 and
// the capacitor held while every DAC is off.
//
// The Color takes the same factor (reported: a measurement of its sibling).
// Pan Docs' "use 0.998943 for MGB&CGB" (blargg's Game Boy Sound Operation),
// a corner near 706 Hz, is contradicted by a recording: kathoc (note.com,
// brickboy Development Story #10, 2026-08-29) recorded a Pocket and an
// original at their headphone sockets at 96 kHz, and at 71 Hz the Pocket was
// 2.8 dB under the original, where 0.998943 predicts 20 dB. Fitted to that
// recording, 706 Hz misses by 11.25 dB, the original's 28 Hz by 5.28 dB and
// the best, 5 Hz, by 5.24 dB, so it rejects 706 Hz but cannot tell 28 Hz
// from a lower corner. The Pocket's amplifier is the Color's chip: Gekkio's
// MGB-ECPU-01 drawing names U3 AMP-MGB, and Ken Shirriff's die photo of the
// Color's IR3R53N is marked AMP MGB. No recording of a Color itself was
// found. The DAC's fade when it turns off, the amplifier, headphone socket
// and speaker are not modelled (README, Known gaps).

#include "../Chips/GameBoyApu.h"
#include "../Driver/Waves.h"
#include "Board.h"

#include <cstdint>

namespace protocodus::reboot {

class Handheld {
public:
    enum Revision { kOriginal, kColor, kRevisions };
    static constexpr int kChannels = gb::Apu::kChannels;
    static constexpr double kFrameHz = 4194304.0 / 70224.0; // the LCD's 59.73 Hz
    static constexpr std::int64_t kFrameCycles = 70224;

    static constexpr Voice VoiceOf(int channel) noexcept {
        return channel < 2 ? Voice::Pulse : channel == 2 ? Voice::Wave : Voice::Noise;
    }

    // Setup.
    void Prepare(int sampleRate) {
        rate_ = sampleRate;
        DesignPinBlocker(pinBlocker_[0], sampleRate);
        DesignPinBlocker(pinBlocker_[1], sampleRate);
    }

    void Reset(int revision) noexcept {
        revision_ = revision;
        apu_.Reset(revision == kColor ? gb::Model::Cgb : gb::Model::Dmg);
        tb_.Set(gb::kClock, 1, rate_);
        mix_.Clear();
        for (int s = 0; s < 2; ++s) {
            capacitor_[s].Design(0.999958, rate_);
            capacitor_[s].Reset();
            pinBlocker_[s].Reset();
        }
        dacsOn_ = false, dacsHeard_ = false, pending_ = 0;
        for (Shadow& s : shadow_)
            s = Shadow {};
        Sink sink { this };
        // A sound driver's start: power on, full master volume, every
        // channel to both sides, the sweep off (Pan Docs, Audio registers).
        apu_.Write(0xFF26, 0x80, sink);
        apu_.Write(0xFF24, 0x77, sink);
        apu_.Write(0xFF25, 0xFF, sink);
        apu_.Write(0xFF10, 0x00, sink);
    }

    [[nodiscard]] std::int64_t CycleAt(int frame) const noexcept { return tb_.CycleAt(frame); }

    void RunTo(std::int64_t cycle) {
        Sink sink { this };
        apu_.Run(cycle, sink);
    }

    // The period for `hz` on a channel: for the pulses and the wave channel
    // the count 2048 - x of the 11-bit register x, for the noise the NR43
    // byte. False when the channel cannot reach `hz`.
    [[nodiscard]] bool PeriodFor(int channel, Voice, double hz, int shape, std::int64_t& period) const noexcept {
        if (hz <= 0.0)
            return false;
        if (channel < 2) {
            // Pan Docs: 131072 / (2048 - x) Hz.
            period = NearestCount(131072.0 / hz);
            return period >= 1 && period <= 2048;
        }
        if (channel == 2) {
            // 65536 / (2048 - x) Hz; counts past 2048 are ours (Reset's
            // SetWaveExtension), so every key reaches it.
            period = NearestCount(65536.0 / hz);
            return period >= 1;
        }
        period = NoiseFor(hz, shape != 0);
        return true;
    }

    void Start(int channel, const ChannelSettings& s) {
        Sink sink { this };
        Shadow& sh = shadow_[channel];
        if (channel < 2) {
            const std::uint16_t base = channel == 0 ? 0xFF10 : 0xFF15;
            const int x = 2048 - static_cast<int>(s.period);
            const int volume = Clamp15(s.volume);
            // Pan Docs: write $V8 to NRx2 before the trigger, so the DAC
            // stays on and the envelope does not run.
            apu_.Write(base + 1, static_cast<std::uint8_t>((s.shape & 3) << 6), sink);
            apu_.Write(base + 2, static_cast<std::uint8_t>((volume << 4) | 0x08), sink);
            apu_.Write(base + 3, static_cast<std::uint8_t>(x & 0xFF), sink);
            apu_.Write(base + 4, static_cast<std::uint8_t>(0x80 | ((x >> 8) & 7)), sink);
            sh = { true, s.period, volume, s.shape & 3 };
        } else if (channel == 2) {
            const int shape = s.shape % kWaveCount;
            if (!sh.waveLoaded || sh.shape != shape) {
                // DAC off, wave RAM, DAC on (Pan Docs: wave RAM is written
                // with the channel off).
                apu_.Write(0xFF1A, 0x00, sink);
                for (int i = 0; i < 16; ++i)
                    apu_.Write(static_cast<std::uint16_t>(0xFF30 + i), kWaves[shape].packed[i], sink);
                sh.waveLoaded = true;
            }
            apu_.Write(0xFF1A, 0x80, sink);
            WaveCount(s.period, sink);
            apu_.Write(0xFF1C, WaveLevel(s.volume), sink);
            const int x = WaveRegister(s.period);
            apu_.Write(0xFF1D, static_cast<std::uint8_t>(x & 0xFF), sink);
            apu_.Write(0xFF1E, static_cast<std::uint8_t>(0x80 | ((x >> 8) & 7)), sink);
            sh.active = true, sh.period = s.period, sh.volume = s.volume, sh.shape = shape;
        } else {
            const int volume = Clamp15(s.volume);
            apu_.Write(0xFF21, static_cast<std::uint8_t>((volume << 4) | 0x08), sink);
            apu_.Write(0xFF22, static_cast<std::uint8_t>(s.period), sink);
            apu_.Write(0xFF23, 0x80, sink);
            sh = { true, s.period, volume, s.shape };
        }
    }

    // A held note's new settings, written as a sound driver writes them
    // between triggers: only what changed, the volume by zombie steps.
    void Change(int channel, const ChannelSettings& s) {
        Sink sink { this };
        Shadow& sh = shadow_[channel];
        if (!sh.active) {
            Start(channel, s);
            return;
        }
        if (channel < 2) {
            const std::uint16_t base = channel == 0 ? 0xFF10 : 0xFF15;
            if ((s.shape & 3) != sh.shape) {
                apu_.Write(base + 1, static_cast<std::uint8_t>((s.shape & 3) << 6), sink);
                sh.shape = s.shape & 3;
            }
            if (s.period != sh.period) {
                const int x = 2048 - static_cast<int>(s.period);
                apu_.Write(base + 3, static_cast<std::uint8_t>(x & 0xFF), sink);
                apu_.Write(base + 4, static_cast<std::uint8_t>((x >> 8) & 7), sink);
                sh.period = s.period;
            }
            Zombie(base + 2, sh, Clamp15(s.volume), sink);
        } else if (channel == 2) {
            if (s.shape % kWaveCount != sh.shape) {
                // A new wave needs the channel off (Start; Pan Docs), so it
                // starts again, as a driver's would.
                Start(channel, s);
                return;
            }
            if (s.period != sh.period) {
                WaveCount(s.period, sink);
                const int x = WaveRegister(s.period);
                apu_.Write(0xFF1D, static_cast<std::uint8_t>(x & 0xFF), sink);
                apu_.Write(0xFF1E, static_cast<std::uint8_t>((x >> 8) & 7), sink);
                sh.period = s.period;
            }
            if (WaveLevel(s.volume) != WaveLevel(sh.volume)) {
                apu_.Write(0xFF1C, WaveLevel(s.volume), sink);
                sh.volume = s.volume;
            }
        } else {
            if (s.period != sh.period) {
                apu_.Write(0xFF22, static_cast<std::uint8_t>(s.period), sink);
                sh.period = s.period;
            }
            Zombie(0xFF21, sh, Clamp15(s.volume), sink);
        }
    }

    // Silence, keeping the DAC on (Pan Docs: turning a DAC off pops).
    void Stop(int channel) {
        ChannelSettings s;
        s.period = shadow_[channel].period;
        s.shape = shadow_[channel].shape;
        Change(channel, s);
    }

    // Runs to the block's end, adds the board's sides into left and right,
    // and moves on.
    void Render(int frames, Path path, float* left, float* right) {
        RunTo(tb_.CycleAt(frames));
        float bus[2][StepBuffer::kMaxFrames] {};
        mix_.Read(0, bus[0], frames);
        mix_.Read(1, bus[1], frames);
        float* out[2] = { left, right };
        for (int i = 0; i < frames; ++i) {
            while (pending_ > 0 && queue_[0].sample <= i) {
                dacsHeard_ = queue_[0].on;
                for (int k = 1; k < pending_; ++k)
                    queue_[k - 1] = queue_[k];
                --pending_;
            }
            for (int side = 0; side < 2; ++side) {
                double y;
                if (path == Path::Line)
                    y = dacsHeard_ ? capacitor_[side].Process(bus[side][i]) : 0.0;
                else
                    y = pinBlocker_[side].Process(bus[side][i]);
                out[side][i] += static_cast<float>(y);
            }
        }
        for (int k = 0; k < pending_; ++k)
            queue_[k].sample -= frames;
        tb_.Advance(frames);
    }

    [[nodiscard]] const gb::Apu& Chip() const noexcept { return apu_; }

private:
    struct Shadow {
        bool active = false;
        std::int64_t period = 0;
        int volume = 0;
        int shape = 0;
        bool waveLoaded = false;
    };

    struct Sink {
        Handheld* board;
        void operator()(std::int64_t cycle) const { board->Changed(cycle); }
    };

    static int Clamp15(int v) noexcept { return v < 0 ? 0 : v > 15 ? 15 : v; }

    // NR32's output level nearest the common volume: mute, 25%, 50%, 100%
    // (codes 0, 3, 2, 1).
    static std::uint8_t WaveLevel(int volume) noexcept {
        const int v = Clamp15(volume);
        const int code = v >= 12 ? 1 : v >= 6 ? 2 : v >= 2 ? 3 : 0;
        return static_cast<std::uint8_t>(code << 5);
    }

    static int WaveRegister(std::int64_t count) noexcept {
        return count <= 2048 ? 2048 - static_cast<int>(count) : 0;
    }

    template <class S>
    void WaveCount(std::int64_t count, S& sink) {
        apu_.SetWaveExtension(count > 2048 ? static_cast<int>(count - 2048) : 0, sink);
    }

    // Pan Docs: "write $08 to NRx2 to increment the volume" - the one zombie
    // write every unit agrees on; 15 of them lower it by one.
    template <class S>
    void Zombie(std::uint16_t nrx2, Shadow& sh, int volume, S& sink) {
        for (int n = (volume - sh.volume) & 15; n > 0; --n)
            apu_.Write(nrx2, 0x08, sink);
        sh.volume = volume;
    }

    // The NR43 byte whose LFSR clock, 262144 / (r * 2^s) Hz with r = 0
    // counting as 0.5 (Pan Docs), is nearest `hz` times the register's
    // cycle: 127 steps in 7-bit mode, so it plays in tune, and 32 in 15-bit
    // mode (ours: the key picks the noise's colour).
    static std::uint8_t NoiseFor(double hz, bool shortMode) noexcept {
        const double target = hz * (shortMode ? 127.0 : 32.0);
        std::uint8_t best = 0;
        double bestRatio = 0.0;
        for (int s = 0; s < 14; ++s)
            for (int r = 0; r < 8; ++r) {
                const double rate = 262144.0 / ((r ? r : 0.5) * static_cast<double>(1 << s));
                const double ratio = rate > target ? rate / target : target / rate;
                if (bestRatio == 0.0 || ratio < bestRatio) {
                    bestRatio = ratio;
                    best = static_cast<std::uint8_t>((s << 4) | r);
                }
            }
        return static_cast<std::uint8_t>(best | (shortMode ? 0x08 : 0x00));
    }

    void Changed(std::int64_t cycle) noexcept {
        const std::int64_t position = tb_.Position(cycle, StepBuffer::kSubBits);
        const std::uint8_t nr51 = apu_.Panning();
        const std::uint8_t nr50 = apu_.MasterVolume();
        double side[2] = { 0.0, 0.0 };
        bool any = false;
        for (int c = 0; c < kChannels; ++c) {
            if (!apu_.DacOn(c))
                continue;
            any = true;
            const double analog = 1.0 - 2.0 * apu_.Out(c) / 15.0;
            if (nr51 & (0x10 << c))
                side[0] += analog;
            if (nr51 & (0x01 << c))
                side[1] += analog;
        }
        // NR50: bits 6-4 left, 2-0 right; (volume + 1) / 8.
        const float levels[2] = {
            static_cast<float>(side[0] * (((nr50 >> 4) & 7) + 1) / 8.0),
            static_cast<float>(side[1] * ((nr50 & 7) + 1) / 8.0),
        };
        mix_.Set(0, position, levels);
        if (any != dacsOn_) {
            dacsOn_ = any;
            // The capacitor switches with the DACs; the steps reach the
            // output blep::kDelay samples late, and so does the switch.
            const int sample = static_cast<int>(position >> StepBuffer::kSubBits) + blep::kDelay;
            if (pending_ == kQueue)
                --pending_;
            queue_[pending_++] = { sample, any };
        }
    }

    static constexpr int kQueue = 32;
    struct Switch {
        int sample;
        bool on;
    };

    int rate_ = 48000;
    int revision_ = kOriginal;
    gb::Apu apu_;
    Timebase tb_;
    StepMix<1, 2> mix_;
    ChargeHighPass capacitor_[2];
    FirstOrder pinBlocker_[2];
    bool dacsOn_ = false, dacsHeard_ = false;
    Switch queue_[kQueue] {};
    int pending_ = 0;
    Shadow shadow_[kChannels];
};

} // namespace protocodus::reboot
