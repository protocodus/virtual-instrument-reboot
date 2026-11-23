#pragma once

// One unit of the chosen board together with its sound driver, behind the
// one interface the playing layer uses. Every board type is built and prepared
// up front, so choosing another board only resets it: nothing is allocated
// while playing. A machine is always the same unit of each board: the one
// with the serial number it was prepared with (Board.h, DrawTraits).
//
// A part is what the voice plays: a run of the board's channels and the voice
// it plays on them. Keys a part cannot reach go to its board's fallback part
// (ours, README How it works): the handheld's CH3 below the pulses' 64 Hz
// (loaded with the chosen Pulse duty wave), the console's triangle below the
// pulses' 54.6 Hz.

#include "../Boards/Console.h"
#include "../Boards/Handheld.h"
#include "ConsoleDriver.h"
#include "DriverTypes.h"
#include "HandheldDriver.h"

#include <climits>
#include <cstdint>

namespace protocodus::reboot {

enum class BoardKind : int { Handheld, Console, Count };

struct MachineSettings {
    BoardKind board = BoardKind::Handheld;
    int revision = 0;
    int expansion = Console::kNone;
    int expansionChannels = 4;
    int bias = 0;
};

struct Part {
    int first = 0;
    int count = 0;     // 0: the board has no such part
    Voice voice = Voice::Pulse;
};

class Machine {
public:
    static constexpr int kMaxChannels = ConsoleDriver::kMaxChannels;
    static constexpr int kParts = 5;

    void Prepare(int sampleRate, int serial = 0) { Prepare(sampleRate, Console::Make5BCurve(), serial); }

    void Prepare(int sampleRate, const Console::Curve5B& levels, int serial = 0) {
        handheld_.Prepare(sampleRate, TraitsOf(BoardKind::Handheld, serial));
        console_.Prepare(sampleRate, levels, TraitsOf(BoardKind::Console, serial));
    }

    // The unit with `serial` of a board: 0 the nominal one (Board.h).
    [[nodiscard]] static UnitTraits TraitsOf(BoardKind kind, int serial) noexcept {
        return DrawTraits(kind == BoardKind::Handheld ? Handheld::kSpread : Console::kSpread,
                          static_cast<std::uint32_t>(serial));
    }

    // Switches the unit on and runs its driver's start; it stands for its
    // warm-up before the playing starts, its crystal `trim` (Timebase's
    // units) off its own.
    void Reset(const MachineSettings& s, std::int64_t trim = 0) {
        kind_ = s.board;
        settings_ = s;
        settings_.expansionChannels = s.expansionChannels < 1 ? 1 : s.expansionChannels > 8 ? 8 : s.expansionChannels;
        switch (kind_) {
        case BoardKind::Handheld:
            settings_.revision = Clamp(s.revision, Handheld::kRevisions);
            handheld_.Reset(settings_.revision, trim);
            handheldDriver_.Boot(handheld_);
            settings_.expansion = Console::kNone;
            break;
        default:
            settings_.revision = Clamp(s.revision, Console::kRevisions);
            console_.Reset(settings_.revision, s.expansion, trim);
            consoleDriver_.Boot(console_, settings_.expansionChannels, s.bias);
            settings_.expansion = console_.CartridgeType();
            break;
        }
    }

    [[nodiscard]] BoardKind Kind() const noexcept { return kind_; }
    // What it was reset with, as it runs: the revision within the board's,
    // the expansion none without a cartridge slot.
    [[nodiscard]] const MachineSettings& Settings() const noexcept { return settings_; }

    // The parts each board offers, in the Part control's order.
    [[nodiscard]] Part PartOf(int index) const noexcept {
        switch (kind_) {
        case BoardKind::Handheld: {
            static constexpr Part kHandheld[] = { { 0, 2, Voice::Pulse }, { 2, 1, Voice::Wave }, { 3, 1, Voice::Noise } };
            return index < 3 ? kHandheld[index] : Part {};
        }
        default:
            switch (index) {
            case 0: return { 0, 2, Voice::Pulse };
            case 1: return { 2, 1, Voice::Triangle };
            case 2: return { 3, 1, Voice::Noise };
            case 3: {
                // The cartridge's chip: its pulses, waves or tones.
                const int first = ConsoleDriver::kApuChannels;
                const int count = consoleDriver_.Channels(console_) - first;
                if (count <= 0)
                    return {};
                if (console_.CartridgeType() == Console::kVrc6)
                    return { first, 2, Voice::Pulse };
                return { first, count, ConsoleDriver::VoiceOf(console_, first) };
            }
            case 4:
                if (console_.CartridgeType() == Console::kVrc6)
                    return { ConsoleDriver::kApuChannels + 2, 1, Voice::Saw };
                return {};
            default: return {};
            }
        }
    }

    // Where keys go that `part` cannot reach; count 0 when nowhere.
    // Handheld pulses fall back onto CH3 carrying the Pulse duty waveform.
    [[nodiscard]] Part FallbackOf(const Part& part) const noexcept {
        switch (kind_) {
        case BoardKind::Handheld:
            return part.voice == Voice::Pulse ? Part { 2, 1, Voice::Pulse } : Part {};
        default:
            return part.voice != Voice::Noise && part.voice != Voice::Triangle ? Part { 2, 1, Voice::Triangle } : Part {};
        }
    }

    // How many settings the Shape control steps through for `voice` here.
    [[nodiscard]] int Shapes(int channel, Voice voice) const noexcept {
        switch (voice) {
        case Voice::Pulse:
            return kind_ == BoardKind::Console && ConsoleDriver::EightDuties(console_, channel) ? 8 : 4;
        case Voice::Wave: case Voice::ModWave: return kWaveCount;
        case Voice::Noise: return 2;
        default: return 1;
        }
    }

    // The frame the machine's own sound driver would run on, in cycles:
    // num / den.
    void FramePeriod(std::int64_t& num, std::int64_t& den) const noexcept {
        den = 1;
        switch (kind_) {
        case BoardKind::Handheld: num = Handheld::kFrameCycles; break;
        default:
            // NESdev Wiki, "Cycle reference chart": 29780.5 CPU cycles a
            // frame on NTSC, 33247.5 on PAL.
            num = settings_.revision == Console::kPal ? 66495 : 59561;
            den = 2;
            break;
        }
    }

    [[nodiscard]] double ClockHz() const noexcept {
        switch (kind_) {
        case BoardKind::Handheld: return static_cast<double>(gb::kClock);
        default: return console_.CpuHz();
        }
    }

    [[nodiscard]] std::int64_t CycleAt(int frame) const noexcept {
        switch (kind_) {
        case BoardKind::Handheld: return handheld_.CycleAt(frame);
        default: return console_.CycleAt(frame);
        }
    }

    [[nodiscard]] std::int64_t Now() const noexcept {
        return kind_ == BoardKind::Handheld ? handheld_.Now() : console_.Now();
    }

    // Where a cycle of this block falls in it (in the step buffer's
    // positions, under the block's length), and the first cycle at or after
    // such a position: how one unit's moment is found on another's clock.
    [[nodiscard]] std::int64_t Position(std::int64_t cycle) const noexcept {
        return kind_ == BoardKind::Handheld ? handheld_.Position(cycle) : console_.Position(cycle);
    }
    [[nodiscard]] std::int64_t CycleAtPosition(std::int64_t position) const noexcept {
        return kind_ == BoardKind::Handheld ? handheld_.CycleAtPosition(position) : console_.CycleAtPosition(position);
    }

    // A trimmer on the unit's crystal, turned at the block's start.
    void SetTrim(std::int64_t trim) noexcept {
        if (kind_ == BoardKind::Handheld)
            handheld_.SetTrim(trim);
        else
            console_.SetTrim(trim);
    }

    [[nodiscard]] std::int64_t NextEvent() const noexcept {
        return kind_ == BoardKind::Handheld ? handheldDriver_.NextEvent() : INT64_MAX;
    }

    void RunTo(std::int64_t cycle) {
        switch (kind_) {
        case BoardKind::Handheld: handheldDriver_.RunTo(handheld_, cycle); break;
        default: console_.RunTo(cycle); break;
        }
    }

    void ServiceAt(std::int64_t cycle) noexcept {
        if (kind_ == BoardKind::Handheld)
            handheldDriver_.ServiceAt(handheld_, cycle);
    }

    [[nodiscard]] int QuantizeVolume(int channel, Voice voice, double gain) const noexcept {
        switch (kind_) {
        case BoardKind::Handheld: return HandheldDriver::QuantizeVolume(channel, voice, gain);
        default: return ConsoleDriver::QuantizeVolume(channel, voice, gain);
        }
    }

    [[nodiscard]] bool PeriodFor(int channel, Voice voice, double hz, int shape, std::int64_t& period) const noexcept {
        switch (kind_) {
        case BoardKind::Handheld: return handheldDriver_.PeriodFor(channel, voice, hz, shape, period);
        default: return consoleDriver_.PeriodFor(console_, channel, voice, hz, shape, period);
        }
    }

    // The channel's own envelope unit, if it has one (DriverTypes.h).
    [[nodiscard]] EnvelopeUnit EnvelopeOf(int channel) const noexcept {
        return kind_ == BoardKind::Handheld ? HandheldDriver::EnvelopeOf(channel) : ConsoleDriver::EnvelopeOf(console_, channel);
    }

    [[nodiscard]] bool HasSweep(int channel) const noexcept {
        return kind_ == BoardKind::Handheld ? HandheldDriver::HasSweep(channel) : ConsoleDriver::HasSweep(channel);
    }

    void Start(int channel, const ChannelSettings& s) {
        switch (kind_) {
        case BoardKind::Handheld: handheldDriver_.Start(handheld_, channel, s); break;
        default: consoleDriver_.Start(console_, channel, s); break;
        }
    }

    void Change(int channel, const ChannelSettings& s) {
        switch (kind_) {
        case BoardKind::Handheld: handheldDriver_.Change(handheld_, channel, s); break;
        default: consoleDriver_.Change(console_, channel, s); break;
        }
    }

    void Stop(int channel) {
        switch (kind_) {
        case BoardKind::Handheld: handheldDriver_.Stop(handheld_, channel); break;
        default: consoleDriver_.Stop(console_, channel); break;
        }
    }

    // What the handheld's socket drives (Handheld::SetHeadphones); the
    // console's outputs have no socket of the kind.
    void SetHeadphones(bool on) noexcept { handheld_.SetHeadphones(on); }

    void SetBias(int level) {
        if (kind_ == BoardKind::Console)
            consoleDriver_.SetBias(console_, level);
    }

    // Adds the machine's sides into left and right.
    void Render(int frames, Path path, float* left, float* right) {
        switch (kind_) {
        case BoardKind::Handheld:
            handheldDriver_.RunTo(handheld_, handheld_.CycleAt(frames));
            handheld_.Render(frames, path, left, right);
            break;
        default:
            console_.Render(frames, path, left, right);
            break;
        }
    }

    [[nodiscard]] const Handheld& HandheldBoard() const noexcept { return handheld_; }
    [[nodiscard]] const Console& ConsoleBoard() const noexcept { return console_; }

private:
    static int Clamp(int revision, int count) noexcept { return revision < 0 ? 0 : revision >= count ? count - 1 : revision; }

    BoardKind kind_ = BoardKind::Handheld;
    MachineSettings settings_;
    Handheld handheld_;
    HandheldDriver handheldDriver_;
    Console console_;
    ConsoleDriver consoleDriver_;
};

} // namespace protocodus::reboot
