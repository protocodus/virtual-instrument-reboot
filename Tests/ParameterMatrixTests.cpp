// U4 parameter-extremes matrix and U11 parameter reach for Reboot's engine,
// through the shared portfolio matrix (Tests/ParameterMatrix.h is a
// byte-identical copy of the master; change it in reason-rack-extensions).
// The device here is the engine as a front end drives it: normalised
// controls in RebootControls.h's order, MIDI at its sample.

#include "DSP/RebootEngine.h"
#include "DSP/RebootPresets.h"
#include "ParameterMatrix.h"

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
        // The longest release either patch takes, 15 steps at the slowest
        // frame (the PAL console's 50 Hz), and a margin.
        int release = 0;
        for (const auto* values : { &c.values, &c.base }) {
            const Controls controls = ToControls(*values);
            for (int l = 0; l < kLayers; ++l) {
                const int r = static_cast<int>(controls[kLayerBase[l] + kRelease] + 0.5);
                release = r > release ? r : release;
            }
        }
        const double seconds = release * 15.0 / 50.0 + 0.5;
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
        const int control = static_cast<int>(index);
        const bool layered = control >= kAOn && control <= kBMod;
        const bool machined = control <= kBoards || control >= kBBoard;
        const int layer = (layered && control >= kBOn) || control >= kBBoard ? 1 : 0;
        const int base = kLayerBase[layer], m = kMachineBase[layer];
        // Layer B is heard alone: on its own machines at A's pitch, it would
        // sum with A by the phases the playing left between them.
        if (layer == 1 && control != kBOn)
            with({ { kBOn, 1 }, { kAOn, 0 } });
        if (machined) {
            switch (control - m) {
            case kMExpansion:
                // A cartridge chip sounds only on the Famicom's slot, through
                // its own part.
                with({ { m + kMBoard, 1 }, { m + kMRevision, 1 }, { base + kPart, 3 } });
                break;
            case kMExpansionChannels:
                with({ { m + kMBoard, 1 }, { m + kMRevision, 1 }, { m + kMExpansion, 3 }, { base + kPart, 3 } });
                break;
            case kMBias:
                // The DMC's level moves the triangle through the mixer.
                with({ { m + kMBoard, 1 }, { base + kPart, 1 } });
                break;
            case kMPath:
                // The handheld original's 28 Hz corner is all but the Pin
                // blocker's on a chord from G3 up; the console's line stage
                // is not.
                with({ { m + kMBoard, 1 } });
                break;
            case kMBoards:
                // One wave channel a machine, so every count changes how
                // many of a three-note chord sound.
                with({ { base + kPart, 1 } });
                break;
            default:
                break;
            }
        }
        switch (control) {
        case kBOn:
            // B doubles A at the same pitch on machines of its own, which
            // start when B comes on, and a trigger leaves a pulse's duty step
            // where it was (Pan Docs), so the doubled level rests on the
            // phases between the two sets of machines.
            rule.history = "the two layers' machines keep the duty phases they started with";
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
        default:
            break;
        }
        if (layered) {
            switch (control - base) {
            case kAttack:
                rule.reach = Reach::NewNotes;
                rule.reason = "the attack climbs from each note's start, so held notes are at their peak";
                break;
            case kDecay:
                with({ { base + kSustain, 5 } });
                rule.reach = Reach::NewNotes;
                rule.reason = "the decay falls from each note's peak, which held notes have passed";
                break;
            case kMod:
                with({ { m + kMBoard, 1 }, { m + kMRevision, 1 }, { m + kMExpansion, 4 }, { base + kPart, 3 } });
                break;
            case kShape:
                // The pulse has four duties, so 6 plays as the base's 2; the
                // wave part's eight waves tell every value apart.
                if (static_cast<int>(FromNormalized(control, value) + 0.5) % 4 == 2)
                    with({ { base + kPart, 1 } });
                break;
            default:
                break;
            }
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

    // Whether the matrix's held chord must be heard on a patch: some layer
    // on a part its board has, loud enough to stay above -60 dBFS on the
    // quietest DAC (the AY's, 3 dB a step), starting at full.
    static bool Sounds(const Controls& c) {
        if (c[kVolume] < -12.0)
            return false;
        const int board = static_cast<int>(c[kBoard] + 0.5);
        const int revision = static_cast<int>(c[kRevision] + 0.5);
        const int expansion = static_cast<int>(c[kExpansion] + 0.5);
        for (int l = 0; l < kLayers; ++l) {
            const int base = kLayerBase[l];
            const int part = static_cast<int>(c[base + kPart] + 0.5);
            bool exists;
            switch (board) {
            case 0: exists = part < 3; break;
            case 1:
                exists = part < 3 || (revision == 1 && ((part == 3 && expansion != 0) || (part == 4 && expansion == 1)));
                break;
            case 2: exists = part < 2; break;
            default: exists = revision == 0 ? part < 4 : part == 3; break;
            }
            if (c[base + kOn] >= 0.5 && exists && c[base + kAttack] < 0.5 && c[base + kLevel] >= 12.0
                && c[base + kSustain] >= 10.0)
                return true;
        }
        return false;
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
