#pragma once

// The handheld board blackbox: the Game Boy's APU (Chips/GameBoyApu.h) on its
// 4194304 Hz clock, its four DACs, its stereo mixer and master volume, the
// coupling capacitor and Color feedback amplifier after them, and the SM83
// CPU bus interface.
//
// Revisions: Original (DMG) and Color (CGB), which differ in the APU's
// quirks and in their output stage.
//
// DACs (Pan Docs, Audio details, read 2026-11-17; spec): a DAC is on when
// NRx2 & $F8 is not 0 (NR30 bit 7 for the wave channel); on, it maps digital
// 0 to 15 linearly to analog +1 to -1 ("a disabled channel outputs 0, which
// an enabled DAC will dutifully convert into analog 1"); off, it gives 0.
// On Line, disabling an active DAC decays its last analog level toward 0 with
// a 50 us RC time constant (reported: SameBoy MIT Core/apu.c DAC_DECAY_SPEED =
// 20000, read for the physical rule, no code copied).
//
// The mixer adds the channels NR51 sends to each side and scales each side
// by NR50's volume + 1 over 8. Pin is that sum. Line adds the capacitor:
// Pan Docs' out = in - capacitor, capacitor = in - out * factor, passing the
// mixer sum plus any 50 us DAC turn-off residual continuously through it (and
// Color's feedback amplifier) so disabling the last DAC decays smoothly.
//
// Line's corners (reported) are fitted to Herbert Weixelbaum's recordings
// (herbertweixelbaum.com/comparison.htm, "game boy sound comparison"): each
// machine at its highest volume into an audio interface, playing LSDJ's
// 50 % pulse at C3, "great C" (65.41 Hz), shown as waveforms. A first-order
// high-pass fits the original at 26 Hz (its "play it loud" edition at 29 Hz),
// where Pan Docs' factor 0.999958 per clock gives 28 Hz; the original keeps
// that factor. The Color's waveform falls away from each edge and undershoots,
// which one pole cannot draw; two first-order high-passes fit it, at 176 Hz
// and 47 Hz (the pairs within 20 % of the best fit's error span 132 to 216 Hz
// and 36 to 66 Hz; Tools/fit_handheld_line.py). The Color takes the 47 Hz as its
// capacitor and the 176 Hz after it, which Ken Shirriff's die analysis of
// its IR3R53N says has "an external feedback capacitor, which causes it to
// act as a high-pass filter" (righto.com, 2020-06). Pan Docs' "use 0.998943
// for MGB&CGB", one pole near 706 Hz, is neither. Those recordings are the
// socket into a line input, where its coupling capacitor's corner is under
// 1 Hz. Into headphones (SetHeadphones) the capacitor and the load make one
// more high-pass: 100 uF, the original's headphone board's two capacitors
// (C1, C2 in its parts lists; reported), into 32 Ohm, a typical headphone
// with nothing in series (estimate), about 50 Hz; the Color's socket is
// taken as the original's (sibling: its value is not published). The
// amplifier's gain and clipping and the speaker are not modelled (README,
// Known gaps).

#include "../Chips/GameBoyApu.h"
#include "Board.h"

#include <cmath>
#include <cstdint>

namespace protocodus::reboot {

class Handheld {
public:
    enum Revision { kOriginal, kColor, kRevisions };
    static constexpr int kChannels = gb::Apu::kChannels;
    static constexpr double kFrameHz = 4194304.0 / 70224.0; // the LCD's 59.73 Hz
    static constexpr std::int64_t kFrameCycles = 70224;

    // Between two units. The crystal (Pan Docs, Specifications, read
    // 2026-11-18; reported): "Real DMG units tend to run about 50-70 PPM
    // slow. Accuracy of other models is unknown." So units lie 20 ppm apart:
    // 10 ppm either side of the nominal unit (ours: the nominal unit keeps
    // the documents' 4194304 Hz; sibling for the Color). The warm-up is up
    // to a second (ours: only where it leaves the unit's dividers and its
    // video frame matters). The level, 5 %, and the corners, 10 %, are what
    // 5 % resistors and 10 % capacitors around the amplifier would give
    // (estimate; README, Known gaps).
    static constexpr UnitSpread kSpread { 1000, 4194304, 0.05, 0.10 };

    // Setup, for the unit with these traits (Board.h).
    void Prepare(int sampleRate, const UnitTraits& traits = {}) {
        rate_ = sampleRate;
        traits_ = traits;
        // A corner moves with its capacitor: 1 - factor is the corner's
        // measure (kColorCapacitor).
        capacitorDesign_[kOriginal].Design(1.0 - (1.0 - 0.999958) * traits.corner, sampleRate);
        capacitorDesign_[kColor].Design(1.0 - (1.0 - kColorCapacitor) * traits.corner, sampleRate);
        colorHighPass_ = FirstOrder::ComputeCoefficients(FirstOrder::Kind::HighPass, 176.0 * traits.corner, sampleRate);
        socket_[0].Design(FirstOrder::Kind::HighPass, kSocketHz * traits.corner, sampleRate);
        socket_[1].Design(FirstOrder::Kind::HighPass, kSocketHz * traits.corner, sampleRate);
        DesignPinBlocker(pinBlocker_[0], sampleRate);
        DesignPinBlocker(pinBlocker_[1], sampleRate);
        // Reported: SameBoy (MIT, Core/apu.c) models pre-GBA physical DAC
        // off decay at DAC_DECAY_SPEED = 20000 (50 us).
        static constexpr double kDacFadeSeconds = 1.0 / 20000.0;
        dacFadeFactor_ = std::exp(-1.0 / (kDacFadeSeconds * static_cast<double>(sampleRate)));
    }

    // Switched on: the chip as power leaves it, nothing written yet. The
    // unit's crystal runs `trim` (Timebase's units) off its own frequency.
    void Reset(int revision, std::int64_t trim = 0) noexcept {
        revision_ = revision;
        apu_.Reset(revision == kColor ? gb::Model::Cgb : gb::Model::Dmg);
        tb_.Set(gb::kClock, 1, rate_, traits_.clock + trim);
        mix_.Clear();
        dacOff_[0].Clear();
        dacOff_[1].Clear();
        bus_.Clear();
        for (int s = 0; s < 2; ++s) {
            const bool color = revision == kColor;
            capacitor_[s] = capacitorDesign_[color ? kColor : kOriginal];
            amplifier_[s].SetCoefficients(color ? FirstOrder::Kind::HighPass : FirstOrder::Kind::Pass,
                                          color ? colorHighPass_ : FirstOrder::Coefficients {});
            amplifier_[s].Reset();
            pinBlocker_[s].Reset();
            socket_[s].Reset();
            dacResidual_[s] = 0.0;
        }
        for (int c = 0; c < kChannels; ++c) {
            wasDacOn_[c] = false;
            lastChSide_[c][0] = 0.0, lastChSide_[c][1] = 0.0;
        }
    }

    // Schedules a CPU bus write to $FF10-$FF3F using SM83 bus timing (8 T-cycles
    // for repeated LDH (C), A writes; 12 T-cycles for LDH (a8), A; Pan Docs).
    void Write(std::uint16_t address, std::uint8_t value) noexcept {
        const int step = (bus_.HasLast() && bus_.LastAddress() == address) ? 8 : 12;
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

    [[nodiscard]] std::uint8_t Read(std::uint16_t address) noexcept { return apu_.Read(address); }

    // Ours: a unit is heard as one that has stood since its driver's start,
    // not as one switched on that moment. Delivers the boot's writes, runs
    // the unit's warm-up with every step landing at once, settles the output
    // stages on the level that leaves, and starts the sample block there.
    void FinishBoot() noexcept {
        settling_ = true;
        const std::int64_t end = bus_.Empty() ? 0 : bus_.LastCycle() + 1;
        RunTo(end > traits_.warmUp ? end : traits_.warmUp);
        settling_ = false;
        const double left = mix_.Land(0);
        const double right = mix_.Land(1);
        pinBlocker_[0].Settle(left);
        pinBlocker_[1].Settle(right);
        capacitor_[0].Settle(left);
        capacitor_[1].Settle(right);
        for (int s = 0; s < 2; ++s) {
            amplifier_[s].Reset();
            socket_[s].Reset();
            dacOff_[s].Clear();
            dacResidual_[s] = 0.0;
        }
        tb_.Restart(apu_.Now());
        bus_.ResetTiming(apu_.Now());
    }

    [[nodiscard]] std::int64_t CycleAt(int frame) const noexcept { return tb_.CycleAt(frame); }
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
    // Line into headphones, else into a line input. The socket's high-pass
    // runs either way, so a change is heard from where the signal is.
    void SetHeadphones(bool on) noexcept { headphones_ = on; }

    void RunTo(std::int64_t cycle) {
        Sink sink { this };
        while (!bus_.Empty() && bus_.Front().cycle < cycle) {
            const BusWrite w = bus_.Pop();
            if (w.cycle > apu_.Now())
                apu_.Run(w.cycle, sink);
            WriteImmediate(w.address, w.value);
        }
        if (apu_.Now() < cycle)
            apu_.Run(cycle, sink);
    }

    // Runs to the block's end, adds the board's sides into left and right,
    // and moves on.
    void Render(int frames, Path path, float* left, float* right) {
        RunTo(tb_.CycleAt(frames));
        float bus[2][StepBuffer::kMaxFrames] {};
        mix_.Read(0, bus[0], frames);
        mix_.Read(1, bus[1], frames);
        // A DAC turn-off is the sole source of this separate RC tail.
        // An empty impulse buffer and zero residual stay exactly at rest.
        if (dacResidual_[0] != 0.0 || dacResidual_[1] != 0.0 ||
            dacOff_[0].HasPendingSteps() || dacOff_[1].HasPendingSteps()) {
            float offDelta[2][StepBuffer::kMaxFrames];
            dacOff_[0].ReadDelta(offDelta[0], frames);
            dacOff_[1].ReadDelta(offDelta[1], frames);
            Filter(frames, path, bus, offDelta, left, right);
        } else {
            Filter(frames, path, bus, nullptr, left, right);
        }
        tb_.Advance(frames);
    }

    [[nodiscard]] const gb::Apu& Chip() const noexcept { return apu_; }
    [[nodiscard]] int Revision() const noexcept { return revision_; }

private:
    void Filter(int frames, Path path, const float (*bus)[StepBuffer::kMaxFrames],
                const float (*offDelta)[StepBuffer::kMaxFrames], float* left, float* right) noexcept {
        float* out[2] = { left, right };
        for (int i = 0; i < frames; ++i) {
            for (int side = 0; side < 2; ++side) {
                double in = bus[side][i];
                if (offDelta != nullptr) {
                    dacResidual_[side] = dacFadeFactor_ * (dacResidual_[side] + static_cast<double>(offDelta[side][i]));
                    if (dacResidual_[side] > -1.0e-15 && dacResidual_[side] < 1.0e-15)
                        dacResidual_[side] = 0.0;
                    in += dacResidual_[side];
                }
                const double pin = pinBlocker_[side].Process(bus[side][i]);
                const double line = amplifier_[side].Process(capacitor_[side].Process(in));
                const double socket = socket_[side].Process(line);
                out[side][i] += static_cast<float>((path == Path::Line ? (headphones_ ? socket : line) : pin) * traits_.level);
            }
        }
    }

    struct Sink {
        Handheld* board;
        void operator()(std::int64_t cycle) const { board->Changed(cycle); }
    };

    void WriteImmediate(std::uint16_t address, std::uint8_t value) noexcept {
        Sink sink { this };
        apu_.Write(address, value, sink);
    }

    void Changed(std::int64_t cycle) noexcept {
        const std::int64_t position = Position(cycle);
        const std::uint8_t nr51 = apu_.Panning();
        const std::uint8_t nr50 = apu_.MasterVolume();
        // NR50: bits 6-4 left, 2-0 right; (volume + 1) / 8.
        const double masterLeft = (((nr50 >> 4) & 7) + 1) / 8.0;
        const double masterRight = ((nr50 & 7) + 1) / 8.0;
        double side[2] = { 0.0, 0.0 };
        double chSide[kChannels][2] {};
        for (int c = 0; c < kChannels; ++c) {
            if (!apu_.DacOn(c))
                continue;
            const double analog = 1.0 - 2.0 * apu_.Out(c) / 15.0;
            if (nr51 & (0x10 << c))
                chSide[c][0] = analog * masterLeft;
            if (nr51 & (0x01 << c))
                chSide[c][1] = analog * masterRight;
            side[0] += chSide[c][0];
            side[1] += chSide[c][1];
        }
        const float levels[2] = { static_cast<float>(side[0]), static_cast<float>(side[1]) };
        mix_.Set(0, position, levels);
        for (int c = 0; c < kChannels; ++c) {
            const bool dac = apu_.DacOn(c);
            if (!dac && wasDacOn_[c]) {
                dacOff_[0].Add(position, QuantizeLevel(static_cast<float>(lastChSide_[c][0])));
                dacOff_[1].Add(position, QuantizeLevel(static_cast<float>(lastChSide_[c][1])));
            }
            wasDacOn_[c] = dac;
            lastChSide_[c][0] = chSide[c][0];
            lastChSide_[c][1] = chSide[c][1];
        }
    }

    int rate_ = 48000;
    int revision_ = kOriginal;
    UnitTraits traits_;
    bool settling_ = false;   // FinishBoot: every step lands at the block's start
    gb::Apu apu_;
    Timebase tb_;
    BusWriteQueue<> bus_;
    StepMix<1, 2> mix_;
    StepBuffer dacOff_[2];
    // A capacitor's factor per clock for a corner f is 1 - 2 pi f / 4194304.
    static constexpr double kColorCapacitor = 1.0 - 2.0 * 3.141592653589793 * 47.0 / 4194304.0;

    ChargeHighPass capacitorDesign_[kRevisions], capacitor_[2];
    FirstOrder::Coefficients colorHighPass_;
    FirstOrder amplifier_[2];   // the Color's feedback high-pass; the original's passes
    FirstOrder pinBlocker_[2];
    static constexpr double kSocketHz = 1.0 / (2.0 * 3.141592653589793 * 100e-6 * 32.0);
    FirstOrder socket_[2];      // the headphone socket's capacitor into its load
    bool headphones_ = false;
    double dacFadeFactor_ = 0.0;
    bool wasDacOn_[kChannels] {};
    double lastChSide_[kChannels][2] {};
    double dacResidual_[2] {};
};

} // namespace protocodus::reboot
