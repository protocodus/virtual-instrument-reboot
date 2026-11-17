#pragma once

// The console board: the 2A03's APU (Chips/NesApu.h) on the CPU clock, with
// a Famicom cartridge's sound chip beside it (Chips/Famicom.h, and the 5B
// as Chips/Ay8910.h at a 16-cycle tick), their DACs, the mixer and the
// machine's output stage.
//
// Revisions: the NTSC front-loader, the NTSC Famicom and the PAL
// front-loader. Only the Famicom's cartridge slot carries the cartridge's
// sound into the mix (NESdev Wiki, "Expansion audio": the front-loader's
// cartridge connector has no audio return), so a cartridge sounds on the
// Famicom revision alone.
//
// APU mixer (NESdev Wiki, "APU Mixer", read 2026-11-17; reported, the page
// calls it approximate):
//   pulse = 95.88 / (8128 / (p1 + p2) + 100)
//   tnd   = 159.79 / (1 / (t / 8227 + n / 12241 + d / 22638) + 100)
// each 0 when its inputs are 0. A pulse at 15 alone is 0.149377 (kPulse15).
// Brad Smith's DAC measurements of a front-loader and a Famicom (NESdev
// forum t=24288, read 2026-11-17) found these curves "reasonably good",
// with variance between units.
//
// The cartridge chips, mixed linearly onto that scale (NESdev Wiki, each
// chip's audio page, read 2026-11-17):
// - VRC6: its 6-bit DAC sum, a pulse at 15 as loud as the APU's and of
//   opposite polarity ("VRC6 audio", reported).
// - MMC5: "equivalent in volume to the corresponding APU channels", "the
//   polarity of all MMC5 channels is reversed" (reported). Mixed per step
//   at the APU pulse's step, linearly (our reading; Known gaps).
// - Namco 163: in 1-channel mode its loudest square, 225 steps peak to peak
//   at (15 - 8) and (0 - 8) times 15, is 11 to 13 dB louder than the APU's
//   loudest square for submapper 3's cartridges; we take the middle, 12 dB.
//   Polarity unstated; taken as the APU's.
// - Disk unit: about 2.4 times a 2A03 square at full volume, same polarity,
//   through its own RC low-pass of about 2 kHz; the master volume divides
//   by 1, 1.5, 2 or 2.5 ("FDS audio", reported).
// - 5B: 1.5 dB a step of its 5-bit DAC (3 dB a volume step), codes 0 and 1
//   silent ("Sunsoft 5B audio", reported). Its level against the APU is
//   Brad Smith's survey (nes-audio-tests, db_5b, results in the NESdev
//   forum thread t=17741, read 2026-11-17; reported, one cartridge): a 5B
//   square at volume 12 measured 1.3 dB under the APU's square at full
//   volume, both 50 % duty, by RMS. Volume 12 is code 25, 9 dB under code
//   31, so code 31 is 7.7 dB over the APU's pulse at 15.
//
// Line stage (NESdev Wiki, "APU Mixer", read 2026-11-17, reported): the
// front-loader's first-order high-pass at 90 Hz and low-pass at 14 kHz
// (the page's second-order low-pass is "comfortably above the audible
// range"); the Famicom's high-pass at 37 Hz. The PAL front-loader takes
// the NTSC one's, unsourced. Before it, on Line, a cartridge's own board:
// - MMC5 (NESdev Wiki, "MMC5 audio", the HVC-ETROM-02 drawing, read
//   2026-11-17; spec): the APU's audio from cartridge pin 45, the MMC5's
//   DAC and its pulses each pass 15 kOhm and 100 nF into the inverting
//   input of the MMC5's amplifier, 10 kOhm from its output back to that
//   input, the output to pin 46. So everything the console plays is high-
//   passed at 1 / (2 pi 15 k 100 n) = 106.1 Hz and scaled by -10 k / 15 k
//   (taking the inverting input as a virtual ground, as the drawing's
//   internal reference on + implies).
// - 5B (rainwarrior's measurements of a Gimmick! cartridge, NESdev forum
//   t=17745 and p=225726, September 2018, read 2026-11-17; reported, one
//   cartridge): the APU's input high-passed at 12 Hz and the 5B's own
//   channels at 20 Hz ("5B internal"), mixed in its amplifier, which is
//   linear for one square "up to and including volume 14" and above that
//   fits "a threshold set 1dB above volume 14 (or 7dB above volume 12),
//   with a compression ratio of 2.7:1 dB" (a hard knee); its output then
//   high-passed at 12 Hz. He found the onset "very immediate" and later
//   "maybe this isn't so much a compressor as just a nonlinearity ... a
//   highpass filter followed by the curve I already described". We apply
//   that curve to the instantaneous amplitude after the input high-passes,
//   so a centred square's peak, half its swing, meets the threshold at
//   volume 14 plus 1 dB (our reading; README, Known gaps, for the soft knee
//   he expected but did not publish).

#include "../Chips/Ay8910.h"
#include "../Chips/Famicom.h"
#include "../Chips/NesApu.h"
#include "../Driver/Waves.h"
#include "Board.h"

#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace protocodus::reboot {

class Console {
public:
    enum Revision { kFrontLoader, kFamicom, kPal, kRevisions };
    enum Cartridge { kNone, kVrc6, kMmc5, kN163, kFds, k5B, kCartridges };
    static constexpr int kApuChannels = 4;          // the DMC is a held level
    static constexpr int kMaxChannels = kApuChannels + 8;
    static constexpr double kPulse15 = 95.88 / (8128.0 / 15.0 + 100.0);

    // Setup.
    void Prepare(int sampleRate) {
        rate_ = sampleRate;
        DesignPinBlocker(pinBlocker_, sampleRate);
        fdsLowPass_.Design(FirstOrder::Kind::LowPass, 2000.0, sampleRate);
        mmc5Board_.Design(FirstOrder::Kind::HighPass, 1.0 / (2.0 * 3.141592653589793 * 15000.0 * 100e-9), sampleRate);
        s5bApuIn_.Design(FirstOrder::Kind::HighPass, 12.0, sampleRate);
        s5bInternal_.Design(FirstOrder::Kind::HighPass, 20.0, sampleRate);
        s5bOut_.Design(FirstOrder::Kind::HighPass, 12.0, sampleRate);
        // The 5B's curve above its threshold, (|x| / T)^(1 / 2.7), tabled here
        // so no library call runs per sample.
        for (int i = 0; i <= kCurveSteps; ++i)
            curve5B_[i] = std::pow(1.0 + i * (kCurveTop - 1.0) / kCurveSteps, 1.0 / 2.7);
    }

    // n163Channels: how many channels the Namco 163 runs (1-8).
    void Reset(int revision, int cartridge, int n163Channels, int dmcLevel) noexcept {
        revision_ = revision;
        cartridge_ = revision == kFamicom ? cartridge : kNone;
        n163Count_ = n163Channels < 1 ? 1 : n163Channels > 8 ? 8 : n163Channels;
        const nes::Region region = revision == kPal ? nes::Region::Pal : nes::Region::Ntsc;
        apu_.Reset(region);
        const nes::Clock clock = nes::kCpuClock[revision == kPal ? 1 : 0];
        cpuHz_ = static_cast<double>(clock.numerator) / static_cast<double>(clock.denominator);
        tb_.Set(clock.numerator, clock.denominator, rate_);
        mix_.Clear();
        pinBlocker_.Reset();
        fdsLowPass_.Reset();
        mmc5Board_.Reset();
        s5bApuIn_.Reset();
        s5bInternal_.Reset();
        s5bOut_.Reset();
        for (Shadow& s : shadow_)
            s = Shadow {};
        highPass_.Design(FirstOrder::Kind::HighPass, revision == kFamicom ? 37.0 : 90.0, rate_);
        if (revision == kFamicom)
            lowPass_.Design(FirstOrder::Kind::Pass, 0.0, rate_);
        else
            lowPass_.Design(FirstOrder::Kind::LowPass, 14000.0, rate_);
        highPass_.Reset();
        lowPass_.Reset();

        Sink sink { this };
        // A sound driver's start: frame IRQs off, the four channels on, the
        // pulses' sweeps negating (so the sweep cannot mute a low note:
        // NESdev, "APU Sweep"), everything silent at constant volume.
        apu_.Write(0x4017, 0x40, sink);
        apu_.Write(0x4015, 0x0F, sink);
        for (std::uint16_t a : { 0x4000, 0x4004, 0x400C })
            apu_.Write(a, 0x30, sink);
        apu_.Write(0x4001, 0x08, sink);
        apu_.Write(0x4005, 0x08, sink);
        apu_.Write(0x4008, 0x00, sink);
        apu_.Write(0x4011, static_cast<std::uint8_t>(dmcLevel & 0x7F), sink);

        CartSink cart { this };
        vrc6_.Reset();
        mmc5_.Reset();
        n163_.Reset();
        fds_.Reset();
        s5b_.Reset(16, true);
        switch (cartridge_) {
        case kMmc5:
            mmc5_.Write(0x5015, 0x03, cart);
            mmc5_.Write(0x5000, 0x30, cart);
            mmc5_.Write(0x5004, 0x30, cart);
            break;
        case kN163:
            n163_.Write(0xE000, 0x00, cart);
            // The channel count in $7F's bits 6-4; every channel's wave
            // address and length set as Start loads them.
            N163Poke(0x7F, static_cast<std::uint8_t>((n163Count_ - 1) << 4));
            break;
        case kFds:
            fds_.Write(0x4023, 0x83, cart);
            fds_.Write(0x4080, 0x80, cart);
            fds_.Write(0x4084, 0x80, cart);
            fds_.Write(0x4089, 0x00, cart);
            break;
        case k5B:
            S5BWrite(7, 0x3F);
            break;
        default:
            break;
        }
    }

    [[nodiscard]] int Channels() const noexcept {
        switch (cartridge_) {
        case kVrc6: case k5B: return kApuChannels + 3;
        case kMmc5: return kApuChannels + 2;
        case kN163: return kApuChannels + n163Count_;
        case kFds: return kApuChannels + 1;
        default: return kApuChannels;
        }
    }

    [[nodiscard]] Voice VoiceOf(int channel) const noexcept {
        switch (channel) {
        case 0: case 1: return Voice::Pulse;
        case 2: return Voice::Triangle;
        case 3: return Voice::Noise;
        default: break;
        }
        switch (cartridge_) {
        case kVrc6: return channel == 6 ? Voice::Saw : Voice::Pulse;
        case kMmc5: return Voice::Pulse;
        case kN163: return Voice::Wave;
        case kFds: return Voice::ModWave;
        default: return Voice::Tone;
        }
    }

    // True when a pulse channel has the VRC6's eight duties.
    [[nodiscard]] bool EightDuties(int channel) const noexcept { return cartridge_ == kVrc6 && channel >= 4 && channel < 6; }

    [[nodiscard]] std::int64_t CycleAt(int frame) const noexcept { return tb_.CycleAt(frame); }
    [[nodiscard]] double CpuHz() const noexcept { return cpuHz_; }

    void RunTo(std::int64_t cycle) {
        Sink sink { this };
        apu_.Run(cycle, sink);
        CartSink cart { this };
        switch (cartridge_) {
        case kVrc6: vrc6_.Run(cycle, cart); break;
        case kMmc5: mmc5_.Run(cycle, cart); break;
        case kN163: n163_.Run(cycle, cart); break;
        case kFds: fds_.Run(cycle, cart); break;
        case k5B: s5b_.Run(cycle, cart); break;
        default: break;
        }
    }

    // Each channel's period register for `hz`; false when it cannot reach it.
    [[nodiscard]] bool PeriodFor(int channel, Voice, double hz, int shape, std::int64_t& period) const noexcept {
        if (hz <= 0.0)
            return false;
        const double cpu = cpuHz_;
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
            period = NoiseFor(hz, shape != 0);
            return true;
        default:
            break;
        }
        switch (cartridge_) {
        case kVrc6:
            period = NearestCount(cpu / ((channel == 6 ? 14.0 : 16.0) * hz)) - 1;
            return period >= 0 && period <= 0xFFF;
        case kMmc5:
            // The MMC5's pulses have no sweep unit, so no low-period mute.
            period = NearestCount(cpu / (16.0 * hz)) - 1;
            return period >= 0 && period <= 0x7FF;
        case kN163: {
            // CPU x freq / (15 x channels x 65536 x length).
            period = NearestCount(hz * 15.0 * n163Count_ * 65536.0 * N163Length() / cpu);
            return period >= 1 && period <= 0x3FFFF;
        }
        case kFds:
            // CPU x pitch / 2^22 (64 steps, the wave accumulator's 2^24
            // advanced by 64 x pitch every 16 cycles when unmodulated).
            period = NearestCount(hz * 4194304.0 / cpu);
            return period >= 1 && period <= 0xFFF;
        case k5B:
            // CPU / (32 TP).
            period = NearestCount(cpu / (32.0 * hz));
            return period >= 1 && period <= 0xFFF;
        default:
            return false;
        }
    }

    void Start(int channel, const ChannelSettings& s) { Write(channel, s, true); }
    void Change(int channel, const ChannelSettings& s) { Write(channel, s, false); }

    void Stop(int channel) {
        ChannelSettings s;
        s.period = shadow_[channel].period;
        s.shape = shadow_[channel].shape;
        s.mod = shadow_[channel].mod;
        Write(channel, s, false);
    }

    // The DMC's held level ($4011), which moves the triangle and noise
    // through the mixer.
    void SetDmcLevel(int level) {
        Sink sink { this };
        apu_.Write(0x4011, static_cast<std::uint8_t>(level & 0x7F), sink);
    }

    void Render(int frames, Path path, float* left, float* right) {
        RunTo(tb_.CycleAt(frames));
        float bus[2][StepBuffer::kMaxFrames] {};
        mix_.Read(0, bus[0], frames);
        mix_.Read(1, bus[1], frames);
        // Bus 0 is the APU and every cartridge but the disk unit and the 5B;
        // bus 1 is one of those two.
        for (int i = 0; i < frames; ++i) {
            const double cart = cartridge_ == kFds ? fdsLowPass_.Process(bus[1][i]) : bus[1][i];
            double x;
            if (path == Path::Line) {
                if (cartridge_ == kMmc5)
                    x = -10.0 / 15.0 * mmc5Board_.Process(bus[0][i]);
                else if (cartridge_ == k5B)
                    x = s5bOut_.Process(Amplify5B(s5bApuIn_.Process(bus[0][i]) + s5bInternal_.Process(cart)));
                else
                    x = bus[0][i] + cart;
                x = lowPass_.Process(highPass_.Process(x));
            } else {
                x = pinBlocker_.Process(bus[0][i] + cart);
            }
            left[i] += static_cast<float>(x);
            right[i] += static_cast<float>(x);
        }
        tb_.Advance(frames);
    }

    [[nodiscard]] const nes::Apu& Apu() const noexcept { return apu_; }
    [[nodiscard]] int CartridgeType() const noexcept { return cartridge_; }

private:
    struct Shadow {
        bool active = false;
        std::int64_t period = -1;
        int volume = -1;
        int shape = -1;
        int mod = -1;
    };

    struct Sink {
        Console* board;
        void operator()(std::int64_t cycle) const { board->ApuChanged(cycle); }
    };
    struct CartSink {
        Console* board;
        void operator()(std::int64_t cycle) const { board->CartChanged(cycle); }
    };

    static int Clamp15(int v) noexcept { return v < 0 ? 0 : v > 15 ? 15 : v; }

    // The noise period whose LFSR clock, CPU / period, is nearest `hz`
    // times the register's cycle: 93 steps in short mode, so it plays in
    // tune, and 32 in long mode (ours: the key picks the noise's colour).
    [[nodiscard]] std::int64_t NoiseFor(double hz, bool shortMode) const noexcept {
        const double target = hz * (shortMode ? 93.0 : 32.0);
        const int region = apu_.RegionIndex();
        int best = 0;
        double bestRatio = 0.0;
        for (int i = 0; i < 16; ++i) {
            const double rate = cpuHz_ / nes::kNoisePeriod[region][i];
            const double ratio = rate > target ? rate / target : target / rate;
            if (bestRatio == 0.0 || ratio < bestRatio)
                bestRatio = ratio, best = i;
        }
        return best;
    }

    // Wave length in samples: 32 while every channel's 32 samples fit in
    // the RAM under the channel registers, else 16.
    // (16 bytes and 8 of registers a channel: up to five channels.)
    [[nodiscard]] int N163Length() const noexcept { return 24 * n163Count_ <= 128 ? 32 : 16; }

    void N163Poke(int address, std::uint8_t value) {
        CartSink cart { this };
        n163_.Write(0xF800, static_cast<std::uint8_t>(address & 0x7F), cart);
        n163_.Write(0x4800, value, cart);
    }

    void S5BWrite(int r, std::uint8_t value) {
        CartSink cart { this };
        s5b_.Write(r, value, cart);
    }

    void Write(int channel, const ChannelSettings& s, bool start) {
        Shadow& sh = shadow_[channel];
        const bool trigger = start || !sh.active;
        const int volume = Clamp15(s.volume);
        Sink sink { this };
        CartSink cart { this };
        switch (channel) {
        case 0: case 1: {
            const std::uint16_t base = static_cast<std::uint16_t>(0x4000 + 4 * channel);
            if (trigger || volume != sh.volume || (s.shape & 3) != sh.shape)
                apu_.Write(base, static_cast<std::uint8_t>(((s.shape & 3) << 6) | 0x30 | volume), sink);
            if (trigger || s.period != sh.period) {
                apu_.Write(base + 2, static_cast<std::uint8_t>(s.period & 0xFF), sink);
                // $4003 restarts the sequencer: a driver writes it only on a
                // trigger or when the high bits change (the click of a
                // vibrato across them is the chip's).
                if (trigger || (s.period >> 8) != (sh.period >> 8))
                    apu_.Write(base + 3, static_cast<std::uint8_t>(0xF8 | ((s.period >> 8) & 7)), sink);
            }
            sh.shape = s.shape & 3;
            break;
        }
        case 2: {
            // No volume: the linear counter held open or closed. $4008 with
            // its control bit set holds the counter at 127; clear and 0, it
            // closes at the next quarter frame. Only $400B sets the reload
            // flag that opens it again, and it leaves the phase alone.
            const bool on = volume > 0;
            const bool wasOn = sh.volume > 0;
            if (trigger || on != wasOn)
                apu_.Write(0x4008, on ? 0xFF : 0x00, sink);
            if (trigger || s.period != sh.period)
                apu_.Write(0x400A, static_cast<std::uint8_t>(s.period & 0xFF), sink);
            if (trigger || (s.period >> 8) != (sh.period >> 8) || (on && !wasOn))
                apu_.Write(0x400B, static_cast<std::uint8_t>(0xF8 | ((s.period >> 8) & 7)), sink);
            break;
        }
        case 3:
            if (trigger || volume != sh.volume)
                apu_.Write(0x400C, static_cast<std::uint8_t>(0x30 | volume), sink);
            if (trigger || s.period != sh.period || (s.shape & 1) != sh.shape)
                apu_.Write(0x400E, static_cast<std::uint8_t>(((s.shape & 1) << 7) | (s.period & 15)), sink);
            if (trigger)
                apu_.Write(0x400F, 0xF8, sink);
            sh.shape = s.shape & 1;
            break;
        default:
            WriteCartridge(channel, s, trigger, cart);
            break;
        }
        sh.active = true;
        sh.period = s.period;
        sh.volume = volume;
        sh.mod = s.mod;
    }

    void WriteCartridge(int channel, const ChannelSettings& s, bool trigger, CartSink& cart) {
        Shadow& sh = shadow_[channel];
        const int volume = Clamp15(s.volume);
        const int k = channel - kApuChannels;
        switch (cartridge_) {
        case kVrc6: {
            const std::uint16_t base = static_cast<std::uint16_t>(0x9000 + 0x1000 * k);
            if (k < 2) {
                const int duty = s.shape & 7;
                if (trigger || volume != sh.volume || duty != sh.shape)
                    vrc6_.Write(base, static_cast<std::uint8_t>((duty << 4) | volume), cart);
                sh.shape = duty;
            } else {
                // The saw's "volume" is its accumulator rate; above 42 it
                // overflows (NESdev: distorts), so the common 15 maps to 42.
                const int rate = (volume * 42 + 7) / 15;
                if (trigger || volume != sh.volume)
                    vrc6_.Write(base, static_cast<std::uint8_t>(rate), cart);
            }
            if (trigger || s.period != sh.period) {
                vrc6_.Write(base + 1, static_cast<std::uint8_t>(s.period & 0xFF), cart);
                vrc6_.Write(base + 2, static_cast<std::uint8_t>(0x80 | ((s.period >> 8) & 15)), cart);
            }
            break;
        }
        case kMmc5: {
            const std::uint16_t base = static_cast<std::uint16_t>(0x5000 + 4 * k);
            if (trigger || volume != sh.volume || (s.shape & 3) != sh.shape)
                mmc5_.Write(base, static_cast<std::uint8_t>(((s.shape & 3) << 6) | 0x30 | volume), cart);
            if (trigger || s.period != sh.period) {
                mmc5_.Write(base + 2, static_cast<std::uint8_t>(s.period & 0xFF), cart);
                if (trigger || (s.period >> 8) != (sh.period >> 8))
                    mmc5_.Write(base + 3, static_cast<std::uint8_t>(0xF8 | ((s.period >> 8) & 7)), cart);
            }
            sh.shape = s.shape & 3;
            break;
        }
        case kN163: {
            // Channel k is the chip's channel 8 - k, registers at
            // $78 - 8k; its wave at k times the wave's bytes.
            const int reg = 0x78 - 8 * k;
            const int length = N163Length();
            const int shape = s.shape % kWaveCount;
            // The chip reads its RAM as it plays, so a held note takes a new
            // wave as it is written.
            if (shape != sh.shape) {
                for (int i = 0; i < length / 2; ++i) {
                    const int step = 32 / length;
                    const int a = kWaves[shape].samples[(2 * i) * step];
                    const int b = kWaves[shape].samples[(2 * i + 1) * step];
                    // Low nibble first (NESdev).
                    N163Poke(k * (length / 2) + i, static_cast<std::uint8_t>(a | (b << 4)));
                }
                N163Poke(reg + 6, static_cast<std::uint8_t>(k * length));
                sh.shape = shape;
            }
            if (trigger || s.period != sh.period) {
                N163Poke(reg + 0, static_cast<std::uint8_t>(s.period & 0xFF));
                N163Poke(reg + 2, static_cast<std::uint8_t>((s.period >> 8) & 0xFF));
                N163Poke(reg + 4, static_cast<std::uint8_t>(((256 - length) & 0xFC) | ((s.period >> 16) & 3)));
            }
            if (trigger) {
                N163Poke(reg + 1, 0);
                N163Poke(reg + 3, 0);
                N163Poke(reg + 5, 0);
            }
            if (trigger || volume != sh.volume) {
                const std::uint8_t high = reg == 0x78 ? static_cast<std::uint8_t>((n163Count_ - 1) << 4) : 0;
                N163Poke(reg + 7, static_cast<std::uint8_t>(high | volume));
            }
            break;
        }
        case kFds: {
            const int shape = s.shape % kWaveCount;
            // The wave is writable while $4089 holds the output (NESdev), so
            // a held note takes a new one too.
            if (shape != sh.shape) {
                fds_.Write(0x4089, 0x80, cart);
                for (int i = 0; i < 64; ++i)
                    fds_.Write(static_cast<std::uint16_t>(0x4040 + i), kWaves64[shape][i], cart);
                fds_.Write(0x4089, 0x00, cart);
                sh.shape = shape;
            }
            if (trigger || volume != sh.volume)
                fds_.Write(0x4080, static_cast<std::uint8_t>(0x80 | ((volume * 32 + 7) / 15)), cart);
            // The modulator at the carrier's own rate (its accumulator runs
            // on the same scale), depth from the part's mod (0-63).
            const int depth = s.mod < 0 ? 0 : s.mod > 63 ? 63 : s.mod;
            if (trigger) {
                fds_.Write(0x4087, 0x80, cart);
                for (int i = 0; i < 32; ++i)
                    fds_.Write(0x4088, kModTable[i], cart);
                fds_.Write(0x4085, 0x00, cart);
            }
            if (trigger || depth != sh.mod)
                fds_.Write(0x4084, static_cast<std::uint8_t>(0x80 | depth), cart);
            if (trigger || s.period != sh.period) {
                fds_.Write(0x4082, static_cast<std::uint8_t>(s.period & 0xFF), cart);
                fds_.Write(0x4083, static_cast<std::uint8_t>((s.period >> 8) & 15), cart);
                fds_.Write(0x4086, static_cast<std::uint8_t>(s.period & 0xFF), cart);
                fds_.Write(0x4087, static_cast<std::uint8_t>((s.period >> 8) & 15), cart);
            }
            break;
        }
        case k5B: {
            if (trigger || s.period != sh.period) {
                S5BWrite(2 * k, static_cast<std::uint8_t>(s.period & 0xFF));
                S5BWrite(2 * k + 1, static_cast<std::uint8_t>((s.period >> 8) & 15));
            }
            if (trigger || volume != sh.volume)
                S5BWrite(8 + k, static_cast<std::uint8_t>(volume));
            if (trigger) {
                // Tone on (bit k clear), noise off, in the shared mixer.
                const std::uint8_t mixer = s5b_.Register(7);
                S5BWrite(7, static_cast<std::uint8_t>((mixer & ~(1 << k)) | (8 << k)));
            }
            break;
        }
        default:
            break;
        }
    }

    void ApuChanged(std::int64_t cycle) noexcept {
        const int p = apu_.Out(nes::Apu::kPulse1) + apu_.Out(nes::Apu::kPulse2);
        const int t = apu_.Out(nes::Apu::kTriangle);
        const int n = apu_.Out(nes::Apu::kNoise);
        const int d = apu_.Out(nes::Apu::kDmc);
        double level = 0.0;
        if (p > 0)
            level += 95.88 / (8128.0 / p + 100.0);
        const double tnd = t / 8227.0 + n / 12241.0 + d / 22638.0;
        if (tnd > 0.0)
            level += 159.79 / (1.0 / tnd + 100.0);
        const float levels[2] = { static_cast<float>(level), 0.0f };
        mix_.Set(0, tb_.Position(cycle, StepBuffer::kSubBits), levels);
    }

    void CartChanged(std::int64_t cycle) noexcept {
        double level = 0.0;
        int bus = 0;
        switch (cartridge_) {
        case kVrc6:
            level = -(kPulse15 / 15.0) * (vrc6_.Out(0) + vrc6_.Out(1) + vrc6_.Out(2));
            break;
        case kMmc5:
            level = -(kPulse15 / 15.0) * (mmc5_.Out(0) + mmc5_.Out(1));
            break;
        case kN163:
            level = n163_.Out() * kN163Step;
            break;
        case kFds: {
            static constexpr double kMaster[4] = { 1.0, 2.0 / 3.0, 2.0 / 4.0, 2.0 / 5.0 };
            level = fds_.Out() * kFdsStep * kMaster[fds_.MasterVolume()];
            bus = 1;
            break;
        }
        case k5B:
            for (int c = 0; c < 3; ++c)
                level += k5BLevel[s5b_.Out(c)];
            bus = 1;
            break;
        default:
            break;
        }
        float levels[2] = { 0.0f, 0.0f };
        levels[bus] = static_cast<float>(level);
        mix_.Set(1, tb_.Position(cycle, StepBuffer::kSubBits), levels);
    }

    // 12 dB over the APU's loudest square, on 225 steps peak to peak.
    static constexpr double kN163Step = kPulse15 * 3.9810717055349722 / 225.0;
    // 2.4 times the APU square, on the wave's 63 x 32 peak to peak.
    static constexpr double kFdsStep = kPulse15 * 2.4 / (63.0 * 32.0);
    // 5B: code 25 1.3 dB under kPulse15, 1.5 dB a code, 0 and 1 silent.
    static constexpr double k5BFull = kPulse15 * 2.4266100950824154;   // 10^(7.7 / 20)
    static constexpr double k5BLevel[32] = {
        0.0, 0.0,
        k5BFull * 0.0066834391756861, k5BFull * 0.0079432823472428,
        k5BFull * 0.0094406087628592, k5BFull * 0.0112201845430196,
        k5BFull * 0.0133352143216332, k5BFull * 0.0158489319246111,
        k5BFull * 0.0188364908948980, k5BFull * 0.0223872113856834,
        k5BFull * 0.0266072505979881, k5BFull * 0.0316227766016838,
        k5BFull * 0.0375837404288444, k5BFull * 0.0446683592150963,
        k5BFull * 0.0530884444230988, k5BFull * 0.0630957344480193,
        k5BFull * 0.0749894209332456, k5BFull * 0.0891250938133746,
        k5BFull * 0.1059253725177289, k5BFull * 0.1258925411794168,
        k5BFull * 0.1496235656094433, k5BFull * 0.1778279410038923,
        k5BFull * 0.2113489039836647, k5BFull * 0.2511886431509580,
        k5BFull * 0.2985382618917959, k5BFull * 0.3548133892335755,
        k5BFull * 0.4216965034285822, k5BFull * 0.5011872336272722,
        k5BFull * 0.5956621435290105, k5BFull * 0.7079457843841379,
        k5BFull * 0.8413951416451951, k5BFull * 1.0000000000000000,
    };

    // The 5B amplifier's threshold: a code-29 (volume 14) square's peak once
    // centred, half its swing, plus 1 dB: 10^(-2 / 20) / 2 of code 31.
    static constexpr double k5BKnee = k5BFull * 0.3971641173621406;
    // The curve's table runs to 32 times the threshold, past the loudest
    // sum the APU and three 5B squares can reach (about 16); beyond, it
    // continues along its last step.
    static constexpr int kCurveSteps = 496;
    static constexpr double kCurveTop = 32.0;

    [[nodiscard]] double Amplify5B(double x) const noexcept {
        const double a = x < 0.0 ? -x : x;
        if (a <= k5BKnee)
            return x;
        const double u = (a / k5BKnee - 1.0) * (kCurveSteps / (kCurveTop - 1.0));
        int i = static_cast<int>(u);
        if (i > kCurveSteps - 1)
            i = kCurveSteps - 1;
        const double y = k5BKnee * (curve5B_[i] + (u - i) * (curve5B_[i + 1] - curve5B_[i]));
        return x < 0.0 ? -y : y;
    }

    int rate_ = 48000;
    int revision_ = kFrontLoader;
    int cartridge_ = kNone;
    int n163Count_ = 4;
    double cpuHz_ = 19687500.0 / 11.0;
    nes::Apu apu_;
    famicom::Vrc6 vrc6_;
    famicom::Mmc5 mmc5_;
    famicom::Namco163 n163_;
    famicom::Fds fds_;
    ay::Chip s5b_;
    Timebase tb_;
    StepMix<2, 2> mix_;
    FirstOrder pinBlocker_, fdsLowPass_, highPass_, lowPass_;
    FirstOrder mmc5Board_, s5bApuIn_, s5bInternal_, s5bOut_;
    double curve5B_[kCurveSteps + 1] {};
    Shadow shadow_[kMaxChannels];
};

} // namespace protocodus::reboot
