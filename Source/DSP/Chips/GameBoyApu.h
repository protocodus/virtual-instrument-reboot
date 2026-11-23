#pragma once

// The APU of the Game Boy (DMG) and Game Boy Color (CGB): two pulse channels
// (the first with a period sweep), a 32-sample 4-bit wave channel and a noise
// channel, clocked at 4194304 Hz. Everything here is from Pan Docs (Audio,
// Audio Registers, Audio details), read 2026-11-17 and CC0; the step map of
// the 512 Hz DIV-APU sequencer is the gbdev wiki's ("Game Boy sound
// hardware", Frame Sequencer).
//
// Like every core here it is integers only and jumps from event to event.
// Out() gives each channel's digital output (0-15), DacOn() whether its DAC
// is on; the board turns those into analog levels (Boards/Handheld.h).
//
// A disabled channel's generators stand still until it is triggered again,
// its frequency timer holding the count it stopped on.
//
// Reported additions (Lior Halphon's SameBoy, Expat/MIT license, read for the
// timing rules it states, no code copied): CH3's 6-T-cycle trigger delay,
// the 2-T-cycle DMG Wave RAM retrigger corruption window, and the original's
// "zombie" volume writes (Zombie).

#include "Lfsr.h"
#include "Timer.h"

#include <cstdint>

namespace protocodus::reboot::gb {

enum class Model { Dmg, Cgb };

constexpr std::int64_t kClock = 4194304;          // Hz
constexpr std::int64_t kDivApuPeriod = kClock / 512;

// Pan Docs, NR11: the duty waveforms, duty step 0 first.
constexpr std::uint8_t kDuty[4][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 0, 1, 1, 1 },
    { 0, 1, 1, 1, 1, 1, 1, 0 },
};

// Pan Docs, Noise channel: bit 15 takes bit 0 XNOR bit 1 (and bit 7 too in
// short mode), then the register shifts right and the bit shifted out is the
// output. Here bits 0-14 are the register and bit 15 holds that output.
constexpr std::uint32_t NoiseStep(std::uint32_t s, bool shortMode) {
    const std::uint32_t b = 1u ^ ((s ^ (s >> 1)) & 1u);
    std::uint32_t reg = ((s & 0x7FFFu) >> 1) | (b << 14);
    if (shortMode)
        reg = (reg & ~(1u << 6)) | (b << 6);
    return reg | ((s & 1u) << 15);
}
constexpr std::uint32_t NoiseStepLong(std::uint32_t s) { return NoiseStep(s, false); }
constexpr std::uint32_t NoiseStepShort(std::uint32_t s) { return NoiseStep(s, true); }
inline constexpr LfsrJump kNoiseJump[2] = { LfsrJump(NoiseStepLong), LfsrJump(NoiseStepShort) };

class Apu {
public:
    enum Channel { kPulse1, kPulse2, kWave, kNoise, kChannels };

    void Reset(Model model) noexcept {
        *this = Apu {};
        model_ = model;
        regs_[0x14] = nr50_;
        regs_[0x15] = nr51_;
        Update();
    }

    [[nodiscard]] std::int64_t Now() const noexcept { return now_; }
    [[nodiscard]] int Out(int channel) const noexcept { return out_[channel]; }
    [[nodiscard]] bool DacOn(int channel) const noexcept { return dac_[channel]; }
    [[nodiscard]] std::uint8_t Panning() const noexcept { return nr51_; }   // NR51
    [[nodiscard]] std::uint8_t MasterVolume() const noexcept { return nr50_; }  // NR50

    template <class Sink>
    void Run(std::int64_t to, Sink& sink) {
        for (;;) {
            std::int64_t next = Earliest(to, divDue_);
            for (const Square& s : square_)
                if (s.live)
                    next = Earliest(next, s.timer.due);
            if (wave_.live)
                next = Earliest(next, wave_.timer.due);
            if (noise_.live)
                next = Earliest(next, noise_.timer.due);
            if (next >= to)
                break;
            now_ = next;
            if (divDue_ == now_)
                DivApuStep();
            for (Square& s : square_)
                if (s.live && s.timer.due == now_) {
                    s.StepDuty(1);
                    s.timer.due += s.timer.period;
                }
            if (wave_.live && wave_.timer.due == now_) {
                wave_.timer.due += wave_.timer.period;
                WaveStep(1);
            }
            if (noise_.live && noise_.timer.due == now_) {
                noise_.shift = NoiseStep(noise_.shift, noise_.shortMode);
                noise_.timer.due += noise_.timer.period;
            }
            if (Update())
                sink(now_);
        }
        now_ = to;
    }

    // A CPU write to $FF10-$FF3F at Now().
    template <class Sink>
    void Write(std::uint16_t address, std::uint8_t value, Sink& sink) {
        CatchUpAll();
        if (address >= 0xFF30 && address <= 0xFF3F) {
            // Pan Docs, Audio Registers ($FF30-$FF3F): while CH3 is active, CGB
            // redirects Wave RAM access to the byte CH3 is playing, while DMG
            // ignores accesses outside the 2-T-cycle read window.
            if (!wave_.on)
                waveRam_[address - 0xFF30] = value;
            else if (model_ == Model::Cgb)
                waveRam_[wave_.position >> 1] = value;
            else if (now_ - wave_.lastRead < 2)
                waveRam_[wave_.position >> 1] = value;
        } else if (address == 0xFF26) {
            const bool on = (value & 0x80) != 0;
            if (!on && power_) {
                PowerOff();
                // PowerOff clears the DAC and output caches together with
                // their registers. The board needs the edge at this cycle.
                sink(now_);
                return;
            }
            // gbdev wiki, Power control: powering on resets the sequencer so
            // its next step is 0, and clears the wave sample buffer.
            if (on && !power_) {
                divStep_ = 0;
                wave_.sample = 0;
                wave_.buffer = 0;
            }
            power_ = on;
        } else if (power_) {
            WriteRegister(address, value);
            if (address >= 0xFF10 && address <= 0xFF25)
                regs_[address - 0xFF10] = value;
        } else if (model_ == Model::Dmg) {
            // Pan Docs, NR52: on DMG, length counters remain writable while powered off.
            if (address == 0xFF11 || address == 0xFF16)
                square_[address == 0xFF11 ? 0 : 1].length.remaining = 64 - (value & 0x3F);
            else if (address == 0xFF1B)
                wave_.length.remaining = 256 - value;
            else if (address == 0xFF20)
                noise_.length.remaining = 64 - (value & 0x3F);
        }
        if (Update())
            sink(now_);
    }

    // A CPU read from $FF10-$FF3F at Now() (Pan Docs, Audio Registers).
    [[nodiscard]] std::uint8_t Read(std::uint16_t address) noexcept {
        CatchUpAll();
        if (address >= 0xFF30 && address <= 0xFF3F) {
            if (!wave_.on)
                return waveRam_[address - 0xFF30];
            if (model_ == Model::Cgb)
                return waveRam_[wave_.position >> 1];
            return (now_ - wave_.lastRead < 2) ? waveRam_[wave_.position >> 1] : 0xFF;
        }
        if (address == 0xFF26) {
            return static_cast<std::uint8_t>((power_ ? 0x80 : 0x00) | 0x70 | (square_[0].on ? 1 : 0)
                                             | (square_[1].on ? 2 : 0) | (wave_.on ? 4 : 0) | (noise_.on ? 8 : 0));
        }
        if (address >= 0xFF10 && address <= 0xFF25) {
            static constexpr std::uint8_t kReadMask[22] = {
                0x80, 0x3F, 0x00, 0xFF, 0xBF,
                0xFF, 0x3F, 0x00, 0xFF, 0xBF,
                0x7F, 0xFF, 0x9F, 0xFF, 0xBF,
                0xFF, 0xFF, 0x00, 0x00, 0xBF,
                0x00, 0x00,
            };
            return static_cast<std::uint8_t>(regs_[address - 0xFF10] | kReadMask[address - 0xFF10]);
        }
        return 0xFF;
    }

private:
    struct Envelope {
        std::uint8_t reg = 0;                  // NRx2
        std::uint8_t volume = 0, timer = 0;
        bool running = false;
        bool halted = false;
        [[nodiscard]] int Pace() const noexcept { return reg & 7; }
        [[nodiscard]] bool Up() const noexcept { return (reg & 8) != 0; }
        void Trigger(bool nextStepClocksEnvelope) noexcept {
            volume = reg >> 4;
            // Pan Docs, Obscure Behavior: a period of 0 counts as 8, and a
            // trigger just before an envelope step reloads one more.
            timer = static_cast<std::uint8_t>((Pace() ? Pace() : 8) + (nextStepClocksEnvelope ? 1 : 0));
            running = Pace() != 0;
            halted = false;
        }
        void Clock() noexcept {
            if (!running || Pace() == 0)
                return;
            if (--timer == 0) {
                timer = static_cast<std::uint8_t>(Pace());
                if (Up() && volume < 15)
                    ++volume;
                else if (!Up() && volume > 0)
                    --volume;
                else {
                    running = false;
                    halted = true;
                }
            }
        }
    };

    struct Length {
        int remaining = 0;                     // steps until the channel is cut
        bool enabled = false;
    };

    struct Square {
        bool on = false, started = false, firstStep = false;
        std::uint8_t duty = 0, step = 0;
        std::uint16_t period = 0;
        Envelope envelope;
        Length length;
        Divider timer { 0, 4 * 2048 };
        bool live = false, wasOn = false;
        std::int64_t held = 0;                 // the timer's count while off
        void SetPeriod(int p) noexcept {
            period = static_cast<std::uint16_t>(p & 0x7FF);
            timer.period = 4 * (2048 - period);   // 1048576 Hz divider clock
        }
        void StepDuty(std::int64_t k) noexcept {
            if (!started || k == 0)
                return;
            firstStep = false;
            step = static_cast<std::uint8_t>((step + k) & 7);
        }
    };

    struct Wave {
        bool on = false, dac = false;
        std::uint8_t level = 0, position = 0, sample = 0;   // NR32 bits 6-5
        std::uint8_t buffer = 0;                  // the sample buffer: the last byte read
        std::uint16_t period = 0;
        std::int64_t lastRead = -10;
        Length length;
        Divider timer { 0, 2 * 2048 };
        bool live = false;
        void SetPeriod(int p) noexcept {
            period = static_cast<std::uint16_t>(p & 0x7FF);
            timer.period = 2 * (2048 - period);   // 2097152 Hz divider clock
        }
    };

    struct Noise {
        bool on = false, shortMode = false;
        std::uint8_t reg = 0;                     // NR43
        std::uint32_t shift = 0;
        Envelope envelope;
        Length length;
        Divider timer { 0, 8 };
        bool live = false;
        [[nodiscard]] bool Clocked() const noexcept { return (reg >> 4) < 14; }
        void SetRegister(std::uint8_t value) noexcept {
            reg = value;
            shortMode = (value & 8) != 0;
            // Pan Docs, NR43: 262144 / (divider x 2^shift) Hz, divider 0
            // counting as 0.5.
            const int divider = value & 7;
            const int shiftBits = value >> 4;
            timer.period = static_cast<std::int64_t>(divider ? 16 * divider : 8) << (shiftBits < 14 ? shiftBits : 0);
        }
    };

    void WriteRegister(std::uint16_t address, std::uint8_t value) noexcept {
        switch (address) {
        case 0xFF10: {
            Square& s = square_[0];
            // Pan Docs, Obscure Behavior: leaving subtraction after a
            // subtraction since the trigger turns the channel off.
            if (sweepNegated_ && (sweepReg_ & 8) && !(value & 8))
                s.on = false;
            // A pace of 0 stops iterations at once; any other value waits for
            // the next iteration or trigger.
            if ((value & 0x70) == 0)
                sweepPace_ = 0;
            else if (sweepPace_ == 0)
                sweepPace_ = (value >> 4) & 7, sweepTimer_ = static_cast<std::uint8_t>(sweepPace_);
            sweepReg_ = value;
            break;
        }
        case 0xFF11: case 0xFF16: {
            Square& s = square_[address == 0xFF11 ? 0 : 1];
            s.duty = value >> 6;
            s.length.remaining = 64 - (value & 0x3F);
            break;
        }
        case 0xFF12: case 0xFF17: {
            Square& s = square_[address == 0xFF12 ? 0 : 1];
            if (s.on)
                Zombie(model_, s.envelope, value);
            else
                s.envelope.reg = value;
            if ((value & 0xF8) == 0)
                s.on = false;
            break;
        }
        case 0xFF13: case 0xFF18: {
            Square& s = square_[address == 0xFF13 ? 0 : 1];
            s.SetPeriod((s.period & 0x700) | value);
            break;
        }
        case 0xFF14: case 0xFF19: {
            const int i = address == 0xFF14 ? 0 : 1;
            Square& s = square_[i];
            s.SetPeriod((s.period & 0xFF) | ((value & 7) << 8));
            LengthWrite(s.length, s.on, value);
            if (value & 0x80)
                TriggerSquare(i);
            break;
        }
        case 0xFF1A:
            wave_.dac = (value & 0x80) != 0;
            if (!wave_.dac)
                wave_.on = false;
            break;
        case 0xFF1B:
            wave_.length.remaining = 256 - value;
            break;
        case 0xFF1C:
            wave_.level = (value >> 5) & 3;
            break;
        case 0xFF1D:
            wave_.SetPeriod((wave_.period & 0x700) | value);
            break;
        case 0xFF1E:
            wave_.SetPeriod((wave_.period & 0xFF) | ((value & 7) << 8));
            LengthWrite(wave_.length, wave_.on, value);
            if (value & 0x80)
                TriggerWave();
            break;
        case 0xFF20:
            noise_.length.remaining = 64 - (value & 0x3F);
            break;
        case 0xFF21:
            if (noise_.on)
                Zombie(model_, noise_.envelope, value);
            else
                noise_.envelope.reg = value;
            if ((value & 0xF8) == 0)
                noise_.on = false;
            break;
        case 0xFF22:
            noise_.SetRegister(value);
            break;
        case 0xFF23:
            LengthWrite(noise_.length, noise_.on, value);
            if (value & 0x80)
                TriggerNoise();
            break;
        case 0xFF24:
            nr50_ = value;
            break;
        case 0xFF25:
            nr51_ = value;
            break;
        default:
            break;
        }
    }

    // An NRx2 write while the channel plays ("zombie" mode). On CGB, Pan
    // Docs' (Obscure Behavior) CGB-04 rules. On DMG, SameBoy's (Core/apu.c,
    // nrx2_glitch, for models up to CGB-C; reported): the write acts as two,
    // the old value to $FF and then $FF to the new, each by one rule
    // (ZombieStep). It keeps Pan Docs' $x8-on-$x8 increment, which SameBoy
    // notes "always work[s] as expected", while "$x0 writes seem to be
    // non-deterministic" on the original (README, Known gaps). SameBoy also
    // reloads the step timer when the envelope's clock is high, a phase the
    // envelope here does not model.
    static void Zombie(Model model, Envelope& e, std::uint8_t value) noexcept {
        if (model == Model::Cgb) {
            int v = e.volume;
            if ((e.reg & 7) == 0 && !e.halted)
                v += 1;
            else if (!(e.reg & 8))
                v += 2;
            if ((e.reg ^ value) & 8)
                v = 16 - v;
            e.volume = static_cast<std::uint8_t>(v & 15);
        } else {
            const int v = ZombieStep(ZombieStep(e.volume, e.reg, 0xFF, e.halted), 0xFF, value, e.halted);
            e.volume = static_cast<std::uint8_t>(v);
        }
        e.reg = value;
    }

    // One half of the original's zombie write, NRx2 from `from` to `to`:
    // the volume counts one step in the new direction when a pace appears
    // or $x8 meets $x8; a change of direction mirrors it instead (to 15 - v,
    // or 14 - v, when climbing; to 16 - v when falling). A halted envelope
    // (one that reached its end) does not count, and mirrors as if a pace
    // had been there.
    static int ZombieStep(int v, int from, int to, bool halted) noexcept {
        bool tick = !halted && (((to & 7) && !(from & 7)) || ((to & 15) == 8 && (from & 15) == 8));
        if ((to ^ from) & 8) {
            if (to & 8) {
                v = !(from & 7) && !halted ? 15 - v : 14 - v;
                tick = false;
            } else {
                v = 16 - v;
            }
        }
        if (tick)
            v += (to & 8) ? 1 : -1;
        return v & 15;
    }

    [[nodiscard]] bool NextStepClocksLength() const noexcept { return (divStep_ & 1) == 0; }

    // Pan Docs, Obscure Behavior: the extra length clock when length is
    // enabled while the sequencer's next step will not clock it.
    void LengthWrite(Length& length, bool& on, std::uint8_t value) noexcept {
        const bool enable = (value & 0x40) != 0;
        if (!NextStepClocksLength() && !length.enabled && enable && length.remaining > 0) {
            if (--length.remaining == 0 && !(value & 0x80))
                on = false;
        }
        length.enabled = enable;
    }

    // Pan Docs, Obscure Behavior: a trigger that reloads an expired length
    // while the next step will not clock it loads one less.
    void LengthTrigger(Length& length, int full) const noexcept {
        if (length.remaining == 0)
            length.remaining = (length.enabled && !NextStepClocksLength()) ? full - 1 : full;
    }

    void TriggerSquare(int i) noexcept {
        Square& s = square_[i];
        LengthTrigger(s.length, 64);
        s.envelope.Trigger(divStep_ == 7);
        // Pan Docs, Obscure Behavior: "the low two bits of the frequency
        // timer are NOT modified" by a trigger. The timer counts master
        // cycles to its next duty step; its reload, 4 (2048 - x), has none.
        const std::int64_t count = s.on ? s.timer.due - now_ : s.held;
        s.timer.due = now_ + s.timer.period + (count & 3);
        if (!s.started) {
            // Pan Docs, Obscure Behavior: the first duty step after the first
            // trigger plays as 0, and the duty clock waits for that trigger.
            s.started = true;
            s.firstStep = true;
        }
        s.on = (s.envelope.reg & 0xF8) != 0;
        if (i == 0) {
            shadow_ = s.period;
            sweepPace_ = (sweepReg_ >> 4) & 7;
            sweepTimer_ = static_cast<std::uint8_t>(sweepPace_ ? sweepPace_ : 8);
            sweepEnabled_ = (sweepReg_ & 0x77) != 0;
            sweepNegated_ = false;
            if (sweepReg_ & 7)
                SweepCalculate();
        }
    }

    void TriggerWave() noexcept {
        LengthTrigger(wave_.length, 256);
        // Pan Docs, Obscure Behavior: on the original handheld a trigger as
        // the channel reads a byte rewrites the start of wave RAM.
        // SameBoy (MIT): the DMG bug triggers in the 2-T-cycle APU tick window
        // before the read, and on a read-tick collision reloads byte 0.
        if (model_ == Model::Dmg && wave_.on && wave_.timer.due - now_ <= 2) {
            const int byte = ((wave_.position + 1) & 31) >> 1;
            if (byte < 4) {
                waveRam_[0] = waveRam_[byte];
            } else {
                const int base = byte & ~3;
                for (int j = 0; j < 4; ++j)
                    waveRam_[j] = waveRam_[base + j];
            }
            if (wave_.timer.due == now_)
                wave_.buffer = waveRam_[0];
        }
        // Pan Docs, Obscure Behavior: a trigger does not read wave RAM, so
        // the first sample is "the previous one still in the high nibble of
        // the sample buffer", and the next is index 1.
        // Reported (SameBoy, MIT): hardware delays the first sample step by 6 T-cycles.
        wave_.position = 0;
        wave_.sample = wave_.buffer >> 4;
        wave_.timer.due = now_ + wave_.timer.period + 6;
        wave_.on = wave_.dac;
    }

    void TriggerNoise() noexcept {
        LengthTrigger(noise_.length, 64);
        noise_.envelope.Trigger(divStep_ == 7);
        noise_.shift = 0;
        noise_.timer.due = now_ + noise_.timer.period;
        noise_.on = (noise_.envelope.reg & 0xF8) != 0;
    }

    // Pan Docs, Pulse channel with sweep: the next period from the shadow
    // register; above $7FF the channel is turned off.
    int SweepCalculate() noexcept {
        const int change = shadow_ >> (sweepReg_ & 7);
        int next;
        if (sweepReg_ & 8) {
            next = shadow_ - change;
            sweepNegated_ = true;
        } else {
            next = shadow_ + change;
        }
        if (next > 0x7FF)
            square_[0].on = false;
        return next;
    }

    // Pan Docs, NR10: an iteration every pace ticks of 128 Hz; the pace
    // written is read again only after an iteration or a trigger.
    void SweepClock() noexcept {
        if (--sweepTimer_ > 0)
            return;
        if (sweepEnabled_ && sweepPace_ != 0) {
            const int next = SweepCalculate();
            if (next <= 0x7FF && (sweepReg_ & 7)) {
                shadow_ = static_cast<std::uint16_t>(next);
                square_[0].SetPeriod(next);
                SweepCalculate();
            }
        }
        sweepPace_ = (sweepReg_ >> 4) & 7;
        sweepTimer_ = static_cast<std::uint8_t>(sweepPace_ ? sweepPace_ : 8);
    }

    void LengthClock(Length& length, bool& on) noexcept {
        if (length.enabled && length.remaining > 0 && --length.remaining == 0)
            on = false;
    }

    // The 512 Hz sequencer: length on even steps, sweep on 2 and 6, the
    // envelopes on 7 (gbdev wiki, Frame Sequencer).
    void DivApuStep() noexcept {
        CatchUpAll();
        if (power_) {
            if ((divStep_ & 1) == 0) {
                LengthClock(square_[0].length, square_[0].on);
                LengthClock(square_[1].length, square_[1].on);
                LengthClock(wave_.length, wave_.on);
                LengthClock(noise_.length, noise_.on);
            }
            if (divStep_ == 2 || divStep_ == 6)
                SweepClock();
            if (divStep_ == 7) {
                square_[0].envelope.Clock();
                square_[1].envelope.Clock();
                noise_.envelope.Clock();
            }
        }
        divStep_ = (divStep_ + 1) & 7;
        divDue_ += kDivApuPeriod;
    }

    void WaveStep(std::int64_t k) noexcept {
        if (k == 0)
            return;
        wave_.position = static_cast<std::uint8_t>((wave_.position + k) & 31);
        wave_.buffer = waveRam_[wave_.position >> 1];
        wave_.sample = (wave_.position & 1) ? (wave_.buffer & 0x0F) : (wave_.buffer >> 4);
        wave_.lastRead = wave_.timer.due - wave_.timer.period;
    }

    void CatchUpAll() noexcept {
        for (Square& s : square_)
            if (!s.live && s.on)
                s.StepDuty(s.timer.CatchUp(now_));
        if (!wave_.live && wave_.on)
            WaveStep(wave_.timer.CatchUp(now_));
        if (!noise_.live && noise_.on) {
            if (!noise_.Clocked()) {
                // Shifts 14 and 15 give the register no clocks; its divider
                // is held here, as no source says what it does meanwhile.
                if (noise_.timer.due < now_)
                    noise_.timer.due = now_;
            } else if (const std::int64_t k = noise_.timer.CatchUp(now_)) {
                noise_.shift = kNoiseJump[noise_.shortMode ? 1 : 0].Advance(noise_.shift, static_cast<std::uint64_t>(k));
            }
        }
    }

    // Pan Docs, NR52: turning the APU off clears its registers and the duty
    // steps; wave RAM is kept, and on DMG the length counters survive too.
    void PowerOff() noexcept {
        std::uint8_t ram[16];
        for (int i = 0; i < 16; ++i)
            ram[i] = waveRam_[i];
        const int len[4] = {
            square_[0].length.remaining,
            square_[1].length.remaining,
            wave_.length.remaining,
            noise_.length.remaining,
        };
        const std::int64_t now = now_, divDue = divDue_;
        const int divStep = divStep_;
        const Model model = model_;
        *this = Apu {};
        model_ = model;
        now_ = now, divDue_ = divDue, divStep_ = divStep;
        power_ = false;
        nr50_ = lastNr50_ = 0;
        nr51_ = lastNr51_ = 0;
        for (int i = 0; i < 16; ++i)
            waveRam_[i] = ram[i];
        if (model_ == Model::Dmg) {
            square_[0].length.remaining = len[0];
            square_[1].length.remaining = len[1];
            wave_.length.remaining = len[2];
            noise_.length.remaining = len[3];
        }
    }

    bool Update() noexcept {
        int codes[kChannels];
        bool dacs[kChannels];
        for (int i = 0; i < 2; ++i) {
            Square& s = square_[i];
            dacs[i] = (s.envelope.reg & 0xF8) != 0;
            if (!dacs[i])
                s.on = false;
            // A channel turned off keeps its timer's count for the next
            // trigger; every path here has caught the timer up first.
            if (s.wasOn && !s.on)
                s.held = s.timer.due - now_;
            s.wasOn = s.on;
            const bool live = s.on && s.envelope.volume > 0;
            if (live && !s.live)
                s.StepDuty(s.timer.CatchUp(now_));
            s.live = live;
            codes[i] = live && !s.firstStep && kDuty[s.duty][s.step] ? s.envelope.volume : 0;
        }
        {
            dacs[kWave] = wave_.dac;
            const bool live = wave_.on && wave_.level != 0;
            if (live && !wave_.live)
                WaveStep(wave_.timer.CatchUp(now_));
            wave_.live = live;
            codes[kWave] = live ? wave_.sample >> (wave_.level - 1) : 0;
        }
        {
            dacs[kNoise] = (noise_.envelope.reg & 0xF8) != 0;
            if (!dacs[kNoise])
                noise_.on = false;
            const bool live = noise_.on && noise_.envelope.volume > 0 && noise_.Clocked();
            if (live && !noise_.live) {
                const std::int64_t k = noise_.timer.CatchUp(now_);
                if (k)
                    noise_.shift = kNoiseJump[noise_.shortMode ? 1 : 0].Advance(noise_.shift, static_cast<std::uint64_t>(k));
            }
            noise_.live = live;
            codes[kNoise] = noise_.on && (noise_.shift & 0x8000u) ? noise_.envelope.volume : 0;
        }
        bool changed = false;
        for (int i = 0; i < kChannels; ++i) {
            changed |= codes[i] != out_[i] || dacs[i] != dac_[i];
            out_[i] = static_cast<std::uint8_t>(codes[i]);
            dac_[i] = dacs[i];
        }
        changed |= nr50_ != lastNr50_ || nr51_ != lastNr51_;
        lastNr50_ = nr50_, lastNr51_ = nr51_;
        return changed;
    }

    Model model_ = Model::Dmg;
    std::int64_t now_ = 0;
    std::int64_t divDue_ = kDivApuPeriod;
    int divStep_ = 0;
    bool power_ = true;
    Square square_[2];
    Wave wave_;
    Noise noise_;
    std::uint8_t sweepReg_ = 0, sweepPace_ = 0, sweepTimer_ = 8;
    std::uint16_t shadow_ = 0;
    bool sweepEnabled_ = false, sweepNegated_ = false;
    std::uint8_t nr50_ = 0x77, nr51_ = 0xFF, lastNr50_ = 0x77, lastNr51_ = 0xFF;
    std::uint8_t regs_[22] {};
    std::uint8_t waveRam_[16] {};
    std::uint8_t out_[kChannels] {};
    bool dac_[kChannels] {};
};

} // namespace protocodus::reboot::gb
