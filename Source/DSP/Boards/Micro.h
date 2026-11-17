#pragma once

// The home micro board: the 128K's AY-3-8912 (Chips/Ay8910.h, clocked at
// half the CPU clock) and the beeper (Chips/Beeper.h), both on the CPU's
// T-states, and their DACs. The 48K has the beeper alone.
//
// Revisions: the 128K (3546900 Hz), the 48K Issue 3 and the 48K Issue 2
// (3500000 Hz).
//
// AY DAC: Ayumi's AY-3-8910 table (Peter Sovietov, MIT licence,
// ThirdParty/ayumi-LICENSE.txt; reported), one level a pair of the 32 codes,
// the three channels summed. The real outputs are tied through resistors
// and interact (README, Known gaps).
// Beeper DAC: pin 28's voltage for each EAR and MIC state, measured on an
// Issue 2 and an Issue 3 (World of Spectrum's 48K tech notes; reported):
// Issue 3 0.34, 0.66, 3.56, 3.70 V; Issue 2 0.39, 0.73, 3.66, 3.79 V. The
// 128K's pin and its mixing with the AY are unsourced: it takes Issue 3's
// levels, and by our convention the beeper's swing at MIC off (0.66 to
// 3.70 V) equals one AY channel at full volume (README, Known gaps): the
// 128K service manual gives only the modulator's inputs, the ULA through
// R112/C123 (68 kOhm, 1 uF) and the AY through R132/C127 (39 kOhm, 1 uF),
// not the AY's level or how its pins are joined. Pin 28's slow fall after
// EAR goes low (World of Spectrum) is put down to the EAR/MIC network (R36,
// C32, R37, C35); no published model fits its timings, so the pin steps at
// once.
//
// Line on the 128K is its TV sound, the modulator's input (128K service
// manual; spec): the ULA through R112 and C123 (68 kOhm, 1 uF) and the AY
// through R132 and C127 (39 kOhm, 1 uF) into one node, which weights the
// beeper's pin by 39 / 107 and the AY's by 68 / 107. The AY's DAC swings
// 0 to 1 V (the AY data manual, 3.7: "a normalized voltage range of from 0
// to 1 Volt"; spec), and its three pins are tied straight together on the
// 128K's board (the tracks between pins 1, 4 and 5 of IC32; Octocom,
// "Articulo 4"), taken here as the pins' average (estimate: equal, linear
// output resistances; the real pins interact, README, Known gaps). The
// beeper's pin swings Issue 3's 0.66 to 3.70 V (sibling: the 48K Issue 3).
// So on Line the beeper stands 3 x 39 / 68 x 3.04 = 5.2 times, 14.4 dB,
// over one AY channel at full volume, which keeps its Pin level; the two
// couplings, 1 uF each in series around 107 kOhm, high-pass at 3.0 Hz
// (estimate: the modulator's input taken as high). The 48K's modulator
// carries no sound and no socket or speaker stage is modelled, so its Line
// renders as Pin.
//
// The beeper's notes run the ROM's BEEPER loop: the first OUT 209 - 12 (L
// mod 4) T-states after the call (its setup and first, shortened pass), then
// one every 4 HL + 118 (Chips/Beeper.h). A key-up lets the loop finish its
// cycle, as BEEPER ends only after a full one, with EAR low.
//
// ULA contention (World of Spectrum, "48K reference" and "128K reference",
// read 2026-11-17; spec): the ROM is not contended, but port $FE is, so the
// loop's OUT ($FE),A waits whenever its I/O cycle meets the ULA reading the
// screen. With A's high byte outside $40-$7F and the port's low bit 0 the
// pattern is N:1, C:3: the I/O cycle's first T-state runs, then the CPU is
// held for the delay at that cycle, then runs 3 more, and the OUT takes
// effect as it ends. The delay is 6, 5, 4, 3, 2, 1, 0, 0 over each 8
// T-states of the first 128 of a line, on the 192 screen lines, from
// T-state 14335 of the 48K's 69888 (224 a line) and 14361 of the 128K's
// 70908 (228 a line). Each delay moves the rest of the loop with it, so a
// note runs slower and unevenly while the screen is drawn. The frame starts
// at the board's cycle 0 (ours: any phase is the machine's).

#include "../Chips/Ay8910.h"
#include "../Chips/Beeper.h"
#include "Board.h"

#include <cstdint>

namespace protocodus::reboot {

class Micro {
public:
    enum Revision { k128K, k48KIssue3, k48KIssue2, kRevisions };
    static constexpr int kChannels = 4;          // AY A, B, C; the beeper
    static constexpr int kBeeper = 3;

    void Prepare(int sampleRate) {
        rate_ = sampleRate;
        DesignPinBlocker(pinBlocker_, sampleRate);
        tvCoupling_.Design(FirstOrder::Kind::HighPass, kTvCouplingHz, sampleRate);
    }

    void Reset(int revision) noexcept {
        revision_ = revision;
        clock_ = revision == k128K ? zx::kClock128 : zx::kClock48;
        tb_.Set(clock_, 1, rate_);
        ay_.Reset(16, true);
        beeper_.Reset();
        mix_.Clear();
        pinBlocker_.Reset();
        tvCoupling_.Reset();
        for (Shadow& s : shadow_)
            s = Shadow {};
        beeping_ = false, stopping_ = false, ear_ = false;
        AyWrite(7, 0x3F);
        // The beeper's pin at rest, before any note, is the bus's zero: the
        // machine was on long before the first key, so its couplings have
        // long settled and switching it in does not thump (ours).
        restVolts_ = PinVolts();
        BeeperChanged(tb_.Start());
    }

    [[nodiscard]] bool HasAy() const noexcept { return revision_ == k128K; }
    // T-states a frame: the 128K's 228 x 311, the 48K's 224 x 312.
    [[nodiscard]] std::int64_t FrameCycles() const noexcept { return revision_ == k128K ? 70908 : 69888; }
    [[nodiscard]] std::int64_t Clock() const noexcept { return clock_; }
    [[nodiscard]] std::int64_t CycleAt(int frame) const noexcept { return tb_.CycleAt(frame); }

    void RunTo(std::int64_t cycle) {
        AySink sink { this };
        BeeperSink beep { this };
        while (beeping_ && due_ < cycle) {
            if (HasAy())
                ay_.Run(due_, sink);
            beeper_.Run(due_, beep);
            ear_ = !ear_;
            beeper_.Write(static_cast<std::uint8_t>((ear_ ? 0x10 : 0x00) | 0x08), beep);
            if (stopping_ && !ear_)
                beeping_ = false;
            due_ = Contended(due_ + zx::BeeperHalfPeriod(hl_));
        }
        if (HasAy())
            ay_.Run(cycle, sink);
        beeper_.Run(cycle, beep);
    }

    // AY tone: TP of CPU / (32 TP). AY noise: NP whose shift rate, CPU /
    // (32 NP), is nearest `hz` times 32 (ours). Buzzer: the envelope period
    // EP, a saw (shapes 0, 1) at CPU / (512 EP), a triangle (2, 3) at half
    // that. Beeper: the BEEPER loop's HL.
    [[nodiscard]] bool PeriodFor(int channel, Voice voice, double hz, int shape, std::int64_t& period) const noexcept {
        if (hz <= 0.0)
            return false;
        const double cpu = static_cast<double>(clock_);
        if (channel == kBeeper) {
            // Ours: the HL nearest in pitch on this machine's clock. The
            // BEEP command's own 437500 / f - 30.125 (routine $03F8)
            // assumes 3.5 MHz and 120.5 T-states a half cycle, which plays
            // a little sharp on the 48K and 23 cents sharp on the 128K.
            const double overhead = static_cast<double>(zx::kBeeperOverhead);
            const double hl = (cpu / (2.0 * hz) - overhead) / 4.0;
            if (!(hl >= 0.0 && hl <= 65535.0))
                return false;
            const auto low = static_cast<std::int64_t>(hl);
            const double target = 4.0 * hl + overhead;
            const double below = 4.0 * static_cast<double>(low) + overhead;
            period = low < 65535 && target * target >= below * (below + 4.0) ? low + 1 : low;
            return true;
        }
        if (!HasAy())
            return false;
        switch (voice) {
        case Voice::Noise: {
            const std::int64_t np = NearestCount(cpu / (32.0 * 32.0 * hz));
            period = np < 1 ? 1 : np > 31 ? 31 : np;
            return true;
        }
        case Voice::Buzzer:
            period = NearestCount(cpu / ((shape & 2 ? 1024.0 : 512.0) * hz));
            return period >= 1 && period <= 0xFFFF;
        default:
            period = NearestCount(cpu / (32.0 * hz));
            return period >= 1 && period <= 0xFFF;
        }
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

    void Render(int frames, Path path, float* left, float* right) {
        RunTo(tb_.CycleAt(frames));
        const bool tv = path == Path::Line && HasAy();
        // Bus 0 is the AY's three pins summed, bus 1 the beeper's pin in volts.
        float bus[2][StepBuffer::kMaxFrames] {};
        mix_.Read(0, bus[0], frames);
        mix_.Read(1, bus[1], frames);
        for (int i = 0; i < frames; ++i) {
            const double pin = bus[1][i];
            const float y = static_cast<float>(tv ? tvCoupling_.Process(bus[0][i] + kTvBeeperPerVolt * pin)
                                                  : pinBlocker_.Process(bus[0][i] + pin / (3.70 - 0.66)));
            left[i] += y;
            right[i] += y;
        }
        tb_.Advance(frames);
    }

    [[nodiscard]] const ay::Chip& Ay() const noexcept { return ay_; }
    [[nodiscard]] const zx::Beeper& Beep() const noexcept { return beeper_; }

private:
    struct Shadow {
        bool active = false;
        Voice voice = Voice::Tone;
        std::int64_t period = -1;
        int volume = -1;
        int shape = -1;
    };

    struct AySink {
        Micro* board;
        void operator()(std::int64_t cycle) const { board->AyChanged(cycle); }
    };
    struct BeeperSink {
        Micro* board;
        void operator()(std::int64_t cycle) const { board->BeeperChanged(cycle); }
    };

    static int Clamp15(int v) noexcept { return v < 0 ? 0 : v > 15 ? 15 : v; }

    // Register select at port $FFFD, data at $BFFD.
    void AyWrite(int r, std::uint8_t value) {
        AySink sink { this };
        ay_.Write(r, value, sink);
    }

    void Mixer(int c, bool tone, bool noise) {
        std::uint8_t m = ay_.Register(7);
        m = static_cast<std::uint8_t>(tone ? (m & ~(1 << c)) : (m | (1 << c)));
        m = static_cast<std::uint8_t>(noise ? (m & ~(8 << c)) : (m | (8 << c)));
        if (m != ay_.Register(7))
            AyWrite(7, m);
    }

    void Write(int channel, const ChannelSettings& s, bool start) {
        Shadow& sh = shadow_[channel];
        const bool trigger = start || !sh.active || s.voice != sh.voice;
        const int volume = Clamp15(s.volume);
        if (channel == kBeeper) {
            hl_ = s.period;
            if (volume > 0 && (!beeping_ || stopping_)) {
                if (!beeping_) {
                    // BEEPER's setup and first pass, from the call.
                    due_ = Contended(RunCursor() + 209 - 12 * (hl_ & 3));
                    ear_ = beeper_.Out() >= 2;
                }
                beeping_ = true;
                stopping_ = false;
            } else if (volume == 0 && beeping_) {
                stopping_ = true;
            }
        } else if (HasAy()) {
            switch (s.voice) {
            case Voice::Noise:
                if (trigger)
                    Mixer(channel, false, true);
                if (trigger || s.period != sh.period)
                    AyWrite(6, static_cast<std::uint8_t>(s.period));
                if (trigger || volume != sh.volume)
                    AyWrite(8 + channel, static_cast<std::uint8_t>(volume));
                break;
            case Voice::Buzzer: {
                // The envelope runs the channel; its level is the shape's,
                // so the channel is either on it (bit 4) or silent.
                static constexpr std::uint8_t kShape[4] = { 8, 12, 10, 14 };
                if (trigger)
                    Mixer(channel, false, false);
                if (trigger || s.period != sh.period) {
                    AyWrite(11, static_cast<std::uint8_t>(s.period & 0xFF));
                    AyWrite(12, static_cast<std::uint8_t>((s.period >> 8) & 0xFF));
                }
                if (trigger || (s.shape & 3) != sh.shape)
                    AyWrite(13, kShape[s.shape & 3]);
                if (trigger || (volume > 0) != (sh.volume > 0))
                    AyWrite(8 + channel, volume > 0 ? 0x10 : 0x00);
                break;
            }
            default:
                if (trigger)
                    Mixer(channel, true, false);
                if (trigger || s.period != sh.period) {
                    AyWrite(2 * channel, static_cast<std::uint8_t>(s.period & 0xFF));
                    AyWrite(2 * channel + 1, static_cast<std::uint8_t>((s.period >> 8) & 15));
                }
                if (trigger || volume != sh.volume)
                    AyWrite(8 + channel, static_cast<std::uint8_t>(volume));
                break;
            }
        }
        sh.active = true;
        sh.voice = s.voice;
        sh.period = s.period;
        sh.volume = volume;
        sh.shape = s.shape & 3;
    }

    // When an OUT ($FE) that would end at `end` uncontended ends: its I/O
    // cycle is held at end - 3 (N:1, C:3).
    [[nodiscard]] std::int64_t Contended(std::int64_t end) const noexcept {
        static constexpr int kDelay[8] = { 6, 5, 4, 3, 2, 1, 0, 0 };
        const bool is128 = revision_ == k128K;
        const std::int64_t line = is128 ? 228 : 224;
        const std::int64_t t = (end - 3) % FrameCycles() - (is128 ? 14361 : 14335);
        if (t < 0 || t >= 192 * line || t % line >= 128)
            return end;
        return end + kDelay[t % line % 8];
    }

    // The cycle the playing layer's writes land on: where the chips stand.
    [[nodiscard]] std::int64_t RunCursor() const noexcept { return beeper_.Now(); }

    void AyChanged(std::int64_t cycle) noexcept {
        double level = 0.0;
        for (int c = 0; c < 3; ++c)
            level += kAyLevel[ay_.Out(c)];
        const float levels[2] = { static_cast<float>(level), 0.0f };
        mix_.Set(0, tb_.Position(cycle, StepBuffer::kSubBits), levels);
    }

    [[nodiscard]] double PinVolts() const noexcept {
        static constexpr double kIssue3[4] = { 0.34, 0.66, 3.56, 3.70 };
        static constexpr double kIssue2[4] = { 0.39, 0.73, 3.66, 3.79 };
        return (revision_ == k48KIssue2 ? kIssue2 : kIssue3)[beeper_.Out()];
    }

    void BeeperChanged(std::int64_t cycle) noexcept {
        const float levels[2] = { 0.0f, static_cast<float>(PinVolts() - restVolts_) };
        mix_.Set(1, tb_.Position(cycle, StepBuffer::kSubBits), levels);
    }

    // Ayumi's AY_dac_table.
    static constexpr double kAyLevel[32] = {
        0.0, 0.0,
        0.00999465934234, 0.00999465934234,
        0.0144502937362, 0.0144502937362,
        0.0210574502174, 0.0210574502174,
        0.0307011520562, 0.0307011520562,
        0.0455481803616, 0.0455481803616,
        0.0644998855573, 0.0644998855573,
        0.107362478065, 0.107362478065,
        0.126588845655, 0.126588845655,
        0.20498970016, 0.20498970016,
        0.292210269322, 0.292210269322,
        0.372838941024, 0.372838941024,
        0.492530708782, 0.492530708782,
        0.635324635691, 0.635324635691,
        0.805584802014, 0.805584802014,
        1.0, 1.0,
    };

    int rate_ = 48000;
    int revision_ = k128K;
    std::int64_t clock_ = zx::kClock128;
    ay::Chip ay_;
    zx::Beeper beeper_;
    Timebase tb_;
    // The TV node's beeper, a volt on its pin, in units of one AY channel
    // at full volume there: (39 / 107) / ((68 / 107) / 3) per volt.
    static constexpr double kTvBeeperPerVolt = 3.0 * 39.0 / 68.0;
    // 1 uF and 1 uF in series (0.5 uF) around 68 + 39 kOhm.
    static constexpr double kTvCouplingHz = 1.0 / (2.0 * 3.141592653589793 * 107e3 * 0.5e-6);

    StepMix<2, 2> mix_;
    FirstOrder pinBlocker_;
    FirstOrder tvCoupling_;
    double restVolts_ = 0.0;   // the pin at rest, the beeper bus's zero
    Shadow shadow_[kChannels];
    bool beeping_ = false, stopping_ = false, ear_ = false;
    std::int64_t due_ = 0, hl_ = 0;
};

} // namespace protocodus::reboot
