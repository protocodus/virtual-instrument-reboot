#pragma once

// Console sound driver: translates notes and controls into legal 6502 CPU
// bus writes onto a Console hardware blackbox (Boards/Console.h) and its
// active Famicom cartridge chip.
//
// - QuantizeVolume exposes each chip's native volume resolution: 0..15 on the
//   APU pulses/noise, VRC6 pulses, MMC5 pulses, Namco 163 and Sunsoft 5B;
//   0..1 gate on the APU triangle; 0..32 on the FDS ($4080); and 0..42 on the
//   VRC6 sawtooth accumulator rate ($B000).
// - On FDS above its 12-bit single-cycle ceiling (~1747 Hz, MIDI 94-108) and
//   8-channel Namco 163 above ~3728 Hz (MIDI 107-108), the driver folds
//   multiple harmonic cycles into the wavetable or halves the wavetable length,
//   playing the full piano compass on the cartridge chip itself.
// - Namco 163 uses $F800 auto-increment burst writes; Sunsoft 5B writes its
//   $C000 register latch only when the selected register changes.
// - The chip's own units, when the playing layer asks for them: the APU's and
//   the MMC5's envelopes (bit 4 of $4000 clear), restarted by the trigger,
//   and the APU pulses' sweeps ($4001), written before a trigger, after which
//   the driver leaves the pulse's period to its sweep.

#include "../Boards/Console.h"
#include "DriverTypes.h"
#include "Waves.h"

#include <cstdint>

namespace protocodus::reboot {

class ConsoleDriver {
public:
    static constexpr int kApuChannels = 4;
    static constexpr int kMaxChannels = kApuChannels + 8;

    void Boot(Console& c, int n163Channels, int bias) noexcept {
        n163Count_ = Clamp(n163Channels, 1, 8);
        for (Shadow& s : shadow_)
            s = Shadow {};
        lastS5bAddr_ = -1;
        s5bMixer_ = 0x3F;

        // A sound driver's start: frame IRQs off, the four channels on, the
        // pulses' sweeps negating (so the sweep cannot mute a low note:
        // NESdev, "APU Sweep"), everything silent at constant volume.
        c.Write(0x4017, 0x40);
        c.Write(0x4015, 0x0F);
        c.Write(0x4000, 0x30);
        c.Write(0x4004, 0x30);
        c.Write(0x400C, 0x30);
        c.Write(0x4001, 0x08);
        c.Write(0x4005, 0x08);
        c.Write(0x4008, 0x00);
        c.Write(0x4011, static_cast<std::uint8_t>(bias & 0x7F));

        switch (c.CartridgeType()) {
        case Console::kMmc5:
            c.Write(0x5015, 0x03);
            c.Write(0x5000, 0x30);
            c.Write(0x5004, 0x30);
            break;
        case Console::kN163:
            c.Write(0xE000, 0x00);
            c.Write(0xF800, 0x7F);
            c.Write(0x4800, static_cast<std::uint8_t>((n163Count_ - 1) << 4));
            break;
        case Console::kFds:
            c.Write(0x4023, 0x83);
            c.Write(0x4080, 0x80);
            c.Write(0x4084, 0x80);
            c.Write(0x4089, 0x00);
            c.Write(0x408A, 0xE8);
            break;
        case Console::k5B:
            S5BWrite(c, 7, s5bMixer_);
            break;
        default:
            break;
        }
        c.FinishBoot();
    }

    [[nodiscard]] int Channels(const Console& c) const noexcept {
        switch (c.CartridgeType()) {
        case Console::kVrc6: case Console::k5B: return kApuChannels + 3;
        case Console::kMmc5: return kApuChannels + 2;
        case Console::kN163: return kApuChannels + n163Count_;
        case Console::kFds: return kApuChannels + 1;
        default: return kApuChannels;
        }
    }

    [[nodiscard]] static Voice VoiceOf(const Console& c, int channel) noexcept {
        switch (channel) {
        case 0: case 1: return Voice::Pulse;
        case 2: return Voice::Triangle;
        case 3: return Voice::Noise;
        default: break;
        }
        switch (c.CartridgeType()) {
        case Console::kVrc6: return channel == 6 ? Voice::Saw : Voice::Pulse;
        case Console::kMmc5: return Voice::Pulse;
        case Console::kN163: return Voice::Wave;
        case Console::kFds: return Voice::ModWave;
        default: return Voice::Tone;
        }
    }

    [[nodiscard]] static bool EightDuties(const Console& c, int channel) noexcept {
        return c.CartridgeType() == Console::kVrc6 && channel >= 4 && channel < 6;
    }

    // The APU's envelope (NESdev, APU Envelope; spec): with bit 4 of $4000
    // clear, a trigger starts it at 15 and every V + 1 quarter frames it falls
    // a step (V the low nibble, so paces 1 to 16), to 0, or with bit 5 set
    // back to 15 (bit 5 also halts the length counter). The quarter frame is
    // a quarter of the 4-step sequence $4017 = $40 leaves (APU Frame
    // Counter). The MMC5's pulses have the same unit on their own 241 Hz
    // clock (Chips/Famicom.h). The triangle, the DMC and the other
    // cartridges' channels have none here.
    [[nodiscard]] static EnvelopeUnit EnvelopeOf(const Console& c, int channel) noexcept {
        if (channel == 0 || channel == 1 || channel == 3)
            return { 16, static_cast<double>(nes::kFrame[c.Apu().RegionIndex()][0].length) / 4.0, false, false, true };
        if (c.CartridgeType() == Console::kMmc5 && (channel == kApuChannels || channel == kApuChannels + 1))
            return { 16, static_cast<double>(famicom::Mmc5::kFramePeriod), false, false, true };
        return {};
    }

    // The APU's pulses have sweeps; the MMC5's do not (NESdev, MMC5 audio).
    [[nodiscard]] static bool HasSweep(int channel) noexcept { return channel == 0 || channel == 1; }

    [[nodiscard]] static int QuantizeVolume(int, Voice voice, double gain) noexcept {
        if (voice == Voice::Triangle)
            return gain > 0.0 ? 1 : 0;
        if (voice == Voice::ModWave)
            return QuantizeGain(gain, 32);
        if (voice == Voice::Saw)
            return QuantizeGain(gain, 42);
        return QuantizeGain(gain, 15);
    }

    // Each channel's effective period register value for `hz`; false when out
    // of reach. For FDS and N163 above single-cycle pitch ceilings, encodes
    // `reg * mult` so `Expected()` stays uniform across the piano compass.
    [[nodiscard]] bool PeriodFor(const Console& c, int channel, Voice, double hz, int shape,
                                 std::int64_t& period) const noexcept {
        if (hz <= 0.0)
            return false;
        const double cpu = c.CpuHz();
        switch (channel) {
        case 0: case 1:
            // CPU / (16 (t + 1)); t under 8 is muted by the sweep unit.
            period = NearestCount(cpu / (16.0 * hz)) - 1;
            return period >= 8 && period <= 0x7FF;
        case 2:
            // CPU / (32 (t + 1)); t under 2 is ultrasonic.
            period = NearestCount(cpu / (32.0 * hz)) - 1;
            return period >= 2 && period <= 0x7FF;
        case 3:
            period = NoiseFor(c, hz, (shape & 1) != 0);
            return true;
        default:
            break;
        }
        switch (c.CartridgeType()) {
        case Console::kVrc6:
            period = NearestCount(cpu / ((channel == 6 ? 14.0 : 16.0) * hz)) - 1;
            return period >= 0 && period <= 0xFFF;
        case Console::kMmc5:
            // The MMC5's pulses have no sweep unit, so no low-period mute.
            period = NearestCount(cpu / (16.0 * hz)) - 1;
            return period >= 0 && period <= 0x7FF;
        case Console::kN163: {
            const int baseLen = N163BaseLength();
            for (int mult = 1; mult <= 4 && baseLen / mult >= 4; mult *= 2) {
                const int len = baseLen / mult;
                const std::int64_t reg = NearestCount(hz * 15.0 * n163Count_ * 65536.0 * len / cpu);
                if (reg >= 1 && reg <= 0x3FFFF) {
                    period = reg * mult;
                    return true;
                }
            }
            return false;
        }
        case Console::kFds: {
            for (int mult : { 1, 2, 4 }) {
                const std::int64_t reg = NearestCount(hz * 4194304.0 / (cpu * mult));
                if (reg >= 1 && reg <= 0xFFF) {
                    period = reg * mult;
                    return true;
                }
            }
            return false;
        }
        case Console::k5B:
            // CPU / (32 TP).
            period = NearestCount(cpu / (32.0 * hz));
            return period >= 1 && period <= 0xFFF;
        default:
            return false;
        }
    }

    void Start(Console& c, int channel, const ChannelSettings& s) noexcept { Write(c, channel, s, true); }
    void Change(Console& c, int channel, const ChannelSettings& s) noexcept { Write(c, channel, s, false); }

    void Stop(Console& c, int channel) noexcept {
        ChannelSettings s;
        s.voice = shadow_[channel].voice;
        s.period = shadow_[channel].period;
        s.shape = shadow_[channel].shape;
        s.mod = shadow_[channel].mod;
        Write(c, channel, s, false);
    }

    void SetBias(Console& c, int level) noexcept {
        c.Write(0x4011, static_cast<std::uint8_t>(level & 0x7F));
    }

private:
    struct Shadow {
        bool active = false;
        Voice voice = Voice::Pulse;
        std::int64_t period = -1;
        std::int64_t regPitch = -1;
        int volume = -1;
        int shape = -1;
        int mod = -1;
        int mult = 1;
        int control = -1;        // what the channel's $4000 (or $5000) holds
        int pace = 0;            // the envelope it was triggered with
        bool loop = false;
        int sweepRegister = 0x08;  // what $4001 holds, as Boot leaves it
        bool sweep = false;      // the pulse sweeps: its period is the sweep's
    };

    // $4000's low six bits (and $400C's, $5000's): the length counter halted
    // and the volume constant, or the envelope at pace V + 1, looping with
    // the halt bit.
    static int ControlBits(const ChannelSettings& s, int volume) noexcept {
        const int pace = Clamp(s.pace, 0, 16);
        return pace == 0 ? 0x30 | volume : (s.loop ? 0x20 : 0x00) | (pace - 1);
    }

    // The length a trigger loads ($4003 bits 7-3): index 31 under the halt
    // bit, where it does not count; else index 1, 254 half frames (APU
    // Length Counter), longer than the slowest envelope's 240 quarter frames.
    static int LengthBits(const ChannelSettings& s) noexcept {
        return s.pace > 0 && !s.loop ? 0x08 : 0xF8;
    }

    static bool NewEnvelope(const Shadow& sh, const ChannelSettings& s) noexcept {
        return s.pace > 0 && (Clamp(s.pace, 0, 16) != sh.pace || s.loop != sh.loop);
    }

    // $4001 (NESdev, APU Sweep; spec): enabled, the divider's period P (P + 1
    // half frames), negate (the period falls, so the pitch rises), the shift.
    // A step's size `amount` 1 to 7 is 8 - shift. No sweep is $08, negating
    // with shift 0, which never mutes a low note (Boot).
    static int SweepRegister(const ChannelSettings& s) noexcept {
        if (s.sweep == 0)
            return 0x08;
        const int amount = Clamp(s.sweep < 0 ? -s.sweep : s.sweep, 1, 7);
        return 0x80 | ((Clamp(s.sweepPace, 1, 8) - 1) << 4) | (s.sweep > 0 ? 0x08 : 0x00) | (8 - amount);
    }

    static int Clamp(int v, int lo, int hi) noexcept { return v < lo ? lo : v > hi ? hi : v; }

    static void Took(Shadow& sh, const ChannelSettings& s, int control) noexcept {
        sh.control = control;
        sh.pace = Clamp(s.pace, 0, 16);
        sh.loop = s.loop;
        sh.shape = s.shape & 3;
    }

    static int QuantizeGain(double gain, int maxStep) noexcept {
        if (gain <= 0.0)
            return 0;
        if (gain >= 1.0)
            return maxStep;
        const int v = static_cast<int>(gain * static_cast<double>(maxStep) + 0.5);
        return v < 0 ? 0 : v > maxStep ? maxStep : v;
    }

    // The noise period whose LFSR clock, CPU / period, is nearest `hz`
    // times the register's cycle: 93 steps in short mode, so it plays in
    // tune, and 32 in long mode (ours: the key picks the noise's colour).
    [[nodiscard]] static std::int64_t NoiseFor(const Console& c, double hz, bool shortMode) noexcept {
        const double target = hz * (shortMode ? 93.0 : 32.0);
        const int region = c.Apu().RegionIndex();
        const double cpuHz = c.CpuHz();
        int best = 0;
        double bestRatio = 0.0;
        for (int i = 0; i < 16; ++i) {
            const double rate = cpuHz / nes::kNoisePeriod[region][i];
            const double ratio = rate > target ? rate / target : target / rate;
            if (bestRatio == 0.0 || ratio < bestRatio)
                bestRatio = ratio, best = i;
        }
        return best;
    }

    // Wave length in samples: 32 while every channel's 32 samples fit in
    // the RAM under the channel registers, else 16.
    // (16 bytes and 8 of registers a channel: up to five channels.)
    [[nodiscard]] int N163BaseLength() const noexcept { return 24 * n163Count_ <= 128 ? 32 : 16; }

    static void N163Poke(Console& c, int address, std::uint8_t value) noexcept {
        c.Write(0xF800, static_cast<std::uint8_t>(address & 0x7F));
        c.Write(0x4800, value);
    }

    void S5BWrite(Console& c, int r, std::uint8_t value) noexcept {
        if (lastS5bAddr_ != r) {
            c.Write(0xC000, static_cast<std::uint8_t>(r));
            lastS5bAddr_ = r;
        }
        c.Write(0xE000, value);
    }

    void Write(Console& c, int channel, const ChannelSettings& s, bool start) noexcept {
        Shadow& sh = shadow_[channel];
        const bool trigger = start || !sh.active;
        int writtenVolume = Clamp(s.volume, 0, 15);
        switch (channel) {
        case 0: case 1: {
            const std::uint16_t base = static_cast<std::uint16_t>(0x4000 + 4 * channel);
            const bool restart = trigger || NewEnvelope(sh, s);
            const int control = ((s.shape & 3) << 6) | ControlBits(s, writtenVolume);
            if (restart || control != sh.control)
                c.Write(base, static_cast<std::uint8_t>(control));
            if (restart) {
                const int sweep = SweepRegister(s);
                if (sweep != sh.sweepRegister)
                    c.Write(base + 1, static_cast<std::uint8_t>(sweep));
                sh.sweepRegister = sweep;
                sh.sweep = s.sweep != 0;
            }
            if (restart || (s.period != sh.period && !sh.sweep)) {
                c.Write(base + 2, static_cast<std::uint8_t>(s.period & 0xFF));
                // $4003 restarts the sequencer and the envelope: a driver
                // writes it only on a trigger or when the high bits change
                // (the click of a vibrato across them, and an envelope
                // starting again there, are the chip's).
                if (restart || (s.period >> 8) != (sh.period >> 8))
                    c.Write(base + 3, static_cast<std::uint8_t>(LengthBits(s) | ((s.period >> 8) & 7)));
            }
            Took(sh, s, control);
            break;
        }
        case 2: {
            // No volume: the linear counter held open or closed. $4008 with
            // its control bit set holds the counter at 127; clear and 0, it
            // closes at the next quarter frame. Only $400B sets the reload
            // flag that opens it again, and it leaves the phase alone.
            const bool on = s.volume > 0;
            const bool wasOn = sh.volume > 0;
            writtenVolume = on ? 1 : 0;
            if (trigger || on != wasOn)
                c.Write(0x4008, on ? 0xFF : 0x00);
            if (trigger || s.period != sh.period)
                c.Write(0x400A, static_cast<std::uint8_t>(s.period & 0xFF));
            if (trigger || (s.period >> 8) != (sh.period >> 8) || (on && !wasOn))
                c.Write(0x400B, static_cast<std::uint8_t>(0xF8 | ((s.period >> 8) & 7)));
            break;
        }
        case 3: {
            const bool restart = trigger || NewEnvelope(sh, s);
            const int control = ControlBits(s, writtenVolume);
            if (restart || control != sh.control)
                c.Write(0x400C, static_cast<std::uint8_t>(control));
            if (restart || s.period != sh.period || (s.shape & 1) != sh.shape)
                c.Write(0x400E, static_cast<std::uint8_t>(((s.shape & 1) << 7) | (s.period & 15)));
            if (restart)
                c.Write(0x400F, static_cast<std::uint8_t>(LengthBits(s)));
            Took(sh, s, control);
            sh.shape = s.shape & 1;
            break;
        }
        default:
            writtenVolume = WriteCartridge(c, channel, s, trigger);
            break;
        }
        sh.active = true;
        sh.voice = s.voice;
        sh.period = s.period;
        sh.volume = writtenVolume;
        sh.mod = s.mod;
    }

    int WriteCartridge(Console& c, int channel, const ChannelSettings& s, bool trigger) noexcept {
        Shadow& sh = shadow_[channel];
        const int k = channel - kApuChannels;
        switch (c.CartridgeType()) {
        case Console::kVrc6: {
            const std::uint16_t base = static_cast<std::uint16_t>(0x9000 + 0x1000 * k);
            int volume;
            if (k < 2) {
                volume = Clamp(s.volume, 0, 15);
                const int duty = s.shape & 7;
                if (trigger || volume != sh.volume || duty != sh.shape)
                    c.Write(base, static_cast<std::uint8_t>((duty << 4) | volume));
                sh.shape = duty;
            } else {
                // The saw's accumulator rate is natively 0..42 (above 42
                // overflows; NESdev: VRC6 audio).
                volume = Clamp(s.volume, 0, 42);
                if (trigger || volume != sh.volume)
                    c.Write(base, static_cast<std::uint8_t>(volume));
            }
            if (trigger || s.period != sh.period) {
                c.Write(base + 1, static_cast<std::uint8_t>(s.period & 0xFF));
                c.Write(base + 2, static_cast<std::uint8_t>(0x80 | ((s.period >> 8) & 15)));
            }
            return volume;
        }
        case Console::kMmc5: {
            const int volume = Clamp(s.volume, 0, 15);
            const std::uint16_t base = static_cast<std::uint16_t>(0x5000 + 4 * k);
            const bool restart = trigger || NewEnvelope(sh, s);
            const int control = ((s.shape & 3) << 6) | ControlBits(s, volume);
            if (restart || control != sh.control)
                c.Write(base, static_cast<std::uint8_t>(control));
            if (restart || s.period != sh.period) {
                c.Write(base + 2, static_cast<std::uint8_t>(s.period & 0xFF));
                if (restart || (s.period >> 8) != (sh.period >> 8))
                    c.Write(base + 3, static_cast<std::uint8_t>(LengthBits(s) | ((s.period >> 8) & 7)));
            }
            Took(sh, s, control);
            return volume;
        }
        case Console::kN163: {
            const int volume = Clamp(s.volume, 0, 15);
            // Channel k is the chip's channel 8 - k, registers at $78 - 8k.
            const int reg = 0x78 - 8 * k;
            const int baseLen = N163BaseLength();
            const int mult = s.period > 2 * 0x3FFFF ? 4 : s.period > 0x3FFFF ? 2 : 1;
            const int length = baseLen / mult;
            const std::int64_t freq = s.period / mult;
            const int shape = s.shape % kWaveCount;
            // The chip reads its RAM as it plays, so a held note takes a new
            // wave (or shorter wave length above single-cycle pitch) as written.
            if (shape != sh.shape || mult != sh.mult) {
                const int step = 32 / length;
                c.Write(0xF800, static_cast<std::uint8_t>(0x80 | (k * (baseLen / 2))));
                for (int i = 0; i < length / 2; ++i) {
                    const int a = kWaves[shape].samples[(2 * i) * step];
                    const int b = kWaves[shape].samples[(2 * i + 1) * step];
                    // Low nibble first (NESdev).
                    c.Write(0x4800, static_cast<std::uint8_t>(a | (b << 4)));
                }
                N163Poke(c, reg + 6, static_cast<std::uint8_t>(k * baseLen));
                sh.shape = shape;
            }
            if (trigger) {
                c.Write(0xF800, static_cast<std::uint8_t>(0x80 | reg));
                c.Write(0x4800, static_cast<std::uint8_t>(freq & 0xFF));
                c.Write(0x4800, 0x00);
                c.Write(0x4800, static_cast<std::uint8_t>((freq >> 8) & 0xFF));
                c.Write(0x4800, 0x00);
                c.Write(0x4800, static_cast<std::uint8_t>(((256 - length) & 0xFC) | ((freq >> 16) & 3)));
                c.Write(0x4800, 0x00);
            } else if (freq != sh.regPitch || mult != sh.mult) {
                N163Poke(c, reg + 0, static_cast<std::uint8_t>(freq & 0xFF));
                N163Poke(c, reg + 2, static_cast<std::uint8_t>((freq >> 8) & 0xFF));
                N163Poke(c, reg + 4, static_cast<std::uint8_t>(((256 - length) & 0xFC) | ((freq >> 16) & 3)));
            }
            if (trigger || volume != sh.volume) {
                const std::uint8_t high = reg == 0x78 ? static_cast<std::uint8_t>((n163Count_ - 1) << 4) : 0;
                N163Poke(c, reg + 7, static_cast<std::uint8_t>(high | volume));
            }
            sh.mult = mult;
            sh.regPitch = freq;
            return volume;
        }
        case Console::kFds: {
            const int volume = Clamp(s.volume, 0, 32);
            const int mult = s.period > 2 * 0xFFF ? 4 : s.period > 0xFFF ? 2 : 1;
            const std::int64_t regPitch = s.period / mult;
            const int shape = s.shape % kWaveCount;
            // The wave is writable while $4089 holds the output (NESdev), so
            // a held note takes a new wave (or multi-cycle octave table).
            if (shape != sh.shape || mult != sh.mult) {
                c.Write(0x4089, 0x80);
                for (int i = 0; i < 64; ++i)
                    c.Write(static_cast<std::uint16_t>(0x4040 + i), kWaves64[shape][(i * mult) & 63]);
                c.Write(0x4089, 0x00);
                sh.shape = shape;
            }
            if (trigger || volume != sh.volume)
                c.Write(0x4080, static_cast<std::uint8_t>(0x80 | volume));
            // The modulator at the carrier's own rate (its accumulator runs
            // on the same scale), depth from the part's mod (0-63).
            const int depth = Clamp(s.mod, 0, 63);
            if (trigger) {
                c.Write(0x4087, 0x80);
                for (int i = 0; i < 32; ++i)
                    c.Write(0x4088, kModTable[i]);
                c.Write(0x4085, 0x00);
            }
            if (trigger || depth != sh.mod)
                c.Write(0x4084, static_cast<std::uint8_t>(0x80 | depth));
            if (trigger || regPitch != sh.regPitch) {
                c.Write(0x4082, static_cast<std::uint8_t>(regPitch & 0xFF));
                c.Write(0x4083, static_cast<std::uint8_t>((regPitch >> 8) & 15));
                c.Write(0x4086, static_cast<std::uint8_t>(regPitch & 0xFF));
                c.Write(0x4087, static_cast<std::uint8_t>((regPitch >> 8) & 15));
            }
            sh.mult = mult;
            sh.regPitch = regPitch;
            return volume;
        }
        case Console::k5B: {
            const int volume = Clamp(s.volume, 0, 15);
            if (trigger || s.period != sh.period) {
                S5BWrite(c, 2 * k, static_cast<std::uint8_t>(s.period & 0xFF));
                S5BWrite(c, 2 * k + 1, static_cast<std::uint8_t>((s.period >> 8) & 15));
            }
            if (trigger || volume != sh.volume)
                S5BWrite(c, 8 + k, static_cast<std::uint8_t>(volume));
            if (trigger) {
                // Tone on (bit k clear), noise off, in the shared mixer.
                s5bMixer_ = static_cast<std::uint8_t>((s5bMixer_ & ~(1 << k)) | (8 << k));
                S5BWrite(c, 7, s5bMixer_);
            }
            return volume;
        }
        default:
            return 0;
        }
    }

    int n163Count_ = 4;
    int lastS5bAddr_ = -1;
    std::uint8_t s5bMixer_ = 0x3F;
    Shadow shadow_[kMaxChannels];
};

} // namespace protocodus::reboot
