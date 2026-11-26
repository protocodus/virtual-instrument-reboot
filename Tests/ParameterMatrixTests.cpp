// U4 parameter-extremes matrix and U11 parameter reach for Reboot's engine,
// through the shared portfolio matrix (Tests/ParameterMatrix.h is a
// byte-identical copy of the master; change it in reason-rack-extensions).
// The device here is the engine as a front end drives it: normalised
// controls in RebootControls.h's order, MIDI at its sample.

#include "DSP/RebootEngine.h"
#include "DSP/RebootPresets.h"
#include "ParameterMatrix.h"

#include <cmath>
#include <cstring>
#include <memory>

namespace {

using namespace protocodus::reboot;
using protocodus::matrix::Case;
using protocodus::matrix::Event;
using protocodus::matrix::Limits;
using protocodus::matrix::Parameter;
using protocodus::matrix::Reach;
using protocodus::matrix::ReachRule;

float Normalised(int index, double value) { return static_cast<float>(ToNormalized(index, value)); }

class RebootDevice final : public protocodus::matrix::Device {
public:
    RebootDevice() : engine_(std::make_unique<Engine>()) {
        for (int i = 0; i < kControlCount; ++i)
            values_[static_cast<std::size_t>(i)] = Normalised(i, kControlInfo[static_cast<std::size_t>(i)].initial);
    }

    std::vector<Parameter> Parameters() const override {
        std::vector<Parameter> list;
        for (int i = 0; i < kControlCount; ++i) {
            const ControlInfo& c = kControlInfo[static_cast<std::size_t>(i)];
            list.push_back({ c.id, c.steps, Normalised(i, c.initial) });
        }
        return list;
    }

    int PresetCount() const override { return static_cast<int>(kPresets.size()); }

    void Prepare(double sampleRate, int) override {
        engine_->Prepare(sampleRate);
        Apply();
    }

    void LoadPreset(int index) override {
        const Controls c = kPresets[static_cast<std::size_t>(index)].ToControls();
        for (int i = 0; i < kControlCount; ++i)
            values_[static_cast<std::size_t>(i)] = Normalised(i, c[i]);
        Apply();
    }

    float GetParameter(std::size_t index) const override { return values_[index]; }

    void SetParameter(std::size_t index, float normalised) override {
        values_[index] = normalised;
        Apply();
    }

    void Process(float* left, float* right, int frames, const Event* events, int eventCount) override {
        int done = 0;
        for (int e = 0; e < eventCount; ++e) {
            if (events[e].offset > done) {
                engine_->Process(left + done, right + done, events[e].offset - done);
                done = events[e].offset;
            }
            engine_->Midi(events[e].bytes, events[e].size);
        }
        if (frames > done)
            engine_->Process(left + done, right + done, frames - done);
    }

    std::vector<std::uint8_t> SaveState() override {
        std::vector<std::uint8_t> state(sizeof values_);
        std::memcpy(state.data(), values_.data(), sizeof values_);
        return state;
    }

    void RestoreState(const std::vector<std::uint8_t>& state) override {
        if (state.size() != sizeof values_)
            return;
        std::memcpy(values_.data(), state.data(), sizeof values_);
        Apply();
    }

    Limits LimitsFor(const Case& c) const override {
        Limits limits;
        limits.ceiling = protocodus::OutputStage<2>::kCeiling + 1.0e-6f;
        // The longest release either patch takes, and a margin.
        double release = 0.0;
        for (const auto* values : { &c.values, &c.base }) {
            const double r = ToControls(*values)[kRelease];
            release = r > release ? r : release;
        }
        const double seconds = release / 1000.0 + 0.5;
        limits.releaseSeconds = seconds > 2.0 ? seconds : 2.0;
        // The ramp lands back on the case's values before the held window
        // ends, so those decide whether the held chord must be heard.
        limits.soundsWhileHeld = Sounds(ToControls(c.values));
        return limits;
    }

    ReachRule ReachFor(std::size_t index, float value) const override {
        ReachRule rule;
        const auto with = [&](std::initializer_list<std::pair<int, double>> settings) {
            for (const auto& s : settings)
                rule.with.push_back({ static_cast<std::size_t>(s.first), Normalised(s.first, s.second) });
        };
        switch (static_cast<int>(index)) {
        case kExpansion:
            // A cartridge chip sounds only on the Famicom's slot, through
            // its own part.
            with({ { kBoard, 1 }, { kRevision, 1 }, { kPart, 3 } });
            break;
        case kExpansionChannels:
            // On one machine the count is how many of the chord's notes the
            // chip holds. Between counts that hold them all, the level trims
            // leave the same loudness (LevelTrims.h) and what differs is the
            // multiplex, which each channel joins at its own place: the
            // replay after the reset puts the probes' notes on other
            // channels than T's playing does.
            with({ { kBoard, 1 }, { kRevision, 1 }, { kExpansion, 3 }, { kPart, 3 }, { kBoards, 1 } });
            rule.history = "which of the multiplexed channels each note takes follows the order the playing freed them";
            break;
        case kBias:
            // The DMC's level moves the triangle through the mixer.
            with({ { kBoard, 1 }, { kPart, 1 } });
            break;
        case kPath:
            // The handheld original's 28 Hz corner is all but the Pin
            // blocker's on a chord from G3 up; the console's line stage is
            // not.
            with({ { kBoard, 1 } });
            break;
        case kBoards:
            // One wave channel a machine: 1 and 2 hold fewer of the three-note
            // chord than 6, and between counts from 3 up that hold them all
            // what differs is which units' crystals and warm-ups take the
            // chord, which follows the order the playing freed them since the
            // last reset.
            with({ { kPart, 1 } });
            rule.history = "which units take the chord follows the order the playing freed them since the last reset";
            break;
        case kShape:
            // The pulse has four duties, so 6 plays as the base's 2; the wave
            // part's eight waves tell every value apart.
            if (static_cast<int>(FromNormalized(static_cast<int>(index), value) + 0.5) % 4 == 2)
                with({ { kPart, 1 } });
            break;
        case kAttack:
            rule.reach = Reach::NewNotes;
            rule.reason = "the attack climbs from each note's start, so held notes are at their peak";
            break;
        case kDecay:
            with({ { kSustain, 5 } });
            rule.reach = Reach::NewNotes;
            rule.reason = "the decay falls from each note's peak, which held notes have passed";
            break;
        case kMod:
            with({ { kBoard, 1 }, { kRevision, 1 }, { kExpansion, 4 }, { kPart, 3 } });
            break;
        case kArpRate:
            with({ { kMode, 2 } });
            break;
        case kVibratoRate:
            with({ { kVibratoDepth, 50 } });
            break;
        case kVibratoDelay:
            with({ { kVibratoDepth, 50 } });
            rule.reach = Reach::NewNotes;
            rule.reason = "the delay counts from each note's start, so held notes are past it";
            break;
        case kGlide:
            with({ { kMode, 1 } });
            rule.reach = Reach::NewNotes;
            rule.reason = "a glide is taken when a key moves a Mono note, not while it holds";
            rule.history = "a glide starts wherever its note is";
            break;
        case kBendRange:
            rule.reach = Reach::NewNotes;
            rule.reason = "the range scales the pitch wheel, which rests while the chord is held; the probes bend";
            break;
        case kDouble:
            // Double resets every machine at the change, so the two sets'
            // pulses take their duty steps and frame sequencers from there,
            // where the run held at 1 has played them all along (Pan Docs:
            // a trigger leaves the duty step where it was).
            rule.history = "the sets' pulses keep the duty steps and frame phases since their last reset";
            break;
        case kDetune:
            // The detune parts the two sets, so it is heard only with Double.
            with({ { kDouble, 1 } });
            break;
        case kArpOrder:
            with({ { kMode, 2 } });
            if (static_cast<int>(FromNormalized(static_cast<int>(index), value) + 0.5) == 3) {
                rule.reach = Reach::Exempt;
                rule.reason = "the held chord and the probes strike their keys upward, so the order they were played "
                              "in is the upward one; Reboot.Engine's arp order check strikes four keys out of order";
            }
            break;
        case kArpRetrigger:
            // Heard where the envelope has somewhere to go on each step.
            with({ { kMode, 2 }, { kDecay, 150 }, { kSustain, 3 } });
            break;
        case kDriverRate:
            // Heard in how finely the vibrato's pitch is stepped.
            with({ { kVibratoDepth, 50 } });
            break;
        case kPolyChannels:
            // One machine's two pulses hold two of the chord's notes; the
            // third goes to the wave channel or takes a pulse.
            with({ { kBoards, 1 } });
            rule.reach = Reach::NewNotes;
            rule.reason = "it chooses the channel a note takes when its key is struck";
            break;
        case kEnvelope:
            // Looping is the console's alone: on the handheld 2 plays as 1.
            if (static_cast<int>(FromNormalized(static_cast<int>(index), value) + 0.5) == 2)
                with({ { kBoard, 1 } });
            break;
        case kSweep:
            rule.reach = Reach::NewNotes;
            rule.reason = "a sweep runs from the trigger it is written before (NR10, $4001), and the driver leaves a "
                          "sweeping pulse's period to it";
            break;
        case kSweepPace:
            with({ { kSweep, -2 } });
            rule.reach = Reach::NewNotes;
            rule.reason = "a sweep runs from the trigger it is written before (NR10, $4001)";
            break;
        case kSync:
            // Heard in the vibrato's rate: 5.5 Hz, or a 1/8T at 120 BPM.
            with({ { kVibratoDepth, 50 } });
            break;
        case kArpSync:
            with({ { kSync, 1 }, { kMode, 2 } });
            break;
        case kVibratoSync:
            with({ { kSync, 1 }, { kVibratoDepth, 50 } });
            break;
        case kGlideSync:
            with({ { kSync, 1 }, { kMode, 1 } });
            rule.reach = Reach::NewNotes;
            rule.reason = "a glide is taken when a key moves a Mono note, not while it holds";
            rule.history = "a glide starts wherever its note is";
            break;
        default:
            break;
        }
        return rule;
    }

private:
    Controls ToControls(const std::vector<float>& values) const {
        Controls c;
        for (int i = 0; i < kControlCount; ++i)
            c[i] = FromNormalized(i, values[static_cast<std::size_t>(i)]);
        return c;
    }

    // Whether the matrix's held chord must be heard on a patch: a part its
    // board has, loud enough to stay above -60 dBFS on the quietest DAC
    // (the 5B's, 3 dB a step), at full from the start.
    static bool Sounds(const Controls& c) {
        if (c[kVolume] < -12.0)
            return false;
        const int board = static_cast<int>(c[kBoard] + 0.5);
        const int revision = static_cast<int>(c[kRevision] + 0.5);
        const int expansion = static_cast<int>(c[kExpansion] + 0.5);
        const int part = static_cast<int>(c[kPart] + 0.5);
        const bool exists = board == 0
            ? part < 3
            : part < 3 || (revision == 1 && ((part == 3 && expansion != 0) || (part == 4 && expansion == 1)));
        // A sweep on the console's pulses runs their period out of range,
        // under 8 or past $7FF, where the APU mutes them (APU Sweep), as
        // the chip does; the handheld's sweeps only its first pulse.
        const bool swept = board == 1 && part == 0 && static_cast<int>(std::lround(c[kSweep])) != 0;
        return exists && !swept && c[kAttack] < 10.0 && c[kLevel] >= 12.0 && c[kSustain] >= 10.0;
    }

    void Apply() {
        Controls c;
        for (int i = 0; i < kControlCount; ++i)
            c[i] = FromNormalized(i, values_[static_cast<std::size_t>(i)]);
        engine_->SetControls(c);
    }

    std::unique_ptr<Engine> engine_;
    std::array<float, kControlCount> values_ {};
};

} // namespace

int main(int argc, char** argv) {
    return protocodus::matrix::Main(argc, argv, [] { return std::make_unique<RebootDevice>(); });
}
