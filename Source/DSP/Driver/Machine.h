#pragma once

// One machine of the chosen board, behind the one interface the playing
// layer uses. Every board type is built and prepared up front, so choosing
// another board only resets it: nothing is allocated while playing.
//
// A part is what a layer plays: a run of the board's channels and the voice
// it plays on them. Keys a part cannot reach go to its board's fallback part
// (ours, README How it works): the handheld's wave channel below the
// pulses' 64 Hz, the console's triangle below the pulses' 54.6 Hz, the PSG's
// periodic-noise bass below its tones' 109 Hz.

#include "../Boards/Console.h"
#include "../Boards/Handheld.h"
#include "../Boards/Micro.h"
#include "../Boards/Psg.h"

#include <cstdint>

namespace protocodus::reboot {

enum class BoardKind : int { Handheld, Console, Psg, Micro, Count };

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
    static constexpr int kMaxChannels = Console::kMaxChannels;
    static constexpr int kParts = 5;

    void Prepare(int sampleRate) {
        handheld_.Prepare(sampleRate);
        console_.Prepare(sampleRate);
        psg_.Prepare(sampleRate);
        micro_.Prepare(sampleRate);
    }

    void Reset(const MachineSettings& s) {
        kind_ = s.board;
        revision_ = s.revision;
        switch (kind_) {
        case BoardKind::Handheld: handheld_.Reset(Clamp(s.revision, Handheld::kRevisions)); break;
        case BoardKind::Console:
            console_.Reset(Clamp(s.revision, Console::kRevisions), s.expansion, s.expansionChannels, s.bias);
            break;
        case BoardKind::Psg: psg_.Reset(Clamp(s.revision, Psg::kRevisions)); break;
        default: micro_.Reset(Clamp(s.revision, Micro::kRevisions)); break;
        }
    }

    [[nodiscard]] BoardKind Kind() const noexcept { return kind_; }

    // The parts each board offers, in the Part control's order.
    [[nodiscard]] Part PartOf(int index) const noexcept {
        switch (kind_) {
        case BoardKind::Handheld: {
            static constexpr Part kHandheld[] = { { 0, 2, Voice::Pulse }, { 2, 1, Voice::Wave }, { 3, 1, Voice::Noise } };
            return index < 3 ? kHandheld[index] : Part {};
        }
        case BoardKind::Console:
            switch (index) {
            case 0: return { 0, 2, Voice::Pulse };
            case 1: return { 2, 1, Voice::Triangle };
            case 2: return { 3, 1, Voice::Noise };
            case 3: {
                // The cartridge's chip: its pulses, waves or tones.
                const int first = Console::kApuChannels;
                const int count = console_.Channels() - first;
                if (count <= 0)
                    return {};
                if (console_.CartridgeType() == Console::kVrc6)
                    return { first, 2, Voice::Pulse };
                return { first, count, console_.VoiceOf(first) };
            }
            case 4:
                if (console_.CartridgeType() == Console::kVrc6)
                    return { Console::kApuChannels + 2, 1, Voice::Saw };
                return {};
            default: return {};
            }
        case BoardKind::Psg: {
            static constexpr Part kPsg[] = { { 0, 3, Voice::Tone }, { 3, 1, Voice::Noise } };
            return index < 2 ? kPsg[index] : Part {};
        }
        default:
            if (index == 3)
                return { Micro::kBeeper, 1, Voice::Beeper };
            if (index > 3 || !micro_.HasAy())
                return {};
            return { 0, 3, index == 0 ? Voice::Tone : index == 1 ? Voice::Noise : Voice::Buzzer };
        }
    }

    // Where keys go that `part` cannot reach; count 0 when nowhere.
    [[nodiscard]] Part FallbackOf(const Part& part) const noexcept {
        switch (kind_) {
        case BoardKind::Handheld:
            return part.voice == Voice::Pulse ? Part { 2, 1, Voice::Wave } : Part {};
        case BoardKind::Console:
            return part.voice != Voice::Noise && part.voice != Voice::Triangle ? Part { 2, 1, Voice::Triangle } : Part {};
        case BoardKind::Psg:
            return part.voice == Voice::Tone ? Part { 3, 1, Voice::Bass } : Part {};
        default:
            return {};
        }
    }

    // A channel `voice` also takes over while it plays, or -1: the PSG's
    // bass silences tone 3 and uses its period.
    [[nodiscard]] int Claims(int channel, Voice voice) const noexcept {
        return kind_ == BoardKind::Psg && channel == 3 && voice == Voice::Bass ? 2 : -1;
    }

    // How many settings the Shape control steps through for `voice` here.
    [[nodiscard]] int Shapes(int channel, Voice voice) const noexcept {
        switch (voice) {
        case Voice::Pulse:
            return kind_ == BoardKind::Console && console_.EightDuties(channel) ? 8 : 4;
        case Voice::Wave: case Voice::ModWave: return kWaveCount;
        case Voice::Noise: return kind_ == BoardKind::Micro ? 1 : 2;
        case Voice::Buzzer: return 4;
        default: return 1;
        }
    }

    // The frame the machine's own sound driver would run on, in cycles:
    // num / den.
    void FramePeriod(std::int64_t& num, std::int64_t& den) const noexcept {
        den = 1;
        switch (kind_) {
        case BoardKind::Handheld: num = Handheld::kFrameCycles; break;
        case BoardKind::Console:
            // NESdev Wiki, "Cycle reference chart": 29780.5 CPU cycles a
            // frame on NTSC, 33247.5 on PAL.
            num = revision_ == Console::kPal ? 66495 : 59561;
            den = 2;
            break;
        case BoardKind::Psg: num = Psg::kFrameCycles; break;
        default: num = micro_.FrameCycles(); break;
        }
    }

    [[nodiscard]] double ClockHz() const noexcept {
        switch (kind_) {
        case BoardKind::Handheld: return static_cast<double>(gb::kClock);
        case BoardKind::Console: return console_.CpuHz();
        case BoardKind::Psg: return static_cast<double>(Psg::kClock);
        default: return static_cast<double>(micro_.Clock());
        }
    }

    [[nodiscard]] std::int64_t CycleAt(int frame) const noexcept {
        switch (kind_) {
        case BoardKind::Handheld: return handheld_.CycleAt(frame);
        case BoardKind::Console: return console_.CycleAt(frame);
        case BoardKind::Psg: return psg_.CycleAt(frame);
        default: return micro_.CycleAt(frame);
        }
    }

    void RunTo(std::int64_t cycle) {
        switch (kind_) {
        case BoardKind::Handheld: handheld_.RunTo(cycle); break;
        case BoardKind::Console: console_.RunTo(cycle); break;
        case BoardKind::Psg: psg_.RunTo(cycle); break;
        default: micro_.RunTo(cycle); break;
        }
    }

    [[nodiscard]] bool PeriodFor(int channel, Voice voice, double hz, int shape, std::int64_t& period) const noexcept {
        switch (kind_) {
        case BoardKind::Handheld: return handheld_.PeriodFor(channel, voice, hz, shape, period);
        case BoardKind::Console: return console_.PeriodFor(channel, voice, hz, shape, period);
        case BoardKind::Psg: return psg_.PeriodFor(channel, voice, hz, shape, period);
        default: return micro_.PeriodFor(channel, voice, hz, shape, period);
        }
    }

    void Start(int channel, const ChannelSettings& s) {
        switch (kind_) {
        case BoardKind::Handheld: handheld_.Start(channel, s); break;
        case BoardKind::Console: console_.Start(channel, s); break;
        case BoardKind::Psg: psg_.Start(channel, s); break;
        default: micro_.Start(channel, s); break;
        }
    }

    void Change(int channel, const ChannelSettings& s) {
        switch (kind_) {
        case BoardKind::Handheld: handheld_.Change(channel, s); break;
        case BoardKind::Console: console_.Change(channel, s); break;
        case BoardKind::Psg: psg_.Change(channel, s); break;
        default: micro_.Change(channel, s); break;
        }
    }

    void Stop(int channel) {
        switch (kind_) {
        case BoardKind::Handheld: handheld_.Stop(channel); break;
        case BoardKind::Console: console_.Stop(channel); break;
        case BoardKind::Psg: psg_.Stop(channel); break;
        default: micro_.Stop(channel); break;
        }
    }

    void SetBias(int level) {
        if (kind_ == BoardKind::Console)
            console_.SetDmcLevel(level);
    }

    // Adds the machine's sides into left and right.
    void Render(int frames, Path path, float* left, float* right) {
        switch (kind_) {
        case BoardKind::Handheld: handheld_.Render(frames, path, left, right); break;
        case BoardKind::Console: console_.Render(frames, path, left, right); break;
        case BoardKind::Psg: psg_.Render(frames, path, left, right); break;
        default: micro_.Render(frames, path, left, right); break;
        }
    }

    [[nodiscard]] const Handheld& HandheldBoard() const noexcept { return handheld_; }
    [[nodiscard]] const Console& ConsoleBoard() const noexcept { return console_; }
    [[nodiscard]] const Psg& PsgBoard() const noexcept { return psg_; }
    [[nodiscard]] const Micro& MicroBoard() const noexcept { return micro_; }

private:
    static int Clamp(int revision, int count) noexcept { return revision < 0 ? 0 : revision >= count ? count - 1 : revision; }

    BoardKind kind_ = BoardKind::Handheld;
    int revision_ = 0;
    Handheld handheld_;
    Console console_;
    Psg psg_;
    Micro micro_;
};

} // namespace protocodus::reboot
