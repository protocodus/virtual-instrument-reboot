#pragma once

// The console board blackbox: the 2A03's APU (Chips/NesApu.h) on the CPU
// clock, with a Famicom cartridge's sound chip beside it (Chips/Famicom.h, and
// the 5B as Chips/Ay8910.h at a 16-cycle tick), their DACs, the mixer, the 6502
// CPU bus, and the machine's output stage.
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
//   polarity of all MMC5 channels is reversed" (reported). Pulses mix at
//   the APU pulse's step and $5011 PCM at the 7-bit $4011 step (our reading;
//   Known gaps).
// - Namco 163: in 1-channel mode its loudest square, 225 steps peak to peak
//   at (15 - 8) and (0 - 8) times 15, is 11 to 13 dB louder than the APU's
//   loudest square for submapper 3's cartridges; we take the middle, 12 dB.
//   Polarity unstated; taken as the APU's.
// - Disk unit: about 2.4 times a 2A03 square at full volume, same polarity,
//   through its own RC low-pass (2 kHz at $4089 master 0, scaling inversely
//   with Yuri213212's measured 2-2R DAC output impedance across $4089 steps 0..3:
//   12k, 8k, 6k, 4.8k -> 2, 3, 4, 5 kHz; reported, "FDS audio" & NESdev);
//   the master volume divides by 1, 1.5, 2 or 2.5 ("FDS audio", reported).
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
#include "Board.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace protocodus::reboot {

class Console {
    // The curve's table runs to 32 times the threshold, past the loudest
    // sum the APU and three 5B squares can reach (about 16); beyond, it
    // continues along its last step.
    static constexpr int kCurveSteps = 496;
    static constexpr double kCurveTop = 32.0;

public:
    enum Revision { kFrontLoader, kFamicom, kPal, kRevisions };
    enum Cartridge { kNone, kVrc6, kMmc5, kN163, kFds, k5B, kCartridges };
    static constexpr double kPulse15 = 95.88 / (8128.0 / 15.0 + 100.0);

    // Between two units. The crystal (NESdev Wiki, "Cycle reference chart",
    // read 2026-11-18; reported): the master clock is "21.477272 MHz ± 40 Hz"
    // on NTSC and "26.601712 MHz ± 50 Hz" on PAL, 1.9 ppm either way. The
    // warm-up is up to 262144 cycles (ours: only where it leaves the unit's
    // video frame, its dividers and its noise register matters, and that
    // is eight frames and twice round the register's 32767 steps at the 4
    // cycles a step it runs at from power-on). Brad Smith's DAC
    // measurements above found "variance between units" and give no
    // figure: the level, 5 %, and the corners, 10 %, are what 5 % resistors
    // and 10 % capacitors would give (estimate; README, Known gaps).
    static constexpr UnitSpread kSpread { 190, 262144, 0.05, 0.10 };

    // The cartridge's compression curve does not depend on the sample rate
    // or board. An engine prepares it once for all its machines.
    using Curve5B = std::array<double, kCurveSteps + 1>;
    static Curve5B Make5BCurve() {
        Curve5B levels {};
        for (int i = 0; i <= kCurveSteps; ++i)
            levels[i] = std::pow(1.0 + i * (kCurveTop - 1.0) / kCurveSteps, 1.0 / 2.7);
        return levels;
    }

    void Prepare(int sampleRate) { Prepare(sampleRate, Make5BCurve()); }

    // Setup, for the unit with these traits (Board.h): every corner of its
    // output stage and of the cartridges it plays is its own.
    void Prepare(int sampleRate, const Curve5B& levels, const UnitTraits& traits = {}) {
        curve5B_ = levels;
        traits_ = traits;
        const double corner = traits.corner;
        frontHighPass_ = FirstOrder::ComputeCoefficients(FirstOrder::Kind::HighPass, 90.0 * corner, sampleRate);
        famicomHighPass_ = FirstOrder::ComputeCoefficients(FirstOrder::Kind::HighPass, 37.0 * corner, sampleRate);
        frontLowPass_ = FirstOrder::ComputeCoefficients(FirstOrder::Kind::LowPass, 14000.0 * corner, sampleRate);
        rate_ = sampleRate;
        DesignPinBlocker(pinBlocker_, sampleRate);
        // Reported: NESdev "FDS audio" (~2 kHz RC low-pass) + Yuri213212's
        // RP2C33 2-2R DAC output impedance measurements across $4089 steps 0..3
        // (~12k, 8k, 6k, 4.8k -> corner scales proportionally to 2, 3, 4, 5 kHz).
        static constexpr double kFdsCornerHz[4] = { 2000.0, 3000.0, 4000.0, 5000.0 };
        for (int m = 0; m < 4; ++m)
            fdsLowPassCoeffs_[m] = FirstOrder::ComputeCoefficients(FirstOrder::Kind::LowPass, kFdsCornerHz[m] * corner, sampleRate);
        fdsLowPass_.SetCoefficients(FirstOrder::Kind::LowPass, fdsLowPassCoeffs_[0]);
        mmc5Board_.Design(FirstOrder::Kind::HighPass, corner / (2.0 * 3.141592653589793 * 15000.0 * 100e-9), sampleRate);
        s5bApuIn_.Design(FirstOrder::Kind::HighPass, 12.0 * corner, sampleRate);
        s5bInternal_.Design(FirstOrder::Kind::HighPass, 20.0 * corner, sampleRate);
        s5bOut_.Design(FirstOrder::Kind::HighPass, 12.0 * corner, sampleRate);
    }

    // Switched on: the chips as power leaves them, nothing written yet. The
    // unit's crystal runs `trim` (Timebase's units) off its own frequency.
    void Reset(int revision, int cartridge, std::int64_t trim = 0) noexcept {
        revision_ = revision;
        cartridge_ = revision == kFamicom ? cartridge : kNone;
        const nes::Region region = revision == kPal ? nes::Region::Pal : nes::Region::Ntsc;
        apu_.Reset(region);
        const nes::Clock clock = nes::kCpuClock[revision == kPal ? 1 : 0];
        cpuHz_ = static_cast<double>(clock.numerator) / static_cast<double>(clock.denominator);
        tb_.Set(clock.numerator, clock.denominator, rate_, traits_.clock + trim);
        mix_.Clear();
        bus_.Clear();
        pinBlocker_.Reset();
        fdsLowPass_.SetCoefficients(FirstOrder::Kind::LowPass, fdsLowPassCoeffs_[0]);
        fdsLowPass_.Reset();
        mmc5Board_.Reset();
        s5bApuIn_.Reset();
        s5bInternal_.Reset();
        s5bOut_.Reset();
        const bool famicom = revision == kFamicom;
        highPass_.SetCoefficients(FirstOrder::Kind::HighPass, famicom ? famicomHighPass_ : frontHighPass_);
        lowPass_.SetCoefficients(famicom ? FirstOrder::Kind::Pass : FirstOrder::Kind::LowPass,
                                 famicom ? FirstOrder::Coefficients {} : frontLowPass_);
        highPass_.Reset();
        lowPass_.Reset();

        vrc6_.Reset();
        mmc5_.Reset();
        n163_.Reset();
        fds_.Reset();
        s5b_.Reset(16, true);
        s5bAddr_ = 0;
        fdsMaster_ = 0;

        ApuChanged(0);
        CartChanged(0);
        SettleOutputStages();
    }

    // Schedules a CPU bus write onto the console/cartridge memory map using
    // 6502 store timing (4 CPU cycles for STA abs when the value is already in
    // A or to an auto-incrementing streaming port; 6 cycles for LDA #imm; STA abs).
    void Write(std::uint16_t address, std::uint8_t value) noexcept {
        const int step = (bus_.HasLast() && (bus_.LastValue() == value || address == 0x4800 || address == 0x4088)) ? 4 : 6;
        if (bus_.Full()) {
            const BusWrite w = bus_.Pop();
            WriteImmediate(w.address, w.value);
        }
        const std::int64_t now = apu_.Now();
        if (bus_.CanExecuteNow(now)) {
            WriteImmediate(address, value);
            bus_.MarkImmediate(now, step, address, value);
        } else {
            bus_.Schedule(now, step, address, value);
        }
    }

    [[nodiscard]] std::uint8_t Read(std::uint16_t address) noexcept {
        if (address == 0x4015)
            return apu_.ReadStatus();
        if (cartridge_ == kN163 && address >= 0x4800 && address < 0x5000)
            return n163_.ReadData();
        return 0xFF;
    }

    // Ours: a unit is heard as one that has stood since its driver's start.
    // Delivers the boot's writes, runs the unit's warm-up with every step
    // landing at once, settles the output stages on the rest levels that
    // leaves (the triangle's held step and the DMC's Bias), and starts the
    // sample block there.
    void FinishBoot() noexcept {
        settling_ = true;
        const std::int64_t end = bus_.Empty() ? 0 : bus_.LastCycle() + 1;
        RunTo(end > traits_.warmUp ? end : traits_.warmUp);
        settling_ = false;
        SettleOutputStages();
        tb_.Restart(apu_.Now());
        bus_.ResetTiming(apu_.Now());
    }

    [[nodiscard]] std::int64_t CycleAt(int frame) const noexcept { return tb_.CycleAt(frame); }
    [[nodiscard]] double CpuHz() const noexcept { return cpuHz_; }
    [[nodiscard]] std::int64_t Now() const noexcept { return apu_.Now(); }
    // Where a cycle of this block falls in it, in the step buffer's
    // positions, and the first cycle at or after such a position: how one
    // unit's moment is found on another's clock.
    [[nodiscard]] std::int64_t Position(std::int64_t cycle) const noexcept {
        return settling_ ? 0 : tb_.Position(cycle, StepBuffer::kSubBits);
    }
    [[nodiscard]] std::int64_t CycleAtPosition(std::int64_t position) const noexcept {
        return tb_.CycleAtPosition(position, StepBuffer::kSubBits);
    }
    // A trimmer on the unit's crystal, turned at the block's start.
    void SetTrim(std::int64_t trim) noexcept { tb_.Trim(traits_.clock + trim); }

    void RunTo(std::int64_t cycle) {
        Sink sink { this };
        CartSink cart { this };
        while (!bus_.Empty() && bus_.Front().cycle < cycle) {
            const BusWrite w = bus_.Pop();
            if (w.cycle > apu_.Now())
                RunChipsTo(w.cycle, sink, cart);
            WriteImmediate(w.address, w.value);
        }
        if (apu_.Now() < cycle)
            RunChipsTo(cycle, sink, cart);
    }

    void Render(int frames, Path path, float* left, float* right) {
        RunTo(tb_.CycleAt(frames));
        float bus[2][StepBuffer::kMaxFrames] {};
        mix_.Read(0, bus[0], frames);
        mix_.Read(1, bus[1], frames);
        // Bus 0 is the APU and every cartridge but the disk unit and the 5B;
        // bus 1 is one of those two.
        for (int i = 0; i < frames; ++i) {
            if (i < fdsScheduleLength_ && fdsSchedule_[i] != 0)
                fdsLowPass_.SetCoefficients(FirstOrder::Kind::LowPass, fdsLowPassCoeffs_[fdsSchedule_[i] - 1]);
            const double cart = cartridge_ == kFds ? fdsLowPass_.Process(bus[1][i]) : bus[1][i];
            // Path selects the tap to hear. The pin blocker and the
            // cartridge/line circuitry continue to see the same board.
            const double mix = bus[0][i] + cart;
            const double pin = pinBlocker_.Process(mix);
            double lineIn = mix;
            if (cartridge_ == kMmc5)
                lineIn = -10.0 / 15.0 * mmc5Board_.Process(bus[0][i]);
            else if (cartridge_ == k5B)
                lineIn = s5bOut_.Process(Amplify5B(s5bApuIn_.Process(bus[0][i]) + s5bInternal_.Process(cart)));
            const double line = lowPass_.Process(highPass_.Process(lineIn));
            const float output = static_cast<float>((path == Path::Line ? line : pin) * traits_.level);
            left[i] += output;
            right[i] += output;
        }
        if (fdsScheduleLength_ > 0) {
            // Only the BLEP delay can carry changes across this block boundary.
            const int remaining = fdsScheduleLength_ > frames ? fdsScheduleLength_ - frames : 0;
            std::memmove(fdsSchedule_.data(), fdsSchedule_.data() + frames, remaining);
            std::memset(fdsSchedule_.data() + remaining, 0, fdsScheduleLength_ - remaining);
            fdsScheduleLength_ = remaining;
        }
        tb_.Advance(frames);
    }

    [[nodiscard]] const nes::Apu& Apu() const noexcept { return apu_; }
    [[nodiscard]] int Revision() const noexcept { return revision_; }
    [[nodiscard]] int CartridgeType() const noexcept { return cartridge_; }

private:
    struct Sink {
        Console* board;
        void operator()(std::int64_t cycle) const { board->ApuChanged(cycle); }
    };
    struct CartSink {
        Console* board;
        void operator()(std::int64_t cycle) const { board->CartChanged(cycle); }
    };

    void RunChipsTo(std::int64_t cycle, Sink& sink, CartSink& cart) {
        apu_.Run(cycle, sink);
        switch (cartridge_) {
        case kVrc6: vrc6_.Run(cycle, cart); break;
        case kMmc5: mmc5_.Run(cycle, cart); break;
        case kN163: n163_.Run(cycle, cart); break;
        case kFds: fds_.Run(cycle, cart); break;
        case k5B: s5b_.Run(cycle, cart); break;
        default: break;
        }
    }

    void WriteImmediate(std::uint16_t address, std::uint8_t value) noexcept {
        if (address >= 0x4000 && address <= 0x4017) {
            Sink sink { this };
            apu_.Write(address, value, sink);
            return;
        }
        CartSink cart { this };
        switch (cartridge_) {
        case kVrc6:
            if (address >= 0x9000 && address < 0xC000)
                vrc6_.Write(address, value, cart);
            break;
        case kMmc5:
            if (address >= 0x5000 && address <= 0x5015)
                mmc5_.Write(address, value, cart);
            break;
        case kN163:
            if ((address >= 0x4800 && address < 0x5000) || (address >= 0xE000 && address < 0xE800) || address >= 0xF800)
                n163_.Write(address, value, cart);
            break;
        case kFds:
            if (address == 0x4023 || (address >= 0x4040 && address <= 0x408A))
                fds_.Write(address, value, cart);
            break;
        case k5B:
            if (address >= 0xC000 && address < 0xE000)
                s5bAddr_ = value;
            else if (address >= 0xE000 && s5bAddr_ < 16)
                s5b_.Write(s5bAddr_, value, cart);
            break;
        default:
            break;
        }
    }

    void SettleOutputStages() noexcept {
        // Ours: a machine chosen is heard as one that has stood at rest, its
        // output stages settled on the level the writes above leave (the
        // triangle held on its first step, the DMC on Bias), not as one
        // switched on that moment, whose step from nothing would thump
        // through the Pin blocker on every reset.
        const double apu = mix_.Land(0), other = mix_.Land(1);
        fdsMaster_ = fds_.MasterVolume() & 3;
        fdsSchedule_.fill(0);
        fdsScheduleLength_ = 0;
        fdsLowPass_.SetCoefficients(FirstOrder::Kind::LowPass, fdsLowPassCoeffs_[fdsMaster_]);
        fdsLowPass_.Settle(other);
        pinBlocker_.Settle(apu + other);
        mmc5Board_.Settle(apu);
        s5bApuIn_.Settle(apu);
        s5bInternal_.Settle(other);
        s5bOut_.Settle(0.0);
        highPass_.Settle(cartridge_ == kMmc5 || cartridge_ == k5B ? 0.0 : apu + other);
        lowPass_.Settle(0.0);
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
        mix_.Set(0, Position(cycle), levels);
    }

    void CartChanged(std::int64_t cycle) noexcept {
        const std::int64_t position = Position(cycle);
        double level = 0.0;
        int bus = 0;
        switch (cartridge_) {
        case kVrc6:
            level = -(kPulse15 / 15.0) * (vrc6_.Out(0) + vrc6_.Out(1) + vrc6_.Out(2));
            break;
        case kMmc5:
            level = -(kPulse15 / 15.0) * (mmc5_.Out(0) + mmc5_.Out(1)) - kMmc5PcmStep * mmc5_.Out(2);
            break;
        case kN163:
            level = n163_.Out() * kN163Step;
            break;
        case kFds: {
            static constexpr double kMaster[4] = { 1.0, 2.0 / 3.0, 2.0 / 4.0, 2.0 / 5.0 };
            const int m = fds_.MasterVolume() & 3;
            if (m != fdsMaster_) {
                fdsMaster_ = m;
                const int sample = static_cast<int>(position >> StepBuffer::kSubBits) + blep::kDelay;
                // Several writes can land on one sample. Only its final setting
                // reaches the RC, while the DAC steps retain their sub-sample times.
                fdsSchedule_[sample] = static_cast<std::uint8_t>(m + 1);
                fdsScheduleLength_ = sample + 1;
            }
            level = fds_.Out() * kFdsStep * kMaster[m];
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
        mix_.Set(1, position, levels);
    }

    // MMC5 $5011 PCM step equivalent to 2A03 $4011 at 127 (inverted).
    static constexpr double kMmc5PcmStep = (159.79 / (22638.0 / 127.0 + 100.0)) / 127.0;
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
    UnitTraits traits_;
    bool settling_ = false;   // FinishBoot: every step lands at the block's start
    int revision_ = kFrontLoader;
    int cartridge_ = kNone;
    double cpuHz_ = 19687500.0 / 11.0;
    nes::Apu apu_;
    famicom::Vrc6 vrc6_;
    famicom::Mmc5 mmc5_;
    famicom::Namco163 n163_;
    famicom::Fds fds_;
    ay::Chip s5b_;
    std::uint8_t s5bAddr_ = 0;
    Timebase tb_;
    BusWriteQueue<> bus_;
    StepMix<2, 2> mix_;
    FirstOrder pinBlocker_, fdsLowPass_, highPass_, lowPass_;
    FirstOrder::Coefficients fdsLowPassCoeffs_[4] {};
    FirstOrder mmc5Board_, s5bApuIn_, s5bInternal_, s5bOut_;
    FirstOrder::Coefficients frontHighPass_, famicomHighPass_, frontLowPass_;
    Curve5B curve5B_ {};
    int fdsMaster_ = 0, fdsScheduleLength_ = 0;
    std::array<std::uint8_t, StepBuffer::kMaxFrames + blep::kDelay + 1> fdsSchedule_ {};
};

} // namespace protocodus::reboot
