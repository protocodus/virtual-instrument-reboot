#pragma once

// Handheld sound driver: translates notes and controls into legal SM83 CPU
// writes onto a Handheld hardware blackbox (Boards/Handheld.h).
//
// - Pulses (CH1, CH2): write $V8 to NRx2 first so the DAC stays on and turns on
//   at cycle 0; follow with NRx1, NRx3, NRx4 trigger. Volume updates on held
//   notes step via Pan Docs' consistent $08 zombie increment.
// - Wave (CH3): native 11-bit period down to 32 Hz (period <= 2048), loading
//   either the chosen wavetable or a 32-step Pulse duty table when serving as
//   the Pulse fallback below 64 Hz. Below 32 Hz (period > 2048, e.g. A0-B0),
//   the driver loads all-$FF into Wave RAM and runs a master-cycle timer ISR
//   writing NR32 at 32 steps per wave cycle, identical to how handheld PCM/bass
//   drivers extend CH3 below its register floor without modifying the chip.
// - Noise (CH4): retriggers when switching between 15-bit and 7-bit LFSR width
//   so a held note cannot hit Pan Docs' all-1s LFSR lock-up.
// - The chip's own units, when the playing layer asks for them: the pulses'
//   and the noise's envelope (NRx2), taken at a trigger; CH1's sweep (NR10),
//   written before a trigger, after which the driver leaves CH1's period to
//   it; and the sides each channel goes to (NR51), written as they change.

#include "../Boards/Handheld.h"
#include "DriverTypes.h"
#include "Waves.h"

#include <climits>
#include <cstdint>

namespace protocodus::reboot {

class HandheldDriver {
public:
    static constexpr int kChannels = Handheld::kChannels;

    void Boot(Handheld& h) noexcept {
        for (Shadow& s : shadow_)
            s = Shadow {};
        waveRamKind_ = WaveRamKind::None;
        waveRamShape_ = 0;
        isrActive_ = false;
        isrVoice_ = Voice::Wave;
        isrShape_ = 0;
        isrVolume_ = 0;
        isrStep_ = 0;
        isrLastNr32_ = 0;
        isrDue_ = 0;
        isrStepPeriod_ = 0;
        nr51_ = 0xFF;
        nr10_ = 0x00;

        // A sound driver's start: power on, full master volume, every
        // channel to both sides, the sweep off (Pan Docs, Audio registers).
        h.Write(0xFF26, 0x80);
        h.Write(0xFF24, 0x77);
        h.Write(0xFF25, 0xFF);
        h.Write(0xFF10, 0x00);
        h.FinishBoot();
    }

    // The pulses' and the noise's envelope unit (Pan Docs, Audio Registers,
    // NRx2; spec): from a trigger it moves the volume one step every pace
    // ticks of the 64 Hz DIV-APU step, 65536 cycles, up to 15 or down to 0,
    // from the volume NRx2 gives; pace 0 holds it. The wave channel has none.
    [[nodiscard]] static EnvelopeUnit EnvelopeOf(int channel) noexcept {
        return channel == 2 ? EnvelopeUnit {} : EnvelopeUnit { 7, 65536.0, true, true, false };
    }

    // Only CH1 has a sweep (Pan Docs, NR10).
    [[nodiscard]] static bool HasSweep(int channel) noexcept { return channel == 0; }

    [[nodiscard]] static int QuantizeVolume(int channel, Voice, double gain) noexcept {
        if (channel == 2) {
            const double v = gain * 15.0;
            return v >= 11.5 ? 3 : v >= 5.5 ? 2 : v >= 1.5 ? 1 : 0;
        }
        if (gain <= 0.0)
            return 0;
        if (gain >= 1.0)
            return 15;
        const int v = static_cast<int>(gain * 15.0 + 0.5);
        return v < 0 ? 0 : v > 15 ? 15 : v;
    }

    // The period for `hz` on a channel: for the pulses and wave channel the
    // 32-step period count 2048 - x (extended past 2048 via NR32 timer ISR on
    // CH3), for noise the NR43 byte. False when the channel cannot reach `hz`.
    [[nodiscard]] bool PeriodFor(int channel, Voice, double hz, int shape, std::int64_t& period) const noexcept {
        if (hz <= 0.0)
            return false;
        if (channel < 2) {
            // Pan Docs: 131072 / (2048 - x) Hz.
            period = NearestCount(131072.0 / hz);
            return period >= 1 && period <= 2048;
        }
        if (channel == 2) {
            // Pan Docs: 65536 / (2048 - x) Hz; counts past 2048 play via the
            // driver's NR32 timer ISR (down to 1 Hz).
            period = NearestCount(65536.0 / hz);
            return period >= 1 && period <= 65536;
        }
        period = NoiseFor(hz, (shape & 1) != 0);
        return true;
    }

    [[nodiscard]] std::int64_t NextEvent() const noexcept {
        return isrActive_ ? isrDue_ : INT64_MAX;
    }

    void RunTo(Handheld& h, std::int64_t cycle) {
        while (isrActive_ && isrDue_ < cycle) {
            const std::int64_t at = isrDue_;
            h.RunTo(at);
            ServiceAt(h, at);
        }
        h.RunTo(cycle);
    }

    void ServiceAt(Handheld& h, std::int64_t cycle) noexcept {
        while (isrActive_ && isrDue_ <= cycle) {
            isrStep_ = (isrStep_ + 1) & 31;
            const std::uint8_t nr32 = IsrStepNr32();
            if (nr32 != isrLastNr32_) {
                h.Write(0xFF1C, nr32);
                isrLastNr32_ = nr32;
            }
            isrDue_ += isrStepPeriod_;
        }
    }

    void Start(Handheld& h, int channel, const ChannelSettings& s) noexcept {
        Shadow& sh = shadow_[channel];
        Sides(h, channel, s.spread);
        if (channel < 2) {
            const std::uint16_t base = channel == 0 ? 0xFF10 : 0xFF15;
            const int x = 2048 - static_cast<int>(s.period);
            const int volume = Clamp(s.volume, 0, 15);
            const std::uint8_t nrx2 = Nrx2(s, volume);
            // Pan Docs: NRx2 is taken at the trigger; $V8 keeps the DAC on and
            // the envelope still. When the DAC is off (!sh.active), write NRx2
            // first so the DAC turns on at cycle 0; when already on, write it
            // immediately before NRx4, and not at all when it already holds
            // that byte (a write while the channel plays is a zombie write,
            // which moves the volume until the trigger reloads it).
            if (!sh.active)
                h.Write(base + 2, nrx2);
            if (channel == 0) {
                const std::uint8_t nr10 = Nr10(s);
                if (nr10 != nr10_) {
                    h.Write(0xFF10, nr10);
                    nr10_ = nr10;
                }
            }
            h.Write(base + 1, static_cast<std::uint8_t>((s.shape & 3) << 6));
            h.Write(base + 3, static_cast<std::uint8_t>(x & 0xFF));
            if (sh.active && nrx2 != sh.nrx2)
                h.Write(base + 2, nrx2);
            h.Write(base + 4, static_cast<std::uint8_t>(0x80 | ((x >> 8) & 7)));
            Took(sh, s, volume, s.shape & 3, nrx2);
            sh.sweep = channel == 0 && nr10_ != 0;
        } else if (channel == 2) {
            WriteWave(h, s, true);
        } else {
            const int volume = Clamp(s.volume, 0, 15);
            const std::uint8_t nrx2 = Nrx2(s, volume);
            if (!sh.active)
                h.Write(0xFF21, nrx2);
            h.Write(0xFF22, static_cast<std::uint8_t>(s.period));
            if (sh.active && nrx2 != sh.nrx2)
                h.Write(0xFF21, nrx2);
            h.Write(0xFF23, 0x80);
            Took(sh, s, volume, s.shape & 1, nrx2);
        }
    }

    // A held note's new settings, written as a sound driver writes them
    // between triggers: only what changed, the volume by zombie steps. The
    // envelope unit takes NRx2 only at a trigger, and once it has run the
    // volume is no longer the one zombie steps count from, so a new
    // envelope, or none after one, starts the channel again; while one runs
    // the volume is the chip's. A sweeping CH1's period is the sweep's.
    void Change(Handheld& h, int channel, const ChannelSettings& s) noexcept {
        Shadow& sh = shadow_[channel];
        if (!sh.active || (channel != 2 && NewEnvelope(sh, s))) {
            Start(h, channel, s);
            return;
        }
        Sides(h, channel, s.spread);
        if (channel < 2) {
            const std::uint16_t base = channel == 0 ? 0xFF10 : 0xFF15;
            if ((s.shape & 3) != sh.shape) {
                h.Write(base + 1, static_cast<std::uint8_t>((s.shape & 3) << 6));
                sh.shape = s.shape & 3;
            }
            if (s.period != sh.period && !sh.sweep) {
                const int x = 2048 - static_cast<int>(s.period);
                h.Write(base + 3, static_cast<std::uint8_t>(x & 0xFF));
                h.Write(base + 4, static_cast<std::uint8_t>((x >> 8) & 7));
                sh.period = s.period;
            }
            if (sh.pace == 0)
                Zombie(h, base + 2, sh, Clamp(s.volume, 0, 15));
        } else if (channel == 2) {
            WriteWave(h, s, false);
        } else {
            if ((s.shape & 1) != sh.shape) {
                // Retrigger on LFSR width change so switching 15-bit -> 7-bit
                // mid-note cannot lock up on seven consecutive 1-bits (Pan Docs).
                Start(h, channel, s);
                return;
            }
            if (s.period != sh.period) {
                h.Write(0xFF22, static_cast<std::uint8_t>(s.period));
                sh.period = s.period;
            }
            if (sh.pace == 0)
                Zombie(h, 0xFF21, sh, Clamp(s.volume, 0, 15));
        }
    }

    // Silence, keeping the DAC on (Pan Docs: turning a DAC off pops); after
    // an envelope, by a trigger at volume 0.
    void Stop(Handheld& h, int channel) noexcept {
        const Shadow& sh = shadow_[channel];
        ChannelSettings s;
        s.voice = sh.voice;
        s.period = sh.period;
        s.shape = sh.shape;
        s.spread = sh.spread;
        s.sweep = sh.sweepAmount;
        s.sweepPace = sh.sweepPace;
        Change(h, channel, s);
    }

private:
    enum class WaveRamKind : std::uint8_t { None, Pulse, Wave, AllMax };

    struct Shadow {
        bool active = false;
        Voice voice = Voice::Pulse;
        std::int64_t period = 0;
        int volume = 0;
        int shape = 0;
        std::uint8_t nrx2 = 0;   // what NRx2 holds
        int pace = 0;            // the envelope it was triggered with: the volume is the chip's while it runs
        bool up = false;
        bool spread = false;
        bool sweep = false;      // CH1 sweeps: its period is the sweep's
        int sweepAmount = 0, sweepPace = 0;
    };

    static int Clamp(int v, int lo, int hi) noexcept { return v < lo ? lo : v > hi ? hi : v; }

    static int Pace(const ChannelSettings& s) noexcept { return Clamp(s.pace, 0, 7); }

    // NRx2 for a trigger: the envelope from the volume at its pace, or $V8,
    // which keeps the DAC on and the envelope still (Pan Docs). A falling
    // envelope from 0 is no envelope: $00 would turn the DAC off.
    static std::uint8_t Nrx2(const ChannelSettings& s, int volume) noexcept {
        const int pace = volume == 0 && !s.up ? 0 : Pace(s);
        return static_cast<std::uint8_t>((volume << 4) | (pace == 0 ? 0x08 : (s.up ? 0x08 : 0x00) | pace));
    }

    static bool NewEnvelope(const Shadow& sh, const ChannelSettings& s) noexcept {
        const int pace = Pace(s);
        return pace != sh.pace || (pace > 0 && s.up != sh.up);
    }

    static void Took(Shadow& sh, const ChannelSettings& s, int volume, int shape, std::uint8_t nrx2) noexcept {
        sh.active = true;
        sh.voice = s.voice;
        sh.period = s.period;
        sh.volume = volume;
        sh.shape = shape;
        sh.nrx2 = nrx2;
        sh.pace = Pace(s);
        sh.up = s.up;
        sh.spread = s.spread;
        sh.sweepAmount = s.sweep;
        sh.sweepPace = s.sweepPace;
    }

    // NR10 (Pan Docs; spec): pace (1 to 7 ticks of 128 Hz) in bits 6-4,
    // bit 3 set to subtract, the shift in bits 2-0. Each iteration the period
    // register x becomes x +/- x >> shift: adding raises the pitch, and past
    // 2047 the chip turns the channel off; subtracting lowers it. A step's
    // size `amount` 1 to 7 is 8 - shift, so 7 moves the most. 0 is no sweep.
    static std::uint8_t Nr10(const ChannelSettings& s) noexcept {
        if (s.sweep == 0)
            return 0x00;
        const int amount = Clamp(s.sweep < 0 ? -s.sweep : s.sweep, 1, 7);
        return static_cast<std::uint8_t>((Clamp(s.sweepPace, 1, 7) << 4) | (s.sweep < 0 ? 0x08 : 0x00) | (8 - amount));
    }

    // NR51's two bits for a channel (Pan Docs: bits 7-4 left, CH4 to CH1;
    // bits 3-0 right), written when they change. Spread puts pulse 1 on the
    // left and pulse 2 on the right (ours: the channels a chord is split
    // across), the wave and the noise on both.
    void Sides(Handheld& h, int channel, bool spread) noexcept {
        const int left = !spread || channel != 1, right = !spread || channel != 0;
        const int mask = 0x11 << channel;
        const int bits = (left ? 0x10 << channel : 0) | (right ? 0x01 << channel : 0);
        const std::uint8_t next = static_cast<std::uint8_t>((nr51_ & ~mask) | bits);
        if (next != nr51_) {
            h.Write(0xFF25, next);
            nr51_ = next;
        }
    }

    static std::uint8_t Nr32Byte(int levelCode) noexcept {
        static constexpr std::uint8_t kCode[4] = { 0x00, 0x60, 0x40, 0x20 };
        return kCode[Clamp(levelCode, 0, 3)];
    }

    static void BuildWaveRam(Voice voice, int shape, bool allMax, std::uint8_t (&packed)[16]) noexcept {
        if (allMax) {
            for (int i = 0; i < 16; ++i)
                packed[i] = 0xFF;
            return;
        }
        if (voice == Voice::Pulse) {
            static constexpr int kHighSteps[4] = { 4, 8, 16, 24 };
            const int high = kHighSteps[shape & 3];
            for (int i = 0; i < 16; ++i) {
                const std::uint8_t a = (2 * i) < high ? 0x0F : 0x00;
                const std::uint8_t b = (2 * i + 1) < high ? 0x0F : 0x00;
                packed[i] = static_cast<std::uint8_t>((a << 4) | b);
            }
            return;
        }
        const Wave32& w = kWaves[shape % kWaveCount];
        for (int i = 0; i < 16; ++i)
            packed[i] = w.packed[i];
    }

    [[nodiscard]] std::uint8_t IsrStepNr32() const noexcept {
        if (isrVolume_ <= 0)
            return 0x00;
        int baseSample = 0;
        if (isrVoice_ == Voice::Pulse) {
            static constexpr int kHighSteps[4] = { 4, 8, 16, 24 };
            baseSample = isrStep_ < kHighSteps[isrShape_ & 3] ? 15 : 0;
        } else {
            baseSample = kWaves[isrShape_ % kWaveCount].samples[isrStep_];
        }
        static constexpr int kShift[4] = { 4, 2, 1, 0 };
        const int target = baseSample >> kShift[Clamp(isrVolume_, 0, 3)];
        if (target >= 11)
            return 0x20;
        if (target >= 5)
            return 0x40;
        if (target >= 2)
            return 0x60;
        return 0x00;
    }

    void WriteWave(Handheld& h, const ChannelSettings& s, bool trigger) noexcept {
        Shadow& sh = shadow_[2];
        const int normShape = s.voice == Voice::Pulse ? (s.shape & 3) : (s.shape % kWaveCount);
        const int volume = Clamp(s.volume, 0, 3);
        const bool needIsr = s.period > 2048;
        const WaveRamKind wantKind = needIsr ? WaveRamKind::AllMax
            : (s.voice == Voice::Pulse ? WaveRamKind::Pulse : WaveRamKind::Wave);
        const int wantRamShape = needIsr ? 0 : normShape;
        const bool ramChanged = waveRamKind_ != wantKind || (wantKind != WaveRamKind::AllMax && waveRamShape_ != wantRamShape);

        if (!needIsr) {
            const bool wasIsr = isrActive_;
            isrActive_ = false;
            if (ramChanged) {
                std::uint8_t packed[16] {};
                BuildWaveRam(s.voice, normShape, false, packed);
                // Pan Docs: wave RAM is written with the DAC off; changing wave
                // restarts the channel on hardware.
                h.Write(0xFF1A, 0x00);
                for (int i = 0; i < 16; ++i)
                    h.Write(static_cast<std::uint16_t>(0xFF30 + i), packed[i]);
                h.Write(0xFF1A, 0x80);
                waveRamKind_ = wantKind;
                waveRamShape_ = wantRamShape;
                trigger = true;
            } else if (trigger) {
                // Pan Docs ("Audio Quirks"; spec): triggering CH3 on DMG while
                // it is active and within 2 T-cycles of a sample read corrupts
                // Wave RAM; drivers turn NR30 off before re-enabling and triggering.
                if (sh.active && h.Revision() == Handheld::kOriginal)
                    h.Write(0xFF1A, 0x00);
                h.Write(0xFF1A, 0x80);
            }
            if (trigger || wasIsr || volume != sh.volume)
                h.Write(0xFF1C, Nr32Byte(volume));
            if (trigger || wasIsr || s.period != sh.period) {
                const int x = 2048 - static_cast<int>(s.period);
                h.Write(0xFF1D, static_cast<std::uint8_t>(x & 0xFF));
                h.Write(0xFF1E, static_cast<std::uint8_t>((trigger ? 0x80 : 0x00) | ((x >> 8) & 7)));
            } else if (trigger) {
                const int x = 2048 - static_cast<int>(s.period);
                h.Write(0xFF1E, static_cast<std::uint8_t>(0x80 | ((x >> 8) & 7)));
            }
        } else {
            const bool wasIsr = isrActive_;
            if (ramChanged) {
                std::uint8_t packed[16] {};
                BuildWaveRam(s.voice, normShape, true, packed);
                h.Write(0xFF1A, 0x00);
                for (int i = 0; i < 16; ++i)
                    h.Write(static_cast<std::uint16_t>(0xFF30 + i), packed[i]);
                h.Write(0xFF1A, 0x80);
                waveRamKind_ = wantKind;
                waveRamShape_ = wantRamShape;
                trigger = true;
            } else if (trigger) {
                // Pan Docs ("Audio Quirks"; spec): triggering CH3 on DMG while
                // it is active and within 2 T-cycles of a sample read corrupts
                // Wave RAM; drivers turn NR30 off before re-enabling and triggering.
                if (sh.active && h.Revision() == Handheld::kOriginal)
                    h.Write(0xFF1A, 0x00);
                h.Write(0xFF1A, 0x80);
            }
            isrActive_ = volume > 0;
            isrVoice_ = s.voice;
            isrShape_ = normShape;
            isrVolume_ = volume;
            isrStepPeriod_ = 2 * s.period;
            if (trigger || !wasIsr) {
                isrStep_ = 0;
                isrDue_ = h.Now() + isrStepPeriod_;
            }
            if (trigger) {
                h.Write(0xFF1D, 0x00);
                h.Write(0xFF1E, 0x80);
            }
            const std::uint8_t nr32 = IsrStepNr32();
            if (trigger || !wasIsr || nr32 != isrLastNr32_) {
                h.Write(0xFF1C, nr32);
                isrLastNr32_ = nr32;
            }
        }
        sh = { true, s.voice, s.period, volume, normShape };
    }

    // Pan Docs: "write $08 to NRx2 to increment the volume" - the one zombie
    // write every unit agrees on; 15 of them lower it by one.
    static void Zombie(Handheld& h, std::uint16_t nrx2, Shadow& sh, int volume) noexcept {
        for (int n = (volume - sh.volume) & 15; n > 0; --n) {
            h.Write(nrx2, 0x08);
            sh.nrx2 = 0x08;
        }
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

    Shadow shadow_[kChannels];
    WaveRamKind waveRamKind_ = WaveRamKind::None;
    int waveRamShape_ = 0;
    bool isrActive_ = false;
    Voice isrVoice_ = Voice::Wave;
    int isrShape_ = 0;
    int isrVolume_ = 0;
    int isrStep_ = 0;
    std::uint8_t isrLastNr32_ = 0;
    std::int64_t isrDue_ = 0;
    std::int64_t isrStepPeriod_ = 0;
    std::uint8_t nr51_ = 0xFF;   // as Boot leaves it
    std::uint8_t nr10_ = 0x00;
};

} // namespace protocodus::reboot
