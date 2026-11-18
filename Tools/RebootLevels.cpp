// The level trims (Source/DSP/LevelTrims.h): every part and shape of every
// machine, on both paths, measured through the engine and brought to the
// loudness of the reference square.
//
//   RebootLevels --write <LevelTrimData.h>   measure and write the table
//   RebootLevels --check                     fail when any part is off by more than 0.05 dB
//   RebootLevels --report                    print each part's loudness against the reference
//
// The measure (ours, README How it works): ITU-R BS.1770-4's K-weighted mean
// square, both sides averaged, over a held C3, C4 and C5, each played alone
// on a machine just reset, at full level with the envelope held open, one
// machine, no Double, 48 kHz without oversampling, Volume at -24 dB and
// added back, so the output stage never acts. Each note is heard for a
// second after 0.3 s, so the boards' output stages have settled. The
// reference is a 50 % square at Engine::kReferenceRms through the same
// weighting, worked out from its harmonics. The pre-filter's two sections
// are designed at the rate from the analog prototypes pyloudnorm (MIT
// licence) gives for the standard's 48 kHz coefficients: a shelf of
// +4.0 dB at 1682 Hz and a high-pass at 38.1 Hz (reported). A constant
// error in either cancels, since every part is set against the same
// reference through the same filter.

#include "DSP/LevelTrims.h"
#include "DSP/RebootEngine.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace protocodus::reboot;

namespace {

constexpr int kRate = 48000;
constexpr int kKeys[] = { 48, 60, 72 };
constexpr double kSettle = 0.3, kHeard = 1.0;
constexpr double kPi = 3.141592653589793;
constexpr double kTolerance = 0.05;   // dB, --check
constexpr double kVolumeDb = -24.0;

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    double Run(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    double Power(double f) const {
        // |H(e^jw)|^2 by direct evaluation.
        const double w = 2.0 * kPi * f / kRate;
        const double nr = b0 + b1 * std::cos(w) + b2 * std::cos(2 * w), ni = -b1 * std::sin(w) - b2 * std::sin(2 * w);
        const double dr = 1 + a1 * std::cos(w) + a2 * std::cos(2 * w), di = -a1 * std::sin(w) - a2 * std::sin(2 * w);
        return (nr * nr + ni * ni) / (dr * dr + di * di);
    }
};

Biquad Normalised(double b0, double b1, double b2, double a0, double a1, double a2) {
    Biquad q;
    q.b0 = b0 / a0, q.b1 = b1 / a0, q.b2 = b2 / a0, q.a1 = a1 / a0, q.a2 = a2 / a0;
    return q;
}

struct KWeighting {
    Biquad shelf, highPass;
    KWeighting() {
        {
            const double g = 3.999843853973347, f0 = 1681.974450955533, q = 0.7071752369554196;
            const double a = std::pow(10.0, g / 40.0), w = 2 * kPi * f0 / kRate, c = std::cos(w);
            const double alpha = std::sin(w) / (2 * q), s = 2 * std::sqrt(a) * alpha;
            shelf = Normalised(a * ((a + 1) + (a - 1) * c + s), -2 * a * ((a - 1) + (a + 1) * c),
                a * ((a + 1) + (a - 1) * c - s), (a + 1) - (a - 1) * c + s, 2 * ((a - 1) - (a + 1) * c),
                (a + 1) - (a - 1) * c - s);
        }
        {
            const double f0 = 38.13547087602444, q = 0.5003270373238773;
            const double w = 2 * kPi * f0 / kRate, c = std::cos(w), alpha = std::sin(w) / (2 * q);
            highPass = Normalised((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha);
        }
    }
    double Run(double x) { return highPass.Run(shelf.Run(x)); }
    double Power(double f) const { return shelf.Power(f) * highPass.Power(f); }
};

// The reference square's K-weighted mean square over the three keys: its
// odd harmonics, amplitude 4A / (pi k), below the Nyquist frequency.
double ReferencePower() {
    const KWeighting k;
    const double a = Engine::kReferenceRms;
    double total = 0.0;
    for (int key : kKeys) {
        const double f = 440.0 * std::pow(2.0, (key - 69) / 12.0);
        double p = 0.0;
        for (int h = 1; h * f < kRate / 2.0; h += 2) {
            const double amp = 4.0 * a / (kPi * h);
            p += amp * amp / 2.0 * k.Power(h * f);
        }
        total += p;
    }
    return total / static_cast<double>(std::size(kKeys));
}

struct Variant {
    int board, revision, expansion, channels;
};

// In LevelTrims.h's row order.
std::vector<Variant> Variants() {
    std::vector<Variant> v { { 0, 0, 0, 4 }, { 0, 1, 0, 4 }, { 1, Console::kFrontLoader, 0, 4 }, { 1, Console::kPal, 0, 4 } };
    for (int cart : { Console::kNone, Console::kVrc6, Console::kMmc5, Console::kFds, Console::k5B })
        v.push_back({ 1, Console::kFamicom, cart, 4 });
    for (int n = 1; n <= 8; ++n)
        v.push_back({ 1, Console::kFamicom, Console::kN163, n });
    return v;
}

Controls ControlsFor(const Variant& v, int path, int part, int shape) {
    Controls c;
    c[kBoard] = v.board;
    c[kRevision] = v.revision;
    c[kExpansion] = v.expansion;
    c[kExpansionChannels] = v.channels;
    c[kPath] = path;
    c[kBoards] = 1;
    c[kPart] = part;
    c[kShape] = shape;
    c[kLevel] = 15;
    c[kAttack] = 0;
    c[kDecay] = 0;
    c[kSustain] = 15;
    c[kRelease] = 0;
    c[kVelocity] = 0;
    c[kVibratoDepth] = 0;
    c[kDouble] = 0;
    // Under the output stage's limiter, whatever the trims (Measure adds
    // the 24 dB back).
    c[kVolume] = kVolumeDb;
    return c;
}

// The engine's K-weighted mean square over the keys, as it plays now.
double Measure(Engine& e) {
    const int settle = static_cast<int>(kSettle * kRate), heard = static_cast<int>(kHeard * kRate);
    std::vector<float> l(static_cast<std::size_t>(settle + heard)), r(l.size());
    double total = 0.0;
    for (int key : kKeys) {
        e.Reset();
        e.NoteOn(key, 127);
        for (std::size_t i = 0; i < l.size(); i += 512) {
            const int n = static_cast<int>(std::min<std::size_t>(512, l.size() - i));
            e.Process(l.data() + i, r.data() + i, n);
        }
        KWeighting kl, kr;
        double p = 0.0;
        for (std::size_t i = 0; i < l.size(); ++i) {
            const double yl = kl.Run(l[i]), yr = kr.Run(r[i]);
            if (static_cast<int>(i) >= settle)
                p += 0.5 * (yl * yl + yr * yr);
        }
        total += p / heard;
    }
    return total / static_cast<double>(std::size(kKeys)) * std::pow(10.0, -kVolumeDb / 10.0);
}

double Db(double powerRatio) { return 10.0 * std::log10(powerRatio); }

} // namespace

int main(int argc, char** argv) {
    const bool write = argc == 3 && std::strcmp(argv[1], "--write") == 0;
    const bool check = argc == 2 && std::strcmp(argv[1], "--check") == 0;
    const bool report = argc == 2 && std::strcmp(argv[1], "--report") == 0;
    if (!write && !check && !report) {
        std::fprintf(stderr, "usage: RebootLevels --write <LevelTrimData.h> | --check | --report\n");
        return 2;
    }
    const double reference = ReferencePower();
    auto e = std::make_unique<Engine>();
    e->Prepare(kRate, 1);

    const std::vector<Variant> variants = Variants();
    std::vector<float> table(static_cast<std::size_t>(kTrimRows * Machine::kParts * kTrimShapes), 1.0f);
    double worst = 0.0;
    int measured = 0;
    for (std::size_t vi = 0; vi < variants.size(); ++vi) {
        const Variant& v = variants[vi];
        for (int path = 0; path < 2; ++path)
            for (int part = 0; part < Machine::kParts; ++part) {
                e->SetControls(ControlsFor(v, path, part, 0));
                const Machine& m = e->MachineAt(0);
                if (TrimVariant(m.Settings()) != static_cast<int>(vi)) {
                    std::fprintf(stderr, "variant %zu runs as row %d\n", vi, TrimVariant(m.Settings()));
                    return 1;
                }
                const Part p = m.PartOf(part);
                if (p.count == 0)
                    continue;
                const int shapes = m.Shapes(p.first, p.voice);
                for (int shape = 0; shape < kTrimShapes; ++shape) {
                    const int at = TrimIndex(m.Settings(), static_cast<Path>(path), part, shape);
                    if (shape >= shapes) {
                        table[static_cast<std::size_t>(at)] =
                            table[static_cast<std::size_t>(TrimIndex(m.Settings(), static_cast<Path>(path), part, shape % shapes))];
                        continue;
                    }
                    e->SetControls(ControlsFor(v, path, part, shape));
                    const double power = Measure(*e);
                    if (!(power > 0.0)) {
                        std::fprintf(stderr, "row %zu path %d part %d shape %d is silent\n", vi, path, part, shape);
                        return 1;
                    }
                    const double trim = e->Trim();
                    const double off = Db(power / reference);
                    ++measured;
                    worst = std::fabs(off) > std::fabs(worst) ? off : worst;
                    table[static_cast<std::size_t>(at)] = static_cast<float>(trim * std::sqrt(reference / power));
                    if (report || (check && std::fabs(off) > kTolerance))
                        std::printf("board %d rev %d exp %d ch %d path %d part %d shape %d: %+.2f dB (trim %.6g)\n",
                            v.board, v.revision, v.expansion, v.channels, path, part, shape, off, trim);
                }
            }
    }
    std::printf("%d parts and shapes; furthest from the reference %+.3f dB\n", measured, worst);
    if (check)
        return std::fabs(worst) <= kTolerance ? 0 : 1;
    if (!write)
        return 0;

    std::FILE* f = std::fopen(argv[2], "w");
    if (f == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    std::fprintf(f, "#pragma once\n\n// Generated by Tools/RebootLevels.cpp (`RebootLevels --write`); do not edit.\n"
                    "// Rows as LevelTrims.h orders them, each part's eight shapes on a line.\n\n"
                    "namespace protocodus::reboot {\n\ninline constexpr float kLevelTrims[%zu] = {\n", table.size());
    for (std::size_t i = 0; i < table.size(); i += kTrimShapes) {
        const std::size_t row = i / (Machine::kParts * kTrimShapes);
        if (i % (Machine::kParts * kTrimShapes) == 0)
            std::fprintf(f, "    // row %zu: variant %zu, %s\n", row, row / 2, row % 2 ? "Line" : "Pin");
        std::fprintf(f, "   ");
        for (int s = 0; s < kTrimShapes; ++s)
            std::fprintf(f, " %.6ef,", static_cast<double>(table[i + static_cast<std::size_t>(s)]));
        std::fprintf(f, "\n");
    }
    std::fprintf(f, "};\n\n} // namespace protocodus::reboot\n");
    std::fclose(f);
    return 0;
}
