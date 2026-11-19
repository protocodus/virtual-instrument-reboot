#pragma once

// The Famicom's cartridge sound chips, each clocked in CPU cycles like the
// 2A03 beside it: Konami's VRC6, Nintendo's MMC5, Namco's 163 and the Famicom
// Disk System's wave unit. Sunsoft's 5B is the AY core (Chips/Ay8910.h) with
// a 16-cycle tick. Everything here is from the NESdev Wiki (VRC6 audio, MMC5
// audio, Namco 163 audio, FDS audio, Sunsoft 5B audio), read 2026-11-17.
//
// Out() gives each chip's own output codes; how loud each is against the
// 2A03, and its polarity, is the board's (Boards/Console.h), from what the
// same pages report.

#include "NesApu.h"
#include "Timer.h"

#include <cstdint>
#include <initializer_list>

namespace protocodus::reboot::famicom {

// ---------------------------------------------------------------------------
// VRC6: two 16-step pulses and an accumulating sawtooth.
class Vrc6 {
public:
    enum Channel { kPulse1, kPulse2, kSaw, kChannels };

    void Reset() noexcept {
        *this = Vrc6 {};
        Update();
    }

    [[nodiscard]] std::int64_t Now() const noexcept { return now_; }
    [[nodiscard]] int Out(int channel) const noexcept { return out_[channel]; }

    template <class Sink>
    void Run(std::int64_t to, Sink& sink) {
        for (;;) {
            std::int64_t next = to;
            for (const Pulse& p : pulse_)
                if (p.live)
                    next = Earliest(next, p.timer.due);
            if (saw_.live)
                next = Earliest(next, saw_.timer.due);
            if (next >= to)
                break;
            now_ = next;
            for (Pulse& p : pulse_)
                if (p.live && p.timer.due == now_) {
                    p.step = (p.step - 1) & 15;
                    p.timer.due += p.timer.period;
                }
            if (saw_.live && saw_.timer.due == now_) {
                SawClock();
                saw_.timer.due += saw_.timer.period;
            }
            if (Update())
                sink(now_);
        }
        now_ = to;
    }

    // A write to $9000-$B002 (VRC6a addressing; the board swaps A0 and A1
    // for VRC6b) at Now().
    template <class Sink>
    void Write(std::uint16_t address, std::uint8_t value, Sink& sink) {
        CatchUpAll();
        const int unit = (address >> 12) - 9;        // 0, 1: pulses; 2: saw
        const int reg = address & 3;
        if (unit == 0 && reg == 3) {
            // $9003: halt (overrides), 256x (overrides 16x), 16x.
            const bool halt = (value & 1) != 0;
            if (halt != halt_) {
                for (Divider* d : { &pulse_[0].timer, &pulse_[1].timer, &saw_.timer }) {
                    if (halt)
                        d->due -= now_;      // the time it still had to run
                    else
                        d->due += now_;
                }
            }
            halt_ = halt;
            shift_ = (value & 4) ? 8 : (value & 2) ? 4 : 0;
            for (Pulse& p : pulse_)
                p.timer.period = (p.period >> shift_) + 1;
            saw_.timer.period = (saw_.period >> shift_) + 1;
        } else if (unit == 0 || unit == 1) {
            Pulse& p = pulse_[unit];
            if (reg == 0) {
                p.mode = (value & 0x80) != 0;
                p.duty = (value >> 4) & 7;
                p.volume = value & 15;
            } else if (reg == 1) {
                p.period = (p.period & 0xF00) | value;
            } else if (reg == 2) {
                p.period = (p.period & 0xFF) | ((value & 15) << 8);
                const bool enable = (value & 0x80) != 0;
                // Clearing E halts the duty generator; it restarts from the
                // beginning when E is set again.
                if (enable && !p.enabled)
                    p.step = 15;
                p.enabled = enable;
            }
            p.timer.period = (p.period >> shift_) + 1;
        } else if (unit == 2) {
            if (reg == 0) {
                saw_.rate = value & 0x3F;
            } else if (reg == 1) {
                saw_.period = (saw_.period & 0xF00) | value;
            } else if (reg == 2) {
                saw_.period = (saw_.period & 0xFF) | ((value & 15) << 8);
                saw_.enabled = (value & 0x80) != 0;
                // Clearing E zeroes the accumulator and resets its 14-clock phase
                // counter, but does not reset the divider (NESdev, VRC6 audio).
                if (!saw_.enabled) {
                    saw_.accumulator = 0;
                    saw_.clocks = 0;
                }
            }
            saw_.timer.period = (saw_.period >> shift_) + 1;
        }
        if (Update())
            sink(now_);
    }

private:
    struct Pulse {
        bool mode = false, enabled = false;
        std::uint8_t duty = 0, volume = 0, step = 15;
        std::uint16_t period = 0;
        Divider timer { 0, 1 };
        bool live = false;
    };
    struct Saw {
        bool enabled = false;
        std::uint8_t rate = 0, accumulator = 0, clocks = 0;
        std::uint16_t period = 0;
        Divider timer { 0, 1 };
        bool live = false;
    };

    // The accumulator reacts on every second clock: six additions of the
    // rate, then a reset on the seventh (14 clocks to a cycle).
    void SawClock() noexcept {
        saw_.clocks = static_cast<std::uint8_t>((saw_.clocks + 1) % 14);
        if (saw_.clocks == 0)
            saw_.accumulator = 0;
        else if ((saw_.clocks & 1) == 0 && saw_.enabled)
            saw_.accumulator = static_cast<std::uint8_t>(saw_.accumulator + saw_.rate);
    }

    void CatchUpAll() noexcept {
        if (halt_)
            return;
        for (Pulse& p : pulse_)
            if (!p.live) {
                const std::int64_t k = p.timer.CatchUp(now_);
                if (p.enabled)
                    p.step = static_cast<std::uint8_t>((p.step - k) & 15);
            }
        // Silent means disabled, or rate 0 with the accumulator at 0, so
        // the accumulator stays where it is.
        if (!saw_.live)
            saw_.clocks = static_cast<std::uint8_t>((saw_.clocks + saw_.timer.CatchUp(now_)) % 14);
    }

    bool Update() noexcept {
        int codes[kChannels];
        for (int i = 0; i < 2; ++i) {
            Pulse& p = pulse_[i];
            const bool live = !halt_ && p.enabled && !p.mode && p.volume > 0;
            if (live && !p.live)
                p.step = static_cast<std::uint8_t>((p.step - p.timer.CatchUp(now_)) & 15);
            p.live = live;
            codes[i] = p.enabled && (p.mode || p.step <= p.duty) ? p.volume : 0;
        }
        {
            // A rate of 0 leaves the accumulator where it was until the
            // cycle's reset, so the saw is followed until it is back at 0.
            const bool live = !halt_ && saw_.enabled && (saw_.rate > 0 || saw_.accumulator != 0);
            if (live && !saw_.live)
                saw_.clocks = static_cast<std::uint8_t>((saw_.clocks + saw_.timer.CatchUp(now_)) % 14);
            saw_.live = live;
            codes[kSaw] = saw_.enabled ? saw_.accumulator >> 3 : 0;
        }
        bool changed = false;
        for (int i = 0; i < kChannels; ++i) {
            changed |= codes[i] != out_[i];
            out_[i] = static_cast<std::uint8_t>(codes[i]);
        }
        return changed;
    }

    std::int64_t now_ = 0;
    Pulse pulse_[2];
    Saw saw_;
    bool halt_ = false;
    int shift_ = 0;
    std::uint8_t out_[kChannels] {};
};

// ---------------------------------------------------------------------------
// MMC5: two pulses as the 2A03's without sweep, and an 8-bit PCM register.
class Mmc5 {
public:
    enum Channel { kPulse1, kPulse2, kPcm, kChannels };
    // M2 / 7424, about 241 Hz: the envelope and length clock "die images
    // suggest", not bench-confirmed (NESdev, MMC5 audio).
    static constexpr std::int64_t kFramePeriod = 7424;

    void Reset() noexcept {
        *this = Mmc5 {};
        frameDue_ = kFramePeriod;
        Update();
    }

    [[nodiscard]] std::int64_t Now() const noexcept { return now_; }
    [[nodiscard]] int Out(int channel) const noexcept { return out_[channel]; }

    template <class Sink>
    void Run(std::int64_t to, Sink& sink) {
        for (;;) {
            std::int64_t next = Earliest(to, frameDue_);
            for (const Pulse& p : pulse_)
                if (p.live)
                    next = Earliest(next, p.timer.due);
            if (next >= to)
                break;
            now_ = next;
            if (frameDue_ == now_) {
                CatchUpAll();
                for (Pulse& p : pulse_) {
                    p.envelope.Clock(p.halt);
                    if (!p.halt && p.length > 0)
                        --p.length;
                }
                frameDue_ += kFramePeriod;
            }
            for (Pulse& p : pulse_)
                if (p.live && p.timer.due == now_) {
                    p.step = (p.step - 1) & 7;
                    p.timer.due += p.timer.period;
                }
            if (Update())
                sink(now_);
        }
        now_ = to;
    }

    template <class Sink>
    void Write(std::uint16_t address, std::uint8_t value, Sink& sink) {
        CatchUpAll();
        if (address >= 0x5000 && address <= 0x5007) {
            Pulse& p = pulse_[(address >> 2) & 1];
            switch (address & 3) {
            case 0:
                p.duty = value >> 6;
                p.halt = (value & 0x20) != 0;
                p.envelope.constant = (value & 0x10) != 0;
                p.envelope.volume = value & 0x0F;
                break;
            case 2:
                p.SetPeriod((p.period & 0x700) | value);
                break;
            case 3:
                p.SetPeriod((p.period & 0xFF) | ((value & 7) << 8));
                if (enabled_ & (1 << ((address >> 2) & 1)))
                    p.length = nes::kLength[value >> 3];
                p.step = 0;
                p.envelope.start = true;
                break;
            default:                       // $5001, $5005: no sweep
                break;
            }
        } else if (address == 0x5010) {
            readMode_ = (value & 1) != 0;
        } else if (address == 0x5011) {
            // Writing $00 leaves the DAC; writes are ignored in read mode.
            if (!readMode_ && value != 0)
                pcm_ = value;
        } else if (address == 0x5015) {
            enabled_ = value & 3;
            for (int i = 0; i < 2; ++i)
                if (!(value & (1 << i)))
                    pulse_[i].length = 0;
        }
        if (Update())
            sink(now_);
    }

private:
    struct Envelope {
        bool start = false, constant = false;
        std::uint8_t volume = 0, divider = 0, decay = 0;
        [[nodiscard]] int Level() const noexcept { return constant ? volume : decay; }
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
        std::uint8_t duty = 0, step = 0, length = 0;
        bool halt = false;
        Envelope envelope;
        std::uint16_t period = 0;
        Divider timer { 0, 2 };
        bool live = false;
        void SetPeriod(int t) noexcept {
            period = static_cast<std::uint16_t>(t);
            timer.period = 2 * (t + 1);
        }
    };

    void CatchUpAll() noexcept {
        for (Pulse& p : pulse_)
            if (!p.live)
                p.step = static_cast<std::uint8_t>((p.step - p.timer.CatchUp(now_)) & 7);
    }

    bool Update() noexcept {
        int codes[kChannels];
        for (int i = 0; i < 2; ++i) {
            Pulse& p = pulse_[i];
            const int level = p.envelope.Level();
            // Periods under 8 do not silence these pulses.
            const bool live = p.length > 0 && level > 0;
            if (live && !p.live)
                p.step = static_cast<std::uint8_t>((p.step - p.timer.CatchUp(now_)) & 7);
            p.live = live;
            codes[i] = live && nes::kDuty[p.duty][p.step] ? level : 0;
        }
        codes[kPcm] = pcm_;
        bool changed = false;
        for (int i = 0; i < kChannels; ++i) {
            changed |= codes[i] != out_[i];
            out_[i] = static_cast<std::uint8_t>(codes[i]);
        }
        return changed;
    }

    std::int64_t now_ = 0, frameDue_ = 0;
    Pulse pulse_[2];
    std::uint8_t enabled_ = 0, pcm_ = 0;
    bool readMode_ = false;
    std::uint8_t out_[kChannels] {};
};

// ---------------------------------------------------------------------------
// Namco 163: up to eight wavetable channels in 128 bytes of sound RAM,
// updated one at a time, one every 15 CPU cycles, through one DAC.
class Namco163 {
public:
    static constexpr std::int64_t kSlot = 15;

    void Reset() noexcept {
        *this = Namco163 {};
        slotDue_ = kSlot;
        Update();
    }

    [[nodiscard]] std::int64_t Now() const noexcept { return now_; }
    // The DAC's value: the last updated channel's (sample - 8) x volume.
    [[nodiscard]] int Out() const noexcept { return out_; }
    [[nodiscard]] int Channels() const noexcept { return ((ram_[0x7F] >> 4) & 7) + 1; }
    [[nodiscard]] std::uint8_t Ram(int address) const noexcept { return ram_[address & 0x7F]; }

    template <class Sink>
    void Run(std::int64_t to, Sink& sink) {
        for (;;) {
            const std::int64_t next = live_ ? Earliest(to, slotDue_) : to;
            if (next >= to)
                break;
            now_ = next;
            Slot();
            slotDue_ += kSlot;
            if (Update())
                sink(now_);
        }
        now_ = to;
    }

    template <class Sink>
    void Write(std::uint16_t address, std::uint8_t value, Sink& sink) {
        CatchUp();
        if (address >= 0xF800) {
            // Bits 0-6 the address, bit 7 auto-increment (stops at $7F).
            pointer_ = value & 0x7F;
            increment_ = (value & 0x80) != 0;
        } else if (address >= 0x4800 && address < 0x5000) {
            ram_[pointer_] = value;
            if (increment_ && pointer_ < 0x7F)
                ++pointer_;
        } else if (address >= 0xE000 && address < 0xE800) {
            disabled_ = (value & 0x40) != 0;
        }
        if (Update())
            sink(now_);
    }

    // A CPU read from $4800-$4FFF at Now() (NESdev, Namco 163 audio).
    [[nodiscard]] std::uint8_t ReadData() noexcept {
        CatchUp();
        const std::uint8_t value = ram_[pointer_];
        if (increment_ && pointer_ < 0x7F)
            ++pointer_;
        return value;
    }

private:
    // One channel's update (NESdev: phase = (phase + freq) mod (length x
    // 65536); the sample at ((phase >> 16) + wave address) & $FF, two to a
    // byte, low nibble first; output (sample - 8) x volume, labelled
    // speculative there). Channels are taken from 8 downwards.
    void Slot() noexcept {
        const int count = Channels();
        const int channel = 7 - (slot_ % count);
        slot_ = (slot_ + 1) % count;
        std::uint8_t* r = ram_ + 0x40 + 8 * channel;
        const std::uint32_t freq = r[0] | (r[2] << 8) | ((r[4] & 3) << 16);
        std::uint32_t phase = r[1] | (r[3] << 8) | (r[5] << 16);
        const std::uint32_t length = (256u - (r[4] & 0xFCu)) << 16;
        phase = (phase + freq) % length;
        r[1] = static_cast<std::uint8_t>(phase);
        r[3] = static_cast<std::uint8_t>(phase >> 8);
        r[5] = static_cast<std::uint8_t>(phase >> 16);
        const int index = ((phase >> 16) + r[6]) & 0xFF;
        const std::uint8_t byte = ram_[(index >> 1) & 0x7F];
        const int sample = (index & 1) ? (byte >> 4) : (byte & 0x0F);
        dac_ = (sample - 8) * (r[7] & 0x0F);
    }

    [[nodiscard]] bool Audible() const noexcept {
        if (disabled_)
            return false;
        const int count = Channels();
        for (int i = 0; i < count; ++i)
            if (ram_[0x40 + 8 * (7 - i) + 7] & 0x0F)
                return true;
        return false;
    }

    // While no enabled channel can be heard the DAC reads 0 and every slot
    // only moves its channel's phase: each channel's skipped updates are
    // added at once.
    void CatchUp() noexcept {
        if (live_ || slotDue_ >= now_)
            return;
        const std::int64_t k = (now_ - slotDue_ - 1) / kSlot + 1;
        slotDue_ += k * kSlot;
        if (disabled_)
            return;
        const int count = Channels();
        for (int i = 0; i < count; ++i) {
            const std::int64_t updates = k / count + ((i - slot_ + count) % count < k % count ? 1 : 0);
            std::uint8_t* r = ram_ + 0x40 + 8 * (7 - i);
            const std::uint64_t freq = r[0] | (r[2] << 8) | ((r[4] & 3) << 16);
            const std::uint64_t phase = r[1] | (r[3] << 8) | (r[5] << 16);
            const std::uint64_t length = static_cast<std::uint64_t>(256u - (r[4] & 0xFCu)) << 16;
            const std::uint64_t moved = (phase + (freq % length) * (static_cast<std::uint64_t>(updates) % length)) % length;
            r[1] = static_cast<std::uint8_t>(moved);
            r[3] = static_cast<std::uint8_t>(moved >> 8);
            r[5] = static_cast<std::uint8_t>(moved >> 16);
        }
        slot_ = static_cast<int>((slot_ + k) % count);
        dac_ = 0;
    }

    bool Update() noexcept {
        const bool live = Audible();
        if (live && !live_)
            CatchUp();
        live_ = live;
        const int code = live ? dac_ : 0;
        const bool changed = code != out_;
        out_ = code;
        return changed;
    }

    std::int64_t now_ = 0, slotDue_ = 0;
    std::uint8_t ram_[128] {};
    int pointer_ = 0, slot_ = 0, dac_ = 0, out_ = 0;
    bool increment_ = false, disabled_ = false, live_ = false;
};

// ---------------------------------------------------------------------------
// The Famicom Disk System's sound: a 64-step 6-bit wavetable with a
// volume envelope and a frequency modulator.
class Fds {
public:
    static constexpr std::int64_t kTick = 16;      // wave and modulator units

    void Reset() noexcept {
        *this = Fds {};
        tickDue_ = kTick;
        Update();
    }

    [[nodiscard]] std::int64_t Now() const noexcept { return now_; }
    // The wave sample times the volume gain (0 to 63 x 32); MasterVolume()
    // is $4089's divisor index.
    [[nodiscard]] int Out() const noexcept { return out_; }
    [[nodiscard]] int MasterVolume() const noexcept { return master_; }

    template <class Sink>
    void Run(std::int64_t to, Sink& sink) {
        if (!live_) {
            now_ = to;
            return;
        }
        for (;;) {
            std::int64_t next = Earliest(to, tickDue_);
            if (EnvelopesRun()) {
                next = Earliest(next, volumeEnvelope_.timer.due);
                next = Earliest(next, modEnvelope_.timer.due);
            }
            if (next >= to)
                break;
            now_ = next;
            if (EnvelopesRun()) {
                for (Envelope* e : { &volumeEnvelope_, &modEnvelope_ })
                    if (e->timer.due == now_) {
                        e->Clock();
                        e->timer.due += e->timer.period;
                    }
                if (waveHalt_)
                    latchedGain_ = volumeEnvelope_.gain;
            }
            if (tickDue_ == now_) {
                Tick();
                tickDue_ += kTick;
            }
            if (Update())
                sink(now_);
        }
        now_ = to;
    }

    template <class Sink>
    void Write(std::uint16_t address, std::uint8_t value, Sink& sink) {
        CatchUp();
        if (address >= 0x4040 && address <= 0x407F) {
            if (waveWrite_)
                wave_[address - 0x4040] = value & 0x3F;
        } else {
            switch (address) {
            case 0x4023:
                // Clearing bit 1 forces the volume envelope to 0, resets
                // the wave accumulator and zeroes the modulator's counter.
                enabled_ = (value & 2) != 0;
                if (!enabled_) {
                    volumeEnvelope_.gain = 0;
                    latchedGain_ = 0;
                    accumulator_ = 0;
                    counter_ = 0;
                }
                break;
            case 0x4080:
                volumeEnvelope_.Write(value, now_, Speed());
                if (waveHalt_)
                    latchedGain_ = volumeEnvelope_.gain;
                break;
            case 0x4082:
                pitch_ = (pitch_ & 0xF00) | value;
                break;
            case 0x4083:
                pitch_ = (pitch_ & 0xFF) | ((value & 15) << 8);
                // Bit 7 halts the wave and resets its accumulator; bit 6
                // stops the envelopes and resets their timers.
                waveHalt_ = (value & 0x80) != 0;
                if (waveHalt_) {
                    accumulator_ = 0;
                    latchedGain_ = volumeEnvelope_.gain;
                }
                envelopeHalt_ = (value & 0x40) != 0;
                if (envelopeHalt_) {
                    volumeEnvelope_.Restart(now_, Speed());
                    modEnvelope_.Restart(now_, Speed());
                }
                break;
            case 0x4084:
                modEnvelope_.Write(value, now_, Speed());
                break;
            case 0x4085:
                counter_ = static_cast<std::int8_t>((value & 0x7F) ^ 0x40) - 0x40;
                break;
            case 0x4086:
                modPitch_ = (modPitch_ & 0xF00) | value;
                break;
            case 0x4087:
                modPitch_ = (modPitch_ & 0xFF) | ((value & 15) << 8);
                // Bit 7 halts the modulator, clearing its accumulator's low
                // 13 bits (the table address stays) and opening $4088;
                // bit 6 forces a carry every tick.
                modHalt_ = (value & 0x80) != 0;
                if (modHalt_)
                    modAccumulator_ &= ~0x1FFFu;
                modForce_ = (value & 0x40) != 0;
                break;
            case 0x4088:
                if (modHalt_) {
                    table_[(modAccumulator_ >> 13) & 31] = value & 7;
                    modAccumulator_ = (modAccumulator_ + (1u << 13)) & 0x3FFFF;
                }
                break;
            case 0x4089:
                master_ = value & 3;
                waveWrite_ = (value & 0x80) != 0;
                break;
            case 0x408A: {
                const int oldSpeed = speed_;
                speed_ = value;
                volumeEnvelope_.SetSpeed(Speed());
                modEnvelope_.SetSpeed(Speed());
                if (oldSpeed == 0 && speed_ != 0) {
                    volumeEnvelope_.timer.CatchUp(now_);
                    modEnvelope_.timer.CatchUp(now_);
                }
                break;
            }
            default:
                break;
            }
        }
        if (Update())
            sink(now_);
    }

private:
    struct Envelope {
        std::uint8_t reg = 0x80, gain = 0;
        Divider timer { 0, 8 };
        void SetSpeed(int m) noexcept { timer.period = 8 * ((reg & 0x3F) + 1) * (m + 1); }
        void Restart(std::int64_t now, int m) noexcept {
            SetSpeed(m);
            timer.due = now + timer.period;
        }
        // $4080 / $4084: bit 7 off (the gain is bits 5-0), bit 6 increase.
        // Writing restarts the timer, so the next tick is delayed.
        void Write(std::uint8_t value, std::int64_t now, int m) noexcept {
            reg = value;
            if (value & 0x80)
                gain = value & 0x3F;
            Restart(now, m);
        }
        void Clock() noexcept {
            if (reg & 0x80)
                return;
            if (reg & 0x40) {
                if (gain < 32)
                    ++gain;
            } else if (gain > 0) {
                --gain;
            }
        }
    };

    [[nodiscard]] int Speed() const noexcept { return speed_; }
    [[nodiscard]] bool EnvelopesRun() const noexcept { return enabled_ && !envelopeHalt_ && speed_ != 0; }
    [[nodiscard]] bool ModulatorRunning() const noexcept { return !modHalt_ && (modPitch_ != 0 || modForce_); }
    [[nodiscard]] bool EnvelopesActive() const noexcept {
        if (!EnvelopesRun())
            return false;
        const bool volMoves = !(volumeEnvelope_.reg & 0x80)
            && ((volumeEnvelope_.reg & 0x40) ? volumeEnvelope_.gain < 32 : volumeEnvelope_.gain > 0);
        const bool modMoves = !(modEnvelope_.reg & 0x80)
            && ((modEnvelope_.reg & 0x40) ? modEnvelope_.gain < 32 : modEnvelope_.gain > 0);
        return volMoves || modMoves;
    }
    [[nodiscard]] bool ShouldBeLive() const noexcept {
        if (!enabled_)
            return false;
        if (EnvelopesActive())
            return true;
        if (waveHalt_ && !ModulatorRunning())
            return false;
        if (latchedGain_ == 0 && volumeEnvelope_.gain == 0 && !ModulatorRunning())
            return false;
        return true;
    }

    // NESdev, FDS audio: the modulated pitch (rainwarrior's RP2C33 formula).
    [[nodiscard]] std::uint32_t WavePitch() const noexcept {
        int temp = counter_ * modEnvelope_.gain;
        if ((temp & 0x0F) != 0 && ((temp >> 4) & 0x80) == 0)
            temp += counter_ < 0 ? -0x10 : 0x20;
        temp += 0x400;
        const std::uint32_t scaled = (static_cast<std::uint32_t>(temp) >> 4) & 0xFF;
        return (pitch_ * scaled) & 0xFFFFF;
    }

    void Tick() noexcept {
        if (!enabled_)
            return;
        // The modulator: its 12-bit pitch into an 18-bit accumulator whose
        // bits 13-17 address the table; each carry out of bit 11 applies
        // the entry to the 7-bit signed counter.
        if (ModulatorRunning()) {
            const std::uint32_t sum = modAccumulator_ + modPitch_;
            const bool carry = modForce_ || ((modAccumulator_ & 0xFFF) + modPitch_) > 0xFFF;
            modAccumulator_ = sum & 0x3FFFF;
            if (carry) {
                static constexpr int kStep[8] = { 0, 1, 2, 4, 0, -4, -2, -1 };
                const int entry = table_[(modAccumulator_ >> 13) & 31];
                counter_ = entry == 4 ? 0 : ((counter_ + kStep[entry] + 64) & 127) - 64;
            }
        }
        // The wave: the modulated pitch into a 24-bit accumulator whose top
        // six bits are the position. A gain change takes effect at position 0
        // or whenever the 24-bit accumulator wraps.
        if (!waveHalt_ && !waveWrite_) {
            const std::uint32_t prev = accumulator_;
            accumulator_ = (accumulator_ + WavePitch()) & 0xFFFFFF;
            if ((accumulator_ >> 18) == 0 || accumulator_ < prev)
                latchedGain_ = volumeEnvelope_.gain;
        }
    }

    void CatchUp() noexcept {
        if (live_)
            return;
        if (EnvelopesRun()) {
            volumeEnvelope_.timer.CatchUp(now_);
            modEnvelope_.timer.CatchUp(now_);
        }
        if (tickDue_ < now_) {
            const std::int64_t k = (now_ - tickDue_ - 1) / kTick + 1;
            tickDue_ += k * kTick;
            if (enabled_ && !waveHalt_ && !waveWrite_) {
                const std::uint64_t pitch = WavePitch();
                accumulator_ = static_cast<std::uint32_t>((accumulator_ + static_cast<std::uint64_t>(k) * pitch) & 0xFFFFFFu);
            }
        }
    }

    bool Update() noexcept {
        const bool live = ShouldBeLive();
        if (live && !live_)
            CatchUp();
        live_ = live;
        int code = out_;
        if (!waveWrite_) {
            // Halted, the output holds the sample at $4040.
            const int sample = waveHalt_ ? wave_[0] : wave_[accumulator_ >> 18];
            const int gain = latchedGain_ < 32 ? latchedGain_ : 32;
            code = enabled_ ? sample * gain : 0;
        }
        // With wave writes on, the output holds its level.
        const bool changed = code != out_ || master_ != shownMaster_;
        out_ = code;
        shownMaster_ = master_;
        return changed;
    }

    std::int64_t now_ = 0, tickDue_ = 0;
    std::uint8_t wave_[64] {};
    std::uint8_t table_[32] {};
    Envelope volumeEnvelope_, modEnvelope_;
    std::uint32_t accumulator_ = 0, modAccumulator_ = 0;
    std::uint16_t pitch_ = 0, modPitch_ = 0;
    int counter_ = 0, master_ = 0, shownMaster_ = 0, out_ = 0;
    std::uint8_t latchedGain_ = 0, speed_ = 0xE8;     // the BIOS writes $E8
    bool enabled_ = true, waveHalt_ = true, envelopeHalt_ = false, modHalt_ = true, modForce_ = false, waveWrite_ = false,
         live_ = false;
};

} // namespace protocodus::reboot::famicom
