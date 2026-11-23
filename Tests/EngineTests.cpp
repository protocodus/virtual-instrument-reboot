// The playing layer and the engine around the machines: notes, voices,
// modes, the pedal, levels, rates and the bounds a host may push.

#include "DSP/RebootEngine.h"
#include "DSP/RebootPresets.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace protocodus::reboot;

int failures = 0;

void Check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// The stage's float gain times a sample can land a few ulps over its
// ceiling; the parameter matrix allows the same 1e-6.
constexpr float kCeiling = protocodus::OutputStage<2>::kCeiling + 1.0e-6f;

struct Event {
    int at;                       // sample
    std::vector<std::uint8_t> midi;
};

Event On(int at, int key, int velocity = 100) {
    return { at, { 0x90, static_cast<std::uint8_t>(key), static_cast<std::uint8_t>(velocity) } };
}
Event Off(int at, int key) { return { at, { 0x80, static_cast<std::uint8_t>(key), 0 } }; }
Event Cc(int at, int cc, int value) {
    return { at, { 0xB0, static_cast<std::uint8_t>(cc), static_cast<std::uint8_t>(value) } };
}
Event Bend(int at, double amount) {
    const int v = std::clamp(static_cast<int>(std::lround(8192 + amount * 8191)), 0, 16383);
    return { at, { 0xE0, static_cast<std::uint8_t>(v & 0x7F), static_cast<std::uint8_t>(v >> 7) } };
}

struct Audio {
    std::vector<float> left, right;
};

// Renders `frames` in blocks of at most `block`, each event at its sample.
Audio Play(Engine& e, const std::vector<Event>& events, int frames, int block = 256) {
    Audio a;
    a.left.assign(static_cast<std::size_t>(frames), 0.0f);
    a.right.assign(static_cast<std::size_t>(frames), 0.0f);
    std::size_t next = 0;
    for (int done = 0; done < frames;) {
        while (next < events.size() && events[next].at <= done) {
            e.Midi(events[next].midi.data(), static_cast<int>(events[next].midi.size()));
            ++next;
        }
        int n = std::min(block, frames - done);
        if (next < events.size())
            n = std::min(n, events[next].at - done);
        e.Process(a.left.data() + done, a.right.data() + done, n);
        done += n;
    }
    return a;
}

std::unique_ptr<Engine> Fresh(const Controls& c, double rate = 48000.0, int oversampling = 1) {
    auto e = std::make_unique<Engine>();
    e->Prepare(rate, oversampling);
    e->SetControls(c);
    return e;
}

double Rms(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i)
        sum += static_cast<double>(x[i]) * x[i];
    return std::sqrt(sum / static_cast<double>(to - from));
}

double Peak(const std::vector<float>& x, std::size_t from = 0) {
    double p = 0.0;
    for (std::size_t i = from; i < x.size(); ++i)
        p = std::max(p, static_cast<double>(std::fabs(x[i])));
    return p;
}

bool Finite(const Audio& a) {
    for (std::size_t i = 0; i < a.left.size(); ++i)
        if (!std::isfinite(a.left[i]) || !std::isfinite(a.right[i]))
            return false;
    return true;
}

// No tone after `from`: on Pin a stopped Game Boy channel stays at its DAC's
// +1 (Pan Docs), and the 5 Hz blocker takes that step out over a fifth of a
// second, so silence there is one sign, never growing.
bool Settles(const std::vector<float>& x, std::size_t from) {
    for (std::size_t i = from + 1; i < x.size(); ++i)
        if ((x[i] > 0.0f) != (x[from] > 0.0f) || std::fabs(x[i]) > std::fabs(x[i - 1]) + 1.0e-7f)
            return false;
    return true;
}

double Db(double v) { return 20.0 * std::log10(v); }

// The fundamental between two samples (two band-passes at `around`, then
// interpolated upward zero crossings), as Tests/BoardTests.cpp measures.
double Measure(const std::vector<float>& x, double around, double rate, std::size_t from, std::size_t to) {
    const double w = 2.0 * 3.14159265358979323846 * around / rate;
    const double alpha = std::sin(w) / 6.0;
    const double a0 = 1.0 + alpha;
    const double b0 = alpha / a0, a1 = -2.0 * std::cos(w) / a0, a2 = (1.0 - alpha) / a0;
    double s[2][4] {};
    double previous = 0.0, first = -1.0, last = -1.0;
    int crossings = 0;
    for (std::size_t i = 0; i < to; ++i) {
        double y = x[i];
        for (auto& f : s) {
            const double out = b0 * y - b0 * f[1] - a1 * f[2] - a2 * f[3];
            f[1] = f[0], f[0] = y, f[3] = f[2], f[2] = out;
            y = out;
        }
        if (i > from && previous < 0.0 && y >= 0.0) {
            const double t = static_cast<double>(i) - y / (y - previous);
            if (first < 0.0)
                first = t;
            last = t;
            ++crossings;
        }
        previous = y;
    }
    return crossings < 10 ? 0.0 : (crossings - 1) * rate / (last - first);
}

double Cents(double a, double b) { return 1200.0 * std::log2(a / b); }

// The Init patch on the pin path at full level, unsensitive, its envelope
// held open until the key's 250 ms release.
Controls Plain(int board, int part = 0, int shape = 2) {
    Controls c;
    c[kBoard] = board;
    c[kPath] = 0;
    c[kPart] = part;
    c[kShape] = shape;
    c[kLevel] = 15;
    c[kDecay] = 0;
    c[kSustain] = 15;
    c[kVelocity] = 0;
    return c;
}

// ---------------------------------------------------------------------------

// A phrase that moves everything that can move: chords, a held pedal, the
// wheel's vibrato, a bend, control changes between events.
std::vector<Event> Phrase() {
    std::vector<Event> ev;
    int t = 100;
    for (int key : { 60, 64, 67, 72 })
        ev.push_back(On(t += 37, key));
    ev.push_back(Cc(t + 2000, 1, 90));
    ev.push_back(Bend(t + 9000, 0.6));
    ev.push_back(Cc(t + 12000, 64, 127));
    ev.push_back(Off(t + 13000, 64));
    ev.push_back(Off(t + 13001, 67));
    ev.push_back(On(t + 20000, 48, 127));
    ev.push_back(Bend(t + 25000, -1.0));
    ev.push_back(Cc(t + 30000, 64, 0));
    for (int key : { 60, 72, 48 })
        ev.push_back(Off(t + 40000, key));
    ev.push_back(On(t + 50000, 84, 30));
    ev.push_back(Off(t + 60000, 84));
    std::sort(ev.begin(), ev.end(), [](const Event& a, const Event& b) { return a.at < b.at; });
    return ev;
}

void TestPartitionAndDeterminism() {
    for (std::size_t p = 0; p < kPresets.size(); ++p) {
        const Controls c = kPresets[p].ToControls();
        const auto reference = Fresh(c);
        const Audio a = Play(*reference, Phrase(), 96000, 256);
        Check(Finite(a), std::string(kPresets[p].name) + ": not finite");
        for (int block : { 1, 64, 512, 1000 }) {
            const auto e = Fresh(c);
            const Audio b = Play(*e, Phrase(), 96000, block);
            Check(a.left == b.left && a.right == b.right,
                  std::string(kPresets[p].name) + ": blocks of " + std::to_string(block) + " change the output");
        }
    }
}

// Shape and tap changes can both select a new level trim. The second change
// interrupts the first ramp, so this also checks its starting gain.
void TestTrimAutomationPartition() {
    for (int factor : { 1, 2, 4, 8 }) {
        const auto render = [factor](int block) {
            Controls c = Plain(0, 0, 2);
            auto e = Fresh(c, 48000.0, factor);
            e->NoteOn(60, 100);
            Play(*e, {}, 4096, block);
            c[kShape] = 0;
            e->SetControls(c);
            Audio first = Play(*e, {}, 19, block);
            c[kPath] = 1;
            e->SetControls(c);
            Audio rest = Play(*e, {}, 512, block);
            first.left.insert(first.left.end(), rest.left.begin(), rest.left.end());
            first.right.insert(first.right.end(), rest.right.begin(), rest.right.end());
            return first;
        };
        const Audio reference = render(64);
        for (int block : { 1, 16, 256 }) {
            const Audio actual = render(block);
            Check(actual.left == reference.left && actual.right == reference.right,
                  "automated trim changes with block " + std::to_string(block) +
                  " at oversampling " + std::to_string(factor));
        }
    }
}

void TestPresets() {
    for (std::size_t p = 0; p < kPresets.size(); ++p) {
        const Controls c = kPresets[p].ToControls();
        const auto e = Fresh(c);
        const std::string name = kPresets[p].name;
        // A chord from C3 up, held a second, then two seconds to die away.
        std::vector<Event> ev;
        for (int key : { 48, 55, 60, 64 })
            ev.push_back(On(0, key, 110));
        for (int key : { 48, 55, 60, 64 })
            ev.push_back(Off(48000, key));
        const Audio a = Play(*e, ev, 3 * 48000);
        Check(Finite(a), name + ": not finite");
        Check(Peak(a.left) <= kCeiling && Peak(a.right) <= kCeiling, name + ": over the ceiling");
        Check(Rms(a.left, 2400, 48000) > 0.003, name + ": silent while held (" + std::to_string(Rms(a.left, 2400, 48000)) + ")");
        Check(Peak(a.left, 2 * 48000 + 24000) < 1.0e-4 && e->Sounding() == 0,
              name + ": still sounding two seconds after release");
    }
}

// One channel at full volume, a 50 % square, reads -24 dBFS RMS on both
// boards, which the other parts are trimmed to (LevelTrims.h; every part
// and shape is Reboot.Levels).
void TestReferenceLevel() {
    struct Case {
        const char* name;
        int board;
        int part, shape;
    };
    for (const Case& k : { Case { "handheld", 0, 0, 2 }, Case { "console", 1, 0, 2 } }) {
        Controls c = Plain(k.board, k.part, k.shape);
        c[kBoards] = 1;
        const auto e = Fresh(c);
        const Audio a = Play(*e, { On(0, 69, 127) }, 48000);
        const double level = Db(Rms(a.left, 12000, 48000));
        Check(std::fabs(level + 24.0) < 0.3, std::string(k.name) + ": the reference reads " + std::to_string(level) + " dBFS");
    }
}

void TestRates() {
    // Init's handheld pulse at A4: x = 2048 - 298, 131072 / 298 Hz.
    const double want = 131072.0 / 298.0;
    for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 }) {
        Controls c = Plain(0);
        const auto e = Fresh(c, rate);
        const int frames = static_cast<int>(rate);
        const Audio a = Play(*e, { On(0, 69) }, frames);
        const double got = Measure(a.left, want, rate, static_cast<std::size_t>(frames / 5), static_cast<std::size_t>(frames));
        Check(std::fabs(Cents(got, want)) < 0.3, "at " + std::to_string(rate) + " Hz A4 plays " + std::to_string(got));
        Check(std::fabs(Db(Rms(a.left, static_cast<std::size_t>(frames / 5), static_cast<std::size_t>(frames))) + 24.0) < 0.3,
              "the level moves with the rate at " + std::to_string(rate));
        // The note's first step, the DAC coming on at full, is half way up
        // at the latency the engine reports.
        std::size_t first = 0;
        while (std::fabs(a.left[first]) <= 0.5 * 0.0631)
            ++first;
        const auto latency = static_cast<std::size_t>(e->LatencySamples());
        Check(first == latency || first == latency + 1,
              "at " + std::to_string(rate) + " Hz the note starts at " + std::to_string(first) + ", not the latency " + std::to_string(latency));
    }
    // A rate no host runs at plays as 48 kHz.
    for (double rate : { 0.0, -1.0, std::nan(""), 1.0e300 }) {
        const auto e = Fresh(Plain(0), rate);
        const Audio a = Play(*e, { On(0, 69) }, 48000);
        Check(Finite(a) && std::fabs(Cents(Measure(a.left, want, 48000.0, 9600, 48000), want)) < 0.3,
              "an invalid rate does not fall back to 48 kHz");
    }
}

// Owner requirement: every key from A0 to C8 sounds at its own pitch on each
// board's default patch, never folded into another octave. The chips'
// period steps leave the top keys up to about 32 cents out (the AY's C8).
void TestKeyboard() {
    for (int board = 0; board < 2; ++board) {
        for (int key = 21; key <= 108; ++key) {
            Controls c;
            c[kBoard] = board;
            const Audio a = Play(*Fresh(c), { On(0, key) }, 24000);
            const double want = 440.0 * std::pow(2.0, (key - 69) / 12.0);
            const double got = Measure(a.left, want, 48000.0, 6000, 24000);
            Check(got > 0.0 && std::fabs(Cents(got, want)) < 40.0,
                  "board " + std::to_string(board) + " key " + std::to_string(key) + " plays " + std::to_string(got) + " Hz");
        }
    }
}

// The engine's latency at 48 kHz and the kernel's 16 samples of ringing.
constexpr std::size_t kLate = 128;

// Oversampling renders the machines at 2, 4 or 8 times the rate and brings
// them back through the halfband stages: the pitch and level of 1x, the note
// starting at the latency reported, the same bits however the host cuts its
// blocks, and the factor halved until the machines run at or under 384 kHz.
void TestOversampling() {
    const double want = 131072.0 / 298.0;
    for (double rate : { 44100.0, 48000.0, 96000.0 }) {
        const int frames = static_cast<int>(rate);
        const auto from = static_cast<std::size_t>(frames / 5), to = static_cast<std::size_t>(frames);
        const Audio one = Play(*Fresh(Plain(0), rate), { On(0, 69) }, frames);
        for (int factor : { 2, 4, 8 }) {
            const std::string at = std::to_string(factor) + "x at " + std::to_string(rate) + " Hz";
            const auto e = Fresh(Plain(0), rate, factor);
            int expect = factor;
            while (expect > 1 && rate * expect > Engine::kMaxInternalRate)
                expect /= 2;
            Check(e->Oversampling() == expect, at + " runs at " + std::to_string(e->Oversampling()) + "x");
            const Audio a = Play(*e, { On(0, 69) }, frames);
            Check(a.left != one.left, at + " renders the same samples as 1x");
            Check(std::fabs(Cents(Measure(a.left, want, rate, from, to), want)) < 0.3, at + ": A4 is out of tune");
            Check(std::fabs(Db(Rms(a.left, from, to)) - Db(Rms(one.left, from, to))) < 0.05, at + ": the level moves");
            std::size_t first = 0;
            while (std::fabs(a.left[first]) <= 0.5 * 0.0631)
                ++first;
            const auto latency = static_cast<std::size_t>(e->LatencySamples());
            // The decimator's delay is whole host samples, so the onset is
            // exactly where it is at 1x, moved by the reported latency.
            Check(first == latency,
                  at + ": the note starts at " + std::to_string(first) + ", not the latency " + std::to_string(latency));
        }
    }
    for (int factor : { 0, 3, 5, 100, -1 }) {
        const auto e = Fresh(Plain(0), 48000.0, factor);
        const int expect = factor >= 8 ? 8 : factor >= 4 ? 4 : factor >= 2 ? 2 : 1;
        Check(e->Oversampling() == expect, "oversampling " + std::to_string(factor) + " is not taken as " + std::to_string(expect));
    }
    // One preset of each board, and one doubled, cut into other blocks.
    for (std::size_t p : { 0, 3, 4, 7 }) {
        Controls c = kPresets[p].ToControls();
        if (p == 7)
            c[kDouble] = 1;
        const Audio a = Play(*Fresh(c, 48000.0, 8), Phrase(), 48000, 256);
        Check(Finite(a), std::string(kPresets[p].name) + " at 8x: not finite");
        for (int block : { 1, 77 }) {
            const Audio b = Play(*Fresh(c, 48000.0, 8), Phrase(), 48000, block);
            Check(a.left == b.left && a.right == b.right,
                  std::string(kPresets[p].name) + " at 8x: blocks of " + std::to_string(block) + " change the output");
        }
    }
    // Every board keeps its keys in tune at 8x.
    for (int board = 0; board < 2; ++board) {
        for (int key = 21; key <= 108; key += 11) {
            Controls c;
            c[kBoard] = board;
            const Audio a = Play(*Fresh(c, 48000.0, 8), { On(0, key) }, 24000);
            const double hz = 440.0 * std::pow(2.0, (key - 69) / 12.0);
            const double got = Measure(a.left, hz, 48000.0, 6000, 24000);
            Check(got > 0.0 && std::fabs(Cents(got, hz)) < 40.0,
                  "board " + std::to_string(board) + " key " + std::to_string(key) + " at 8x plays " + std::to_string(got) + " Hz");
        }
    }
}

// Double: a second set of units of the same board plays every note, the
// first set on the left and the second on the right, each set's crystal
// half the detune off its own.
void TestDouble() {
    // The owner's Double (2026-11-18) is two devices of the same kind set up
    // the same, whose natural differences make the stereo. Until 0.6.0 this
    // held each side to the single set bit for bit; now, with no detune, the
    // left is still the single set's sound bit for bit, and the right is
    // another unit's: not a copy, but the same note within its crystal's
    // ppm and its parts' tolerances (Boards/Board.h, UnitSpread).
    for (int board = 0; board < 2; ++board) {
        Controls c;
        c[kBoard] = board;
        const Audio one = Play(*Fresh(c), Phrase(), 96000);
        c[kDouble] = 1;
        c[kDetune] = 0;
        const Audio two = Play(*Fresh(c), Phrase(), 96000);
        Check(one.left == one.right, "board " + std::to_string(board) + " is not centred");
        Check(two.left == one.left, "board " + std::to_string(board) + ": Double's first set is not the single set");
        Check(two.right != two.left, "board " + std::to_string(board) + ": Double's second unit is a copy of the first");
        Controls held = Plain(board);
        held[kDouble] = 1;
        held[kDetune] = 0;
        const Audio a = Play(*Fresh(held), { On(0, 57) }, 48000);
        const double left = Measure(a.left, 220.0, 48000.0, 9600, 48000), right = Measure(a.right, 220.0, 48000.0, 9600, 48000);
        const double level = Rms(a.right, 9600, 48000) / Rms(a.left, 9600, 48000);
        Check(std::fabs(Cents(right, left)) < 0.1, "board " + std::to_string(board) + ": Double's units play A3 "
              + std::to_string(Cents(right, left)) + " cents apart with no detune");
        Check(level > 0.85 && level < 1.15, "board " + std::to_string(board) + ": Double's units differ in level by "
              + std::to_string(level));
    }
    // A3's period on the handheld moves 2.9 cents a step, so each side sits
    // within 3 cents of its half of a 20 cent detune; the crystals' pull
    // moves the whole unit, not its period steps, so the sides lie the
    // detune apart.
    Controls c = Plain(0);
    c[kDouble] = 1;
    c[kDetune] = 20;
    const Audio a = Play(*Fresh(c), { On(0, 57) }, 48000);
    const double a3 = 220.0;
    const double left = Measure(a.left, a3, 48000.0, 9600, 48000), right = Measure(a.right, a3, 48000.0, 9600, 48000);
    Check(std::fabs(Cents(left, a3) + 10.0) < 3.0, "Double's left plays A3 at " + std::to_string(Cents(left, a3)) + " cents");
    Check(std::fabs(Cents(right, a3) - 10.0) < 3.0, "Double's right plays A3 at " + std::to_string(Cents(right, a3)) + " cents");
    Check(std::fabs(Cents(right, left) - 20.0) < 0.2, "Double's sides lie " + std::to_string(Cents(right, left)) + " cents apart, not 20");
    // Each unit's driver steps on its own frame, counted from its own
    // power-on: on the console, a slow attack's first audible step comes
    // on the left a frame from the key and on the right at the second
    // unit's next frame, 16 ms before (it was switched on at another moment).
    {
        Controls slow = Plain(1);
        slow[kAttack] = 300;
        slow[kDouble] = 1;
        slow[kDetune] = 0;
        const Audio s = Play(*Fresh(slow), { On(0, 57) }, 4800);
        const auto onset = [](const std::vector<float>& x) {
            std::size_t i = 0;
            while (i < x.size() && std::fabs(x[i]) < 1.0e-4f)
                ++i;
            return static_cast<int>(i);
        };
        Check(std::abs(onset(s.left) - onset(s.right)) > 48, "Double's units step their envelopes on one frame: "
              + std::to_string(onset(s.left)) + " and " + std::to_string(onset(s.right)));
    }
    // Turned on under a held key, Double plays it on both sides; after the
    // key, both sides fall silent.
    Controls single = Plain(0);
    single[kRelease] = 0;
    const auto e = Fresh(single);
    std::vector<float> l(48000), r(48000);
    const std::uint8_t on[] = { 0x90, 64, 100 }, off[] = { 0x80, 64, 0 };
    e->Midi(on, 3);
    e->Process(l.data(), r.data(), 4800);
    single[kDouble] = 1;
    e->SetControls(single);
    e->Process(l.data() + 4800, r.data() + 4800, 19200);
    e->Midi(off, 3);
    e->Process(l.data() + 24000, r.data() + 24000, 24000);
    Check(Rms(r, 12000, 24000) > 0.03 && Rms(l, 12000, 24000) > 0.03, "Double turned on under a held key is not heard on both sides");
    Check(Settles(l, 24000 + kLate + 804) && Settles(r, 24000 + kLate + 804), "Double's sides do not stop with the key");
}

void TestVoices() {
    // Two machines' two pulses: four voices, the oldest stolen.
    {
        Controls c = Plain(0);
        const auto e = Fresh(c);
        std::vector<Event> ev;
        for (int i = 0; i < 6; ++i)
            ev.push_back(On(i * 100, 60 + i));
        Play(*e, ev, 2000);
        Check(e->Sounding() == 4, "handheld x2: " + std::to_string(e->Sounding()) + " voices, not 4");
        c[kBoards] = 8;
        e->SetControls(c);
        Play(*e, {}, 256);
        Check(e->Sounding() == 6, "handheld x8: the held keys are not played again on the new machines");
    }
    // Under the pulses' 64 Hz the handheld's wave channel takes the key.
    {
        const auto e = Fresh(Plain(0));
        Play(*e, { On(0, 30) }, 2000);
        Check(e->Sounding() == 1 && e->MachineAt(0).HandheldBoard().Chip().DacOn(2), "the low key does not go to the wave channel");
    }
    // Poly channels on: one machine plays a chord on its unlike voices. The
    // part's channels come first and the others only once those are held,
    // so a melody keeps its voice whatever its release.
    {
        Controls c = Plain(0);
        c[kBoards] = 1;
        {
            const auto e = Fresh(c);
            Play(*e, { On(0, 60), On(100, 64), On(200, 67) }, 2000);
            Check(e->Sounding() == 2, "handheld, the part's channels: a third key finds a third voice");
        }
        c[kPolyChannels] = 1;
        c[kRelease] = 2000;
        const auto e = Fresh(c);
        Play(*e, { On(0, 60), On(100, 64), On(200, 67) }, 2000);
        const auto at = [&](int ch) { return e->HoldingAt(0, ch); };
        Check(e->Sounding() == 3 && at(0).key == 60 && at(1).key == 64 && at(2).key == 67
                  && at(0).voice == Voice::Pulse && at(2).voice == Voice::Wave,
              "handheld, the chip's channels: the chord is not two pulses and the wave");
        Play(*e, { On(0, 72) }, 2000);
        Check(e->Sounding() == 3 && at(0).key == 72, "handheld, the chip's channels: a fourth key does not take the oldest");
        Play(*e, { Off(0, 60), Off(0, 64), Off(0, 67), Off(0, 72) }, 48000 * 3);
        Check(e->Sounding() == 0, "handheld, the chip's channels: the chord does not end");
        Play(*e, { On(0, 60), Off(2000, 60), On(4000, 62), Off(6000, 62), On(8000, 64), Off(10000, 64), On(12000, 65) }, 14000);
        Check(!at(2).busy && (at(0).key == 65 || at(1).key == 65),
              "handheld, the chip's channels: a melody with a long release wanders onto the wave");
    }
    // The console's chord: two pulses and the triangle, then the cartridge's
    // voices; the noise is no part of it.
    {
        Controls c = Plain(1);
        c[kBoards] = 1;
        c[kPolyChannels] = 1;
        {
            const auto e = Fresh(c);
            Play(*e, { On(0, 60), On(100, 64), On(200, 67), On(300, 72) }, 2000);
            Check(e->Sounding() == 3 && e->HoldingAt(0, 2).voice == Voice::Triangle && e->HoldingAt(0, 2).busy
                      && !e->HoldingAt(0, 3).busy,
                  "console, the chip's channels: the chord is not two pulses and the triangle");
        }
        c[kRevision] = 1;
        c[kExpansion] = 1;
        {
            const auto e = Fresh(c);
            std::vector<Event> ev;
            for (int i = 0; i < 8; ++i)
                ev.push_back(On(i * 100, 60 + i));
            Play(*e, ev, 2000);
            Check(e->Sounding() == 6, "console with expansion A, the chip's channels: "
                                          + std::to_string(e->Sounding()) + " voices, not 6");
        }
        c[kPart] = 2;
        {
            const auto e = Fresh(c);
            Play(*e, { On(0, 60), On(100, 64) }, 2000);
            Check(e->Sounding() == 1, "the noise part spills onto pitched channels");
        }
        // With Double the second unit plays the same chord.
        c[kPart] = 0;
        c[kDouble] = 1;
        {
            const auto e = Fresh(c);
            const Audio a = Play(*e, { On(0, 60), On(100, 64), On(200, 67) }, 24000);
            Check(e->Sounding() == 3 && Rms(a.right, 12000, 24000) > 0.5 * Rms(a.left, 12000, 24000),
                  "the chip's channels with Double: the second unit does not play the chord");
        }
    }
}

// Across the hardware pulse floor, Mono must keep gliding on the fallback
// channel and return to an actual pulse once that register can play the key.
void TestLegatoRange() {
    for (int board : { 0, 1 }) {
        for (int glide : { 0, 250 }) {
            Controls c = Plain(board);
            c[kBoards] = 1;
            c[kMode] = 1;
            c[kGlide] = glide;
            c[kAttack] = 750;
            const auto e = Fresh(c);
            const std::string which = "board " + std::to_string(board) + ", glide " + std::to_string(glide);
            Play(*e, { On(0, 45) }, 48000); // A2 on pulse: already at full sustain.
            const Audio low = Play(*e, { On(0, 21) }, 48000); // A0 needs wave or triangle.
            const double hz = Measure(low.left, 27.5, 48000.0, 16000, 48000);
            Check(std::fabs(Cents(hz, 27.5)) < 5.0, which + ": downward legato landed at " + std::to_string(hz) + " Hz");
            const double full = Rms(low.left, 36000, 48000);
            Check(full > 0.005 && Rms(low.left, 16000, 22000) > 0.8 * full,
                  which + ": legato channel handoff restarted the attack");

            const Audio high = Play(*e, { Off(0, 21) }, 48000); // Back to the held A2.
            const double back = Measure(high.left, 110.0, 48000.0, 16000, 48000);
            Check(std::fabs(Cents(back, 110.0)) < 5.0, which + ": upward legato landed at " + std::to_string(back) + " Hz");
            bool pulseActive = false;
            float l[17], r[17];
            for (int i = 0; i < 100; ++i) {
                e->Process(l, r, 17);
                const Machine& m = e->MachineAt(0);
                pulseActive |= board == 0 ? (m.HandheldBoard().Chip().Out(0) | m.HandheldBoard().Chip().Out(1)) != 0
                                          : (m.ConsoleBoard().Apu().Out(0) | m.ConsoleBoard().Apu().Out(1)) != 0;
            }
            Check(pulseActive && e->Sounding() == 1, which + ": returning to A2 did not hand the voice back to pulse");
        }
    }
}

// Selecting Arp over an existing chord starts on its lowest key. If that
// pitch is below even the triangle's register floor, later steps still play.
void TestPolyRange() {
    for (int board : { 0, 1 }) {
        Controls c = Plain(board);
        c[kBoards] = 2;
        c[kBendRange] = 12;
        const auto e = Fresh(c);
        Play(*e, { On(0, 43) }, 12000);
        e->SetBend(-1.0);
        const Audio down = Play(*e, {}, 48000);
        const double low = Measure(down.left, 49.0, 48000, 12000, 48000);
        Check(std::fabs(Cents(low, 49.0)) < 5.0,
              "Poly: the held G2 bent down to " + std::to_string(low) + " Hz on board " + std::to_string(board));
        e->SetBend(0.0);
        const Audio up = Play(*e, {}, 24000);
        Check(std::fabs(Cents(Measure(up.left, 98.0, 48000, 6000, 24000), 98.0)) < 5.0,
              "Poly: the G2 did not return from its fallback");

        // Both original pulses fit on the first machine, but each machine
        // has just one fallback channel. Both bent voices still fit the rig.
        e->NoteOn(44, 100);
        e->SetBend(-1.0);
        Play(*e, {}, 16000);
        for (int b = 0; b < 2; ++b) {
            const Machine& m = e->MachineAt(b);
            int first = -1;
            bool varied = false;
            float l[13], r[13];
            for (int i = 0; i < 160; ++i) {
                e->Process(l, r, 13);
                const int level = board == 0 ? m.HandheldBoard().Chip().Out(2) : m.ConsoleBoard().Apu().Out(2);
                if (i == 0)
                    first = level;
                varied |= level != first;
            }
            Check(varied, "Poly: bent chord missing fallback on machine " + std::to_string(b));
        }
        e->ReleaseAll();
        Play(*e, {}, 48000);
        Check(e->Sounding() == 0, "Poly: a migrated chord did not release");
    }

    // One machine cannot play both pulses on its single fallback. When
    // the migrated G is released, the other bent key must claim that slot.
    Controls one = Plain(0);
    one[kBoards] = 1;
    one[kBendRange] = 12;
    one[kRelease] = 0;
    const auto crowded = Fresh(one);
    Play(*crowded, { On(0, 43), On(500, 44) }, 12000);
    crowded->SetBend(-1.0);
    Play(*crowded, {}, 12000);
    const Audio remaining = Play(*crowded, { Off(0, 43) }, 48000);
    const double flat = Measure(remaining.left, 51.91, 48000, 12000, 48000);
    Check(std::fabs(Cents(flat, 51.91)) < 5.0 && crowded->Sounding() == 1,
          "Poly: waiting bent key did not take the newly freed fallback");
    const Audio empty = Play(*crowded, { Off(0, 44) }, 24000);
    Check(crowded->Sounding() == 0 && Settles(empty.left, 12000), "Poly: fallback remained gated after its own note-off");

    Controls c = Plain(1);
    c[kBendRange] = 12;
    const auto e = Fresh(c);
    Play(*e, { On(0, 31) }, 12000); // G1 already needs the triangle fallback.
    e->SetBend(-1);
    const Audio outside = Play(*e, {}, 48000);
    Check(Settles(outside.left, 20000), "Poly: an unplayable bend holds the triangle's previous pitch");
    e->SetBend(0);
    const Audio back = Play(*e, {}, 48000);
    Check(std::fabs(Cents(Measure(back.left, 49.0, 48000, 12000, 48000), 49.0)) < 5.0,
          "Poly: triangle did not recover after the bend came back in range");
}

// A0 fits the Console triangle, but a downward bend/tuning can put it
// below that register before note-on. Keeping the key down must let it
// sound when a wheel or automation brings it back into range.
void TestHeldPitchRecovery() {
    for (int mode : { 0, 1, 2 }) {
        for (int move : { 0, 1, 2 }) {
            Controls c = Plain(1);
            c[kDouble] = 1;
            c[kDetune] = 4;
            c[kMode] = mode;
            c[kBendRange] = 12;
            if (move == 2)
                c[kTranspose] = -12;
            const auto e = Fresh(c);
            if (move == 0)
                e->SetBend(-1);
            if (move == 1)
                e->SetTune(-100);
            const Audio outside = Play(*e, { On(0, 21) }, 9600);
            Check(Peak(outside.left) < 1.0e-6 && Peak(outside.right) < 1.0e-6, "unplayable A0 sounded at note-on");
            if (move == 0)
                e->SetBend(0);
            if (move == 1)
                e->SetTune(0);
            if (move == 2) {
                c[kTranspose] = 0;
                e->SetControls(c);
            }
            const Audio inside = Play(*e, {}, 48000);
            for (const auto* side : { &inside.left, &inside.right }) {
                const double hz = Measure(*side, 27.5, 48000, 12000, 48000);
                Check(std::fabs(Cents(hz, 27.5)) < 8.0,
                      "held A0: mode " + std::to_string(mode) + ", pitch source " + std::to_string(move) + ", Hz " + std::to_string(hz));
            }
        }
    }
    // A0 starts below the triangle's reach. When a bend brings it back,
    // its pending onset must wait for the only channel, already playing A2.
    Controls triangle = Plain(1, 1);
    triangle[kBoards] = 1;
    triangle[kBendRange] = 12;
    triangle[kRelease] = 0;
    for (bool liftOlder : { false, true }) {
        const auto e = Fresh(triangle);
        e->SetBend(-1);
        Play(*e, { On(0, 21), On(1200, 45) }, 9600);
        e->SetBend(0);
        const Audio recent = Play(*e, {}, 24000);
        Check(std::fabs(Cents(Measure(recent.left, 110.0, 48000, 6000, 24000), 110.0)) < 8.0,
              "pending A0 stole the held A2 on pitch recovery");
        const int released = liftOlder ? 21 : 45;
        const double remaining = liftOlder ? 110.0 : 27.5;
        const Audio held = Play(*e, { Off(0, released) }, 48000);
        Check(std::fabs(Cents(Measure(held.left, remaining, 48000, 12000, 48000), remaining)) < 8.0,
              "releasing one key lost the other during pending pitch recovery");
    }

    Controls wave = Plain(0, 1);
    wave[kBoards] = 1;
    wave[kRelease] = 0;
    const auto stolen = Fresh(wave);
    Play(*stolen, { On(0, 60), On(100, 67), Off(1000, 67) }, 24000);
    stolen->SetTune(50);
    const Audio silent = Play(*stolen, {}, 16000);
    Check(stolen->Sounding() == 0 && Settles(silent.left, 0), "a pitch change resurrected a stolen Poly note");

}

void TestInactiveMachineControls() {
    for (int setup : { 0, 1, 2 }) {
        Controls original = Plain(setup == 0 ? 0 : 1);
        original[kRevision] = setup == 1 ? Console::kFrontLoader : 1;
        original[kExpansion] = setup == 2 ? Console::kFds : Console::kNone;
        original[kAttack] = 350;
        const auto reference = Fresh(original), changed = Fresh(original);
        Play(*reference, { On(0, 69) }, 16000);
        Play(*changed, { On(0, 69) }, 16000);
        Controls next = original;
        next[kExpansionChannels] = 8;
        if (setup != 2)
            next[kExpansion] = Console::kN163; // Neither machine has an active cartridge slot.
        if (setup == 0)
            next[kRevision] = 2; // Both settings select the same physical Handheld revision.
        changed->SetControls(next);
        const Audio a = Play(*reference, {}, 16000);
        const Audio b = Play(*changed, {}, 16000);
        Check(a.left == b.left && a.right == b.right, "inactive machine settings restarted setup " + std::to_string(setup));

        next[kBoard] = 1;
        next[kRevision] = Console::kFamicom;
        next[kExpansion] = Console::kN163;
        changed->SetControls(next);
        Check(changed->MachineAt(0).PartOf(3).count == 8, "deferred cartridge channel count lost");
    }
}

void TestArpRange() {
    Controls c = Plain(1);
    c[kArpRate] = 6;
    const auto e = Fresh(c);
    e->NoteOn(12, 127);
    e->NoteOn(69, 127);
    c[kMode] = 2;
    e->SetControls(c);
    const Audio a = Play(*e, {}, 48000);
    Check(Rms(a.left, 6000, 9000) > 0.02 && Rms(a.left, 25500, 27000) > 0.02,
          "arp: a key outside the console's range silences the higher steps");
}

// Editing the chord below the sounding step leaves the next higher pitch next.
void TestArpChordEdits() {
    Controls c = Plain(0);
    c[kMode] = 2;
    c[kArpRate] = 6;
    for (const Event& edit : { On(7000, 55), Off(7000, 60), Off(7000, 64) }) {
        const auto e = Fresh(c);
        const Audio a = Play(*e, { On(0, 60), On(0, 64), On(0, 67), edit }, 14400);
        const double hz = Measure(a.left, 392.0, 48000, 10800, 14200);
        Check(std::fabs(Cents(hz, 392.0)) < 6.0,
              "Arp: changed chord after E4 landed at " + std::to_string(hz) + " Hz instead of G4");
    }
}

void TestModes() {
    // Mono: one voice, legato moves it, and lifting the top key returns to
    // the one under it.
    {
        Controls c = Plain(0);
        c[kMode] = 1;
        const auto e = Fresh(c);
        const double a4 = 131072.0 / 298.0, c5 = 131072.0 / 250.0;
        const Audio a = Play(*e, { On(0, 69), On(24000, 72), Off(48000, 72) }, 72000);
        Check(e->Sounding() == 1, "mono: more than one voice");
        Check(std::fabs(Cents(Measure(a.left, c5, 48000.0, 30000, 48000), c5)) < 1.0, "mono: the legato key does not sound");
        Check(std::fabs(Cents(Measure(a.left, a4, 48000.0, 54000, 72000), a4)) < 1.0, "mono: lifting the top key does not return");
    }
    // Glide: a legato move slides through the pitches between.
    {
        Controls c = Plain(0);
        c[kMode] = 1;
        c[kGlide] = 500;
        const auto e = Fresh(c);
        const Audio a = Play(*e, { On(0, 57), On(24000, 69) }, 72000);
        const double a3 = 131072.0 / 596.0, a4 = 131072.0 / 298.0;
        const double mid = Measure(a.left, std::sqrt(a3 * a4), 48000.0, 30000, 33000);
        Check(mid > a3 * 1.05 && mid < a4 * 0.95, "glide: no pitch between the keys mid-glide (" + std::to_string(mid) + ")");
        Check(std::fabs(Cents(Measure(a.left, a4, 48000.0, 54000, 72000), a4)) < 1.0, "glide: does not arrive");
    }
    // Arp: one voice stepping through the held keys, a step every Rate
    // frames.
    {
        Controls c = Plain(0);
        c[kMode] = 2;
        c[kArpRate] = 6;
        const auto e = Fresh(c);
        const Audio a = Play(*e, { On(0, 60), On(0, 64), On(0, 67) }, 48000);
        Check(e->Sounding() == 1, "arp: more than one voice");
        // Six frames of 70224 cycles at 4194304 Hz: 0.1005 s a step. Each
        // step's own pitch, measured inside it.
        const double step = 6.0 * 70224.0 / 4194304.0 * 48000.0;
        const int counts[3] = { 501, 398, 334 };   // C4, E4, G4 (2048 - x)
        for (int i = 2; i < 5; ++i) {
            const double f = 131072.0 / counts[i % 3];
            const auto from = static_cast<std::size_t>(i * step + 600), to = static_cast<std::size_t>((i + 1) * step - 100);
            Check(std::fabs(Cents(Measure(a.left, f, 48000.0, from, to), f)) < 5.0, "arp: step " + std::to_string(i) + " off its key");
        }
    }
    // The arpeggio's order. Four keys struck out of order; after its first
    // round the note walks each order's own round.
    {
        Controls c = Plain(0);
        c[kBoards] = 1;
        c[kMode] = 2;
        c[kArpRate] = 2;
        const std::vector<std::vector<int>> rounds = {
            { 60, 64, 67, 72 }, { 72, 67, 64, 60 }, { 60, 64, 67, 72, 67, 64 }, { 60, 67, 64, 72 } };
        for (int order = 0; order < 4; ++order) {
            c[kArpOrder] = order;
            const auto e = Fresh(c);
            Play(*e, { On(0, 60), On(0, 67), On(0, 64), On(0, 72) }, 16);
            // The keys the note takes, one entry a change.
            std::vector<int> keys;
            for (int i = 0; i < 2000; ++i) {
                Play(*e, {}, 16);
                const int key = e->HoldingAt(0, 0).key;
                if (keys.empty() || keys.back() != key)
                    keys.push_back(key);
            }
            const std::vector<int>& round = rounds[static_cast<std::size_t>(order)];
            bool ok = keys.size() >= 3 * round.size();
            // From the second round on, some rotation of the order's round.
            std::size_t at = 0;
            while (ok && at < round.size() && round[at] != keys[round.size()])
                ++at;
            for (std::size_t i = 0; ok && i < 2 * round.size(); ++i)
                ok = keys[round.size() + i] == round[(at + i) % round.size()];
            Check(ok, "arp order " + std::to_string(order) + ": the note does not walk its round");
        }
    }
    // Arp retrigger off: a step moves the pitch and leaves the envelope, so
    // a decay to silence runs out while the note keeps stepping.
    {
        Controls c = Plain(0);
        c[kBoards] = 1;
        c[kMode] = 2;
        c[kArpRate] = 3;
        c[kDecay] = 300;
        c[kSustain] = 0;
        double late[2] {};
        for (int retrigger = 0; retrigger < 2; ++retrigger) {
            c[kArpRetrigger] = retrigger;
            const auto e = Fresh(c);
            const Audio a = Play(*e, { On(0, 60), On(0, 64), On(0, 67) }, 48000);
            late[retrigger] = Rms(a.left, 36000, 48000);
            const int key = e->HoldingAt(0, 0).key;
            Play(*e, {}, 4000);
            Check(e->Sounding() == 1 && e->HoldingAt(0, 0).key != key, "arp: the note stops stepping");
        }
        Check(late[1] > 0.01 && late[0] < 1.0e-4, "arp retrigger off: the steps restart the envelope ("
                                                      + std::to_string(late[0]) + " against " + std::to_string(late[1]) + ")");
    }
    // Driver rate: the driver's ticks a frame. The arpeggio counts ticks,
    // so it steps as many times as often; times in ms stay where they are.
    {
        Controls c = Plain(0);
        c[kBoards] = 1;
        c[kMode] = 2;
        c[kArpRate] = 1;
        int steps[2] {};
        for (int k = 0; k < 2; ++k) {
            c[kDriverRate] = k ? 4 : 1;
            const auto e = Fresh(c);
            Play(*e, { On(0, 60), On(0, 64), On(0, 67) }, 16);
            int last = -1;
            for (int i = 0; i < 3000; ++i) {   // one second
                Play(*e, {}, 16);
                const int key = e->HoldingAt(0, 0).key;
                steps[k] += key != last;
                last = key;
            }
        }
        Check(std::abs(steps[0] - 60) <= 2 && std::abs(steps[1] - 239) <= 3,
              "driver rate: " + std::to_string(steps[0]) + " and " + std::to_string(steps[1]) + " arpeggio steps a second, not 60 and 239");
        Controls d = Plain(0);
        d[kBoards] = 1;
        d[kAttack] = 400;
        double half[2] {}, full[2] {};
        for (int k = 0; k < 2; ++k) {
            d[kDriverRate] = k ? 8 : 1;
            const auto e = Fresh(d);
            const Audio a = Play(*e, { On(0, 69) }, 48000);
            half[k] = Rms(a.left, 9600 - 480, 9600 + 480);    // 200 ms in: near half way
            full[k] = Rms(a.left, 28800, 38400);
        }
        Check(std::fabs(full[1] / full[0] - 1.0) < 0.01 && std::fabs(half[1] / half[0] - 1.0) < 0.15
                  && half[0] > 0.3 * full[0] && half[0] < 0.7 * full[0],
              "driver rate: a 400 ms attack is not where it was at 8 ticks a frame");
    }
}

void TestPedalAndPanic() {
    Controls c = Plain(0);
    c[kRelease] = 0;
    {
        const auto e = Fresh(c);
        const Audio a = Play(*e, { On(0, 69), Cc(100, 64, 127), Off(200, 69) }, 48000);
        Check(e->Sounding() == 1 && Rms(a.left, 40000, 48000) > 0.03, "the pedal does not hold a lifted key");
        const Audio b = Play(*e, { Cc(0, 64, 0) }, 4800);
        Check(e->Sounding() == 0 && Settles(b.left, kLate), "lifting the pedal does not release");
    }
    {
        // A key struck again under the pedal is one note, not two.
        const auto e = Fresh(c);
        Play(*e, { Cc(0, 64, 127), On(10, 69), Off(20, 69), On(30, 69), Off(40, 69), On(50, 69) }, 4800);
        Check(e->Sounding() == 1, "a re-struck key under the pedal is " + std::to_string(e->Sounding()) + " notes");
    }
    {
        Controls mono = Plain(0);
        mono[kMode] = 1;
        mono[kRelease] = 1500;
        const auto e = Fresh(mono);
        Play(*e, { Cc(0, 64, 127), On(100, 57), Off(1000, 57), On(2000, 69), Off(3000, 69) }, 12000);
        const Audio tail = Play(*e, { Cc(0, 64, 0) }, 24000);
        const double hz = Measure(tail.left, 311.0, 48000, 4800, 24000);
        Check(std::fabs(Cents(hz, 440.0)) < 5.0, "Mono pedal-up bent the A4 release tail to " + std::to_string(hz));
    }
    for (int cc : { 120, 123 }) {
        Controls slow = Plain(0);
        slow[kRelease] = 500;
        const auto e = Fresh(slow);
        Play(*e, { On(0, 60), On(0, 64), Cc(10, 64, 127) }, 4800);
        const Audio a = Play(*e, { Cc(0, cc, 0) }, 48000);
        if (cc == 120)
            Check(e->Sounding() == 0 && Settles(a.left, kLate), "all sound off does not silence at once");
        else
            Check(Rms(a.left, kLate, 4800) > 0.01 && e->Sounding() == 0 && Settles(a.left, 26000),
                  "all notes off does not release");
    }
}

// A host's audio reset leaves every board as Prepare does: what follows is
// the same bits a fresh engine plays, the pedal and held keys gone, the
// filters and the output stage's look-ahead at rest. One preset of each
// board, one doubled, at 1x and 4x.
void TestReset() {
    for (std::size_t p : { 0, 3, 4, 7 }) {
        for (int factor : { 1, 4 }) {
            Controls c = kPresets[p].ToControls();
            if (p == 7)
                c[kDouble] = 1;
            const auto e = Fresh(c, 48000.0, factor);
            Play(*e, { On(0, 60), On(0, 67), Cc(10, 64, 127), Off(20, 60), Bend(30, 0.0) }, 9000);
            e->Reset();
            const Audio a = Play(*e, Phrase(), 48000);
            const Audio b = Play(*Fresh(c, 48000.0, factor), Phrase(), 48000);
            Check(a.left == b.left && a.right == b.right,
                  std::string(kPresets[p].name) + " at " + std::to_string(factor) + "x: a reset does not play as a fresh engine");
        }
    }
}

// The level about its own mean between two samples, so the Pin blocker's
// slow drift while a level moves does not count.
double AcRms(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double mean = 0.0;
    for (std::size_t i = from; i < to; ++i)
        mean += x[i];
    mean /= static_cast<double>(to - from);
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i)
        sum += (x[i] - mean) * (x[i] - mean);
    return std::sqrt(sum / static_cast<double>(to - from));
}

// The envelope moves the volume at the rate that takes each stage its time
// and writes the chip the whole step it has reached, once a frame; the
// handheld's DAC is linear, so the level is the step over 15. A frame is
// 70224 cycles at 4194304 Hz, 803.65 samples; frame k's tick lands 112
// samples late (the engine's latency). Windows are whole A4 periods inside
// one frame. Each time here is a whole number of frames, so every frame's
// step is exact.
void TestEnvelope() {
    constexpr double kFrame = 70224.0 / 4194304.0 * 48000.0;
    constexpr double kPeriod = 48000.0 * 298.0 / 131072.0;
    const auto ms = [](double frames) { return frames * 70224.0 / 4194304.0 * 1000.0; };
    const auto window = [&](double fromFrame, int periods) {
        const auto from = static_cast<std::size_t>(fromFrame * kFrame + 112.0 + 150.0);
        return std::make_pair(from, from + static_cast<std::size_t>(periods * kPeriod));
    };
    const auto level = [&](const Audio& a, std::pair<std::size_t, std::size_t> w) { return AcRms(a.left, w.first, w.second); };
    const Audio full = Play(*Fresh(Plain(0)), { On(0, 69) }, 48000);
    const double reference = level(full, window(40.0, 40));

    // Attack over 30 frames: step round(k / 2) at frame k, full from 30.
    {
        Controls c = Plain(0);
        c[kAttack] = ms(30.0);
        const Audio a = Play(*Fresh(c), { On(0, 69) }, 48000);
        Check(std::fabs(Db(level(a, window(14.0, 5)) / reference) - Db(7.0 / 15.0)) < 0.5, "attack: step 7 is not at frame 14");
        Check(std::fabs(Db(level(a, window(31.0, 40)) / reference)) < 0.2, "attack: not full from frame 30");
    }
    // Decay over 30 frames to sustain 5: step 15 - k / 3 at frame k, then 5
    // held.
    {
        Controls c = Plain(0);
        c[kDecay] = ms(30.0);
        c[kSustain] = 5;
        const Audio a = Play(*Fresh(c), { On(0, 69) }, 48000);
        Check(std::fabs(Db(level(a, window(15.0, 5)) / reference) - Db(10.0 / 15.0)) < 0.5, "decay: step 10 is not at frame 15");
        Check(std::fabs(Db(level(a, window(40.0, 40)) / reference) - Db(5.0 / 15.0)) < 0.2, "decay: the sustain level does not hold");
    }
    // A time under a frame is no time: full at once.
    {
        Controls c = Plain(0);
        c[kAttack] = ms(0.9);
        const Audio a = Play(*Fresh(c), { On(0, 69) }, 48000);
        Check(std::fabs(Db(level(a, window(0.0, 5)) / reference)) < 0.2, "attack: under a frame is not at once");
    }
    // Sustain moved to 0 and back while the key holds: the note follows.
    {
        Controls c = Plain(0);
        const auto e = Fresh(c);
        Play(*e, { On(0, 69) }, 12000);
        c[kSustain] = 0;
        e->SetControls(c);
        const Audio quiet = Play(*e, {}, 12000);
        c[kSustain] = 15;
        e->SetControls(c);
        const Audio back = Play(*e, {}, 24000);
        // The level moves on the next frame, up to one after the change.
        Check(Settles(quiet.left, kLate + 804) && std::fabs(Db(level(back, { 12000, 24000 }) / reference)) < 0.2,
              "sustain: a held note does not come back from sustain 0");
    }
    // Release over 60 frames from the key at sample 24000, inside frame 29:
    // step 15 - j / 4 at frame 29 + j, silent from frame 89.
    {
        Controls c = Plain(0);
        c[kRelease] = ms(60.0);
        const auto e = Fresh(c);
        const Audio a = Play(*e, { On(0, 69), Off(24000, 69) }, 72000);
        Check(std::fabs(Db(level(a, window(49.0, 5)) / reference) - Db(10.0 / 15.0)) < 0.5, "release: step 10 is not at frame 49");
        Play(*e, {}, 2400);
        Check(e->Sounding() == 0, "release: the note outlives its last step");
    }
    // Each step of the arpeggio starts the envelope again: a decay over six
    // frames to sustain 0 falls from 15 to step 3 inside every six-frame
    // step and is back at 15 on the next.
    {
        Controls c = Plain(0);
        c[kMode] = 2;
        c[kArpRate] = 6;
        c[kDecay] = ms(6.0);
        c[kSustain] = 0;
        const Audio a = Play(*Fresh(c), { On(0, 60), On(0, 64), On(0, 67) }, 48000);
        for (int i = 2; i < 6; ++i) {
            const auto start = static_cast<std::size_t>(6.0 * i * kFrame + 112.0);
            const double early = AcRms(a.left, start + 100, start + 700);
            const double late = AcRms(a.left, start + static_cast<std::size_t>(5.0 * kFrame) + 100,
                                      start + static_cast<std::size_t>(5.0 * kFrame) + 700);
            Check(early > 0.8 * reference && late < 0.3 * reference,
                  "arp: step " + std::to_string(i) + " does not start the envelope again (" + std::to_string(early / reference)
                      + ", " + std::to_string(late / reference) + ")");
        }
    }
}

// Envelope Chip, the sweeps and Sides: the chips' own units, played as a
// driver plays them. The handheld's envelope steps every pace/64 s and its
// sweep every pace/128 s (Pan Docs); the console's envelope every pace
// quarter frames, 1/240 s on NTSC (NESdev). Levels are against a note at
// full volume: the handheld's DAC is linear, so a level is its step over
// 15; the console's pulse mixer gives 95.88 / (8128 / n + 100) (NESdev, "APU
// Mixer").
void TestChipUnits() {
    constexpr double kRate = 48000.0;
    const auto step = [](int board, double n) {
        const auto pulse = [](double v) { return v > 0.0 ? 95.88 / (8128.0 / v + 100.0) : 0.0; };
        return board == 0 ? n / 15.0 : pulse(n) / pulse(15.0);
    };
    const auto ms = [](double frames) { return frames * 70224.0 / 4194304.0 * 1000.0; };
    const auto at = [&](double seconds) { return static_cast<std::size_t>(seconds * kRate); };
    const auto level = [&](const Audio& a, double seconds) { return AcRms(a.left, at(seconds), at(seconds) + 960); };
    for (int board = 0; board < 2; ++board) {
        const std::string name = board == 0 ? "handheld" : "console";
        const Audio full = Play(*Fresh(Plain(board)), { On(0, 69) }, 48000);
        const double reference = level(full, 0.5);

        // A decay to sustain 10 that the driver would hold at 10: the unit
        // runs on to 0 under the held key. Its step is the driver's, 2/64 s
        // on the handheld (Decay over 28/3 frames for its five steps) and 2
        // quarter frames on the console at its 15 steps over 5 of 15 kept.
        Controls c = Plain(board);
        c[kSustain] = 10;
        c[kDecay] = board == 0 ? ms(131072.0 * 5.0 / 70224.0) : 2.0 * 7457.5 * 5.0 / 1789773.0 * 1000.0;
        const Audio driver = Play(*Fresh(c), { On(0, 69) }, 48000);
        c[kEnvelope] = 1;
        const Audio chip = Play(*Fresh(c), { On(0, 69) }, 48000);
        Check(std::fabs(Db(level(driver, 0.7) / reference) - Db(step(board, 10.0))) < 0.3, name + ": the driver's decay does not hold at sustain 10");
        Check(level(chip, 0.7) < 0.02 * reference, name + ": the chip's envelope does not run on to 0 under the held key");
        Check(level(chip, 0.01) > 0.8 * reference, name + ": the chip's envelope does not start from the top");
        const double time = board == 0 ? 0.25 : 0.1;            // 8 steps of the handheld's, 12 of the console's
        const double expect = board == 0 ? 7.0 : 3.0;
        Check(std::fabs(level(chip, time) / reference - step(board, expect)) < 1.6 / 15.0,
              name + ": the chip's envelope is not at step " + std::to_string(static_cast<int>(expect)) + " (" + std::to_string(15.0 * level(chip, time) / reference) + ")");

        // The level the unit starts from: the handheld's is the Level, the
        // console's always 15.
        c[kLevel] = 6;
        const Audio low = Play(*Fresh(c), { On(0, 69) }, 4800);
        const double start = level(low, 0.005) / reference;
        Check(board == 0 ? std::fabs(start - step(0, 6.0)) < 0.05 : start > 0.9,
              name + ": the chip's envelope starts from the wrong level (" + std::to_string(15.0 * start) + ")");

        // Partition: the same samples in any blocks.
        c[kLevel] = 15;
        c[kSweep] = -2;
        Check(Play(*Fresh(c), Phrase(), 48000, 256).left == Play(*Fresh(c), Phrase(), 48000, 61).left,
              name + ": the chip's units differ with the block size");
    }

    // Looping, the console's: the envelope falls from 15 again and again
    // while the key holds, where the handheld's (no loop) plays as Chip.
    {
        Controls c = Plain(1);
        c[kSustain] = 0;
        c[kDecay] = 50;
        c[kEnvelope] = 2;
        const auto e = Fresh(c);
        const Audio a = Play(*e, { On(0, 69), Off(96000, 69) }, 120000);
        double most = 0.0, least = 1.0e9;
        for (double t = 1.5; t < 1.9; t += 0.01) {
            most = std::max(most, AcRms(a.left, at(t), at(t) + 240));
            least = std::min(least, AcRms(a.left, at(t), at(t) + 240));
        }
        const double reference = AcRms(Play(*Fresh(Plain(1)), { On(0, 69) }, 48000).left, at(0.5), at(0.5) + 960);
        Check(most > 0.7 * reference && least < 0.2 * reference, "console: the looping envelope does not fall again and again");
        Play(*e, {}, 48000);
        Check(e->Sounding() == 0, "console: a looping envelope's note does not end after its release");
    }

    // The handheld's climb and its release: Attack over 14 frames is pace
    // 1, 15 steps of 1/64 s from 0 to 15, whatever the Level; the release
    // over 60 frames falls from 15 at pace 4 (15 steps of 4/64 s).
    {
        Controls c = Plain(0);
        c[kLevel] = 6;
        c[kAttack] = ms(14.0);
        c[kRelease] = ms(60.0);
        c[kEnvelope] = 1;
        const auto e = Fresh(c);
        const Audio a = Play(*e, { On(0, 69), Off(24000, 69) }, 72000);
        const double reference = AcRms(Play(*Fresh(Plain(0)), { On(0, 69) }, 48000).left, at(0.5), at(0.5) + 960);
        Check(std::fabs(level(a, 0.4) / reference - 1.0) < 0.05, "handheld: the chip's climb does not reach 15");
        Check(std::fabs(level(a, 0.1) / reference - 6.0 / 15.0) < 1.6 / 15.0, "handheld: the chip's climb is not at step 6");
        Check(std::fabs(level(a, 0.5 + 0.5) / reference - 7.0 / 15.0) < 1.6 / 15.0, "handheld: the chip's release is not at step 7");
        Play(*e, {}, 24000);
        Check(e->Sounding() == 0, "handheld: the chip's release does not end the note");
    }

    // The sweeps. The handheld's down by 1 (shift 7) at pace 7: x loses
    // x >> 7 every 7/128 s, so A4's 1750 is 1631 after nine steps, 314 Hz.
    // Up by 7 (shift 1) A4's 1750 + 875 is past 2047 at the trigger, and
    // the chip turns the channel off. The console's up by 2 (shift 6) at
    // pace 1, every half frame: A4's 253 falls by 253 >> 6 each, under 8
    // within a second, where the sweep mutes it.
    {
        Controls c = Plain(0);
        c[kSweep] = -1;
        c[kSweepPace] = 8;
        const Audio a = Play(*Fresh(c), { On(0, 69) }, 48000);
        const double early = Measure(a.left, 440.0, kRate, at(0.005), at(0.05));
        const double late = Measure(a.left, 314.0, kRate, at(0.5), at(0.54));
        Check(std::fabs(Cents(early, 440.0)) < 10.0 && late < 330.0 && late > 290.0,
              "handheld: the sweep down does not lower the pitch (" + std::to_string(early) + ", " + std::to_string(late) + " Hz)");
        c[kSweep] = 7;
        const Audio off = Play(*Fresh(c), { On(0, 69) }, 9600);
        const double reference = AcRms(Play(*Fresh(Plain(0)), { On(0, 69) }, 9600).left, at(0.1), at(0.2));
        // What is left is the DAC's step at the trigger, through the 5 Hz blocker.
        Check(AcRms(off.left, at(0.1), at(0.2)) < 0.05 * reference, "handheld: the sweep's overflow does not turn the channel off");
        Controls n = Plain(1);
        n[kSweep] = 2;
        n[kSweepPace] = 1;
        const Audio up = Play(*Fresh(n), { On(0, 69) }, 96000);
        const double after = Measure(up.left, 554.0, kRate, at(0.13), at(0.15));
        Check(after > 500.0, "console: the sweep up does not raise the pitch (" + std::to_string(after) + " Hz)");
        Check(AcRms(up.left, at(1.5), at(1.9)) < 1.0e-3, "console: the swept pulse is not muted under period 8");
    }

    // Sides: pulse 1 on the left only, pulse 2 on the right only (NR51
    // $DE: bit 0 and bit 5 clear), and every channel back on both once off.
    {
        Controls c = Plain(0);
        c[kBoards] = 1;
        c[kSides] = 1;
        const auto e = Fresh(c);
        const Audio one = Play(*e, { On(0, 69) }, 9600);
        Check(AcRms(one.right, at(0.1), at(0.2)) < 0.01 * AcRms(one.left, at(0.1), at(0.2)),
              "handheld: Spread does not put pulse 1 on the left alone");
        Play(*e, { On(0, 76) }, 9600);
        Check(e->MachineAt(0).HandheldBoard().Chip().Panning() == 0xDE, "handheld: Spread's NR51 is not $DE");
        c[kSides] = 0;
        e->SetControls(c);
        // The sides' 5 Hz blockers still settle from what each had before.
        const Audio both = Play(*e, {}, 9600);
        const double l = AcRms(both.left, at(0.1), at(0.2)), r = AcRms(both.right, at(0.1), at(0.2));
        Check(e->MachineAt(0).HandheldBoard().Chip().Panning() == 0xFF && std::fabs(Db(l / r)) < 0.05,
              "handheld: Sides back to both does not reach the held notes");
    }
}

// Load: Line into headphones adds the socket's 100 uF into 32 Ohm, a
// high-pass at 49.7 Hz, which takes C2's fundamental (65.4 Hz) to 0.796 of
// what a line input gets; Pin has no socket.
void TestLoad() {
    const auto fundamental = [](const std::vector<float>& x, double hz) {
        double re = 0.0, im = 0.0;
        const std::size_t from = 24000, to = 48000;
        for (std::size_t i = from; i < to; ++i) {
            const double phase = 6.283185307179586 * hz * static_cast<double>(i) / 48000.0;
            re += x[i] * std::cos(phase);
            im += x[i] * std::sin(phase);
        }
        return std::hypot(re, im);
    };
    const double hz = 440.0 * std::pow(2.0, (36 - 69) / 12.0);
    for (int path : { 1, 0 }) {
        Controls c = Plain(0);
        c[kPath] = path;
        const Audio line = Play(*Fresh(c), { On(0, 36) }, 48000);
        c[kLoad] = 1;
        const Audio phones = Play(*Fresh(c), { On(0, 36) }, 48000);
        const double ratio = fundamental(phones.left, hz) / fundamental(line.left, hz);
        const double expect = path == 1 ? hz / std::hypot(hz, 1.0 / (2.0 * 3.141592653589793 * 100e-6 * 32.0)) : 1.0;
        Check(std::fabs(ratio - expect) < 0.01, std::string("load: headphones take C2's fundamental to ") + std::to_string(ratio)
              + " of a line input's on " + (path == 1 ? "Line" : "Pin") + ", not " + std::to_string(expect));
    }
}

// The cartridge chips' own envelopes: the disk unit's (expansion D) from
// the Level's gain, up to 32 or down to 0; the 5B's (expansion E) from its
// top, shared by its three channels.
void TestCartridgeUnits() {
    constexpr double kRate = 48000.0;
    constexpr double kCpu = 1789773.0;
    const auto at = [&](double seconds) { return static_cast<std::size_t>(seconds * kRate); };
    const auto level = [&](const Audio& a, double seconds) { return AcRms(a.left, at(seconds), at(seconds) + 240); };

    // The disk unit: Decay 111 ms from gain 32 to Sustain 10 is 10.7 steps
    // of 10.4 ms, speed 9 (pace 10, 8 x 10 x 233 cycles), so 9 steps at
    // 0.1 s; at Level 6 it starts from gain 13.
    {
        Controls c = Plain(1, 3);
        c[kRevision] = 1; // the Famicom: the cartridge's sound reaches the mix
        c[kExpansion] = 4;
        const double reference = level(Play(*Fresh(c), { On(0, 69) }, 48000), 0.5);
        c[kSustain] = 10;
        c[kDecay] = 111;
        c[kEnvelope] = 1;
        const Audio a = Play(*Fresh(c), { On(0, 69) }, 48000);
        Check(std::fabs(level(a, 0.1) / reference - 23.0 / 32.0) < 1.6 / 32.0,
              "disk unit: its envelope is not at gain 23 (" + std::to_string(32.0 * level(a, 0.1) / reference) + ")");
        Check(level(a, 0.5) < 0.02 * reference, "disk unit: its envelope does not run on to 0 under the held key");
        c[kLevel] = 6;
        const Audio low = Play(*Fresh(c), { On(0, 69) }, 4800);
        Check(std::fabs(level(low, 0.003) / reference - 13.0 / 32.0) < 1.1 / 32.0,
              "disk unit: its envelope does not start from the Level's gain (" + std::to_string(32.0 * level(low, 0.003) / reference) + ")");
        // A climb to 32 whatever the Level, and the release from there.
        c[kAttack] = 200;
        c[kRelease] = 200;
        const auto e = Fresh(c);
        const Audio up = Play(*e, { On(0, 69), Off(21600, 69) }, 24000);
        Check(level(up, 0.03) < 0.5 * reference && std::fabs(level(up, 0.4) / reference - 1.0) < 0.05,
              "disk unit: its envelope does not climb to 32");
        Play(*e, {}, 24000);
        Check(e->Sounding() == 0, "disk unit: the climbed note does not end after its release");
        c[kAttack] = 0;
        c[kLevel] = 15;
        Check(Play(*Fresh(c), Phrase(), 48000, 256).left == Play(*Fresh(c), Phrase(), 48000, 61).left,
              "disk unit: its envelope differs with the block size");
    }

    // The 5B: Decay 268 ms over its 15 volume steps is period 1000, 16000
    // cycles an envelope step, 1.5 dB each on its DAC: 11 steps at 0.1 s.
    // It starts from the top at any Level.
    {
        Controls c = Plain(1, 3);
        c[kRevision] = 1; // the Famicom: the cartridge's sound reaches the mix
        c[kExpansion] = 5;
        const double reference = level(Play(*Fresh(c), { On(0, 69) }, 48000), 0.5);
        c[kSustain] = 0;
        c[kDecay] = 1000.0 * 32.0 * 15.0 / kCpu * 1000.0;
        c[kEnvelope] = 1;
        const Audio a = Play(*Fresh(c), { On(0, 69) }, 48000);
        Check(std::fabs(Db(level(a, 0.1) / reference) + 16.5) < 1.5,
              "5B: its envelope is not 11 steps down at 0.1 s (" + std::to_string(Db(level(a, 0.1) / reference)) + " dB)");
        Check(level(a, 0.5) < 0.02 * reference, "5B: its envelope does not run on to 0 under the held key");
        c[kLevel] = 6;
        Check(level(Play(*Fresh(c), { On(0, 69) }, 4800), 0.003) > 0.8 * reference, "5B: its envelope does not start from the top");
        c[kLevel] = 15;
        Check(Play(*Fresh(c), Phrase(), 48000, 256).left == Play(*Fresh(c), Phrase(), 48000, 61).left,
              "5B: its envelope differs with the block size");

        // One envelope for three channels: the second note takes it, and the
        // first goes on under the driver from where it was (5 steps down).
        const auto e = Fresh(c);
        Play(*e, { On(0, 69), On(4800, 76) }, 4900);
        const ay::Chip& chip = e->MachineAt(0).ConsoleBoard().S5B();
        int enveloped = 0, held = -1;
        for (int k = 0; k < 3; ++k) {
            if (chip.Register(8 + k) & 0x10)
                ++enveloped;
            else if (chip.Register(8 + k) != 0)
                held = chip.Register(8 + k);
        }
        Check(enveloped == 1 && held >= 9 && held <= 10,
              "5B: the newest note does not take the one envelope from the held one (" + std::to_string(enveloped) + ", " + std::to_string(held) + ")");
    }
}

// A held note takes a new wave: the handheld's wave channel, which starts
// again to take it, and the cartridge chips C and D, which take it as it is
// written.
void TestHeldWaves() {
    struct Setup { const char* name; int board, revision, expansion, part; };
    const Setup setups[] = { { "handheld wave", 0, 0, 0, 1 }, { "expansion C", 1, 1, 3, 3 }, { "expansion D", 1, 1, 4, 3 } };
    for (const Setup& u : setups) {
        Controls c = Plain(u.board, u.part, 0);
        c[kRevision] = u.revision;
        c[kExpansion] = u.expansion;
        Controls changed = c;
        changed[kShape] = 5;
        const Audio r = Play(*Fresh(c), { On(0, 69) }, 48000);
        const Audio t = Play(*Fresh(changed), { On(0, 69) }, 48000);
        const auto e = Fresh(c);
        const Audio first = Play(*e, { On(0, 69) }, 24000);
        e->SetControls(changed);
        const Audio second = Play(*e, {}, 24000);
        double difference = 0.0;
        for (std::size_t i = 6000; i < 24000; ++i) {
            const double d = second.left[i] - r.left[24000 + i];
            difference += d * d;
        }
        difference = std::sqrt(difference / 18000.0);
        const double level = Rms(second.left, 6000, 24000), want = Rms(t.left, 30000, 48000);
        Check(difference > 0.3 * Rms(r.left, 30000, 48000) && std::fabs(Db(level / want)) < 0.5,
              std::string(u.name) + ": a held note does not take the new wave");
    }
}

void TestTouch() {
    // Velocity 1 scales the level by the key's velocity: 64 against 127 is
    // the volume step round(15 x 64 / 127) = 8 against 15.
    Controls c = Plain(0);
    c[kVelocity] = 1;
    const auto loud = Fresh(c), soft = Fresh(c);
    const Audio a = Play(*loud, { On(0, 69, 127) }, 24000);
    const Audio b = Play(*soft, { On(0, 69, 64) }, 24000);
    const double ratio = Rms(b.left, 6000, 24000) / Rms(a.left, 6000, 24000);
    Check(std::fabs(ratio - 8.0 / 15.0) < 0.005, "velocity: " + std::to_string(ratio) + " not 8 / 15");

    // The wheel's bend at full is the bend range.
    c[kVelocity] = 0;
    c[kBendRange] = 7;
    const auto e = Fresh(c);
    const Audio d = Play(*e, { Bend(0, 1.0), On(0, 62) }, 24000);
    const double a4 = 131072.0 / 298.0;   // D4 bent up a fifth
    Check(std::fabs(Cents(Measure(d.left, a4, 48000.0, 6000, 24000), a4)) < 1.0, "bend: a full bend is not its range");

    // The host's master tuning reaches a held note: A4 at +50 cents, then
    // -30. Out of range it stops at 100 cents, and NaN plays at pitch. Each
    // is heard at the pulse's nearest period, 131072 / (2048 - x) Hz.
    c[kBendRange] = 2;
    const auto t = Fresh(c);
    const auto heard = [](double cents) {
        const double hz = 440.0 * std::pow(2.0, cents / 1200.0);
        return 131072.0 / std::round(131072.0 / hz);
    };
    const auto at = [&](double cents, const std::vector<Event>& ev) {
        const Audio played = Play(*t, ev, 24000);
        return std::fabs(Cents(Measure(played.left, heard(cents), 48000.0, 6000, 24000), heard(cents))) < 1.0;
    };
    t->SetTune(50.0);
    Check(at(50.0, { On(0, 69) }), "tune: +50 cents does not reach A4");
    t->SetTune(-30.0);
    Check(at(-30.0, {}), "tune: -30 cents does not reach a held A4");
    t->SetTune(1.0e9);
    Check(at(100.0, {}), "tune: past 100 cents is not 100");
    t->SetTune(std::nan(""));
    Check(at(0.0, {}), "tune: NaN is not at pitch");
}

void TestBounds() {
    // Every board at its loudest: eight machines, every channel, held.
    for (int board = 0; board < 2; ++board) {
        Controls c = Plain(board);
        c[kBoards] = 8;
        c[kVolume] = 6;
        c[kRevision] = board == 1 ? 1 : 0;
        c[kExpansion] = board == 1 ? 4 : 0;
        const auto e = Fresh(c);
        std::vector<Event> ev;
        for (int k = 0; k < 40; ++k)
            ev.push_back(On(k, 36 + k * 2, 127));
        const Audio a = Play(*e, ev, 48000);
        Check(Finite(a) && Peak(a.left) <= kCeiling && Peak(a.right) <= kCeiling, "board " + std::to_string(board) + " over the ceiling");
    }

    // Controls out of range, NaN included, land in range.
    {
        Controls c;
        for (int i = 0; i < kControlCount; ++i)
            c[i] = i % 3 == 0 ? std::nan("") : i % 3 == 1 ? 1.0e300 : -1.0e300;
        const auto e = Fresh(c);
        std::vector<Event> ev;
        for (int k = 0; k < 128; k += 5)
            ev.push_back(On(k, k, 127));
        ev.push_back(Bend(100, 1.0));
        ev.push_back(Cc(200, 1, 127));
        const Audio a = Play(*e, ev, 24000);
        Check(Finite(a) && Peak(a.left) <= kCeiling, "wild controls are not taken into range");
        for (int i = 0; i < kControlCount; ++i) {
            const ControlInfo& info = kControlInfo[static_cast<std::size_t>(i)];
            const double v = e->GetControls()[i];
            Check(v >= info.min && v <= info.max, std::string(info.id) + " out of its range");
        }
    }

    // Changing the board, the expansion or the machines while keys are held
    // plays them again on the new machines.
    {
        Controls c = Plain(0);
        const auto e = Fresh(c);
        Play(*e, { On(0, 60), On(0, 67) }, 4800);
        for (int board : { 1, 0 }) {
            c[kBoard] = board;
            e->SetControls(c);
            const Audio a = Play(*e, {}, 9600);
            Check(e->Sounding() == 2 && Rms(a.left, 4800, 9600) > 0.01, "a board change drops the held keys");
        }
    }

    // MIDI the engine cannot use is ignored; short messages too.
    {
        const auto e = Fresh(Plain(0));
        const std::uint8_t junk[][3] = { { 0x90, 60, 0 }, { 0xF8, 0, 0 }, { 0xA0, 60, 40 }, { 0xC0, 5, 0 }, { 0xD0, 9, 0 } };
        for (const auto& m : junk)
            e->Midi(m, 3);
        const std::uint8_t one[1] = { 0x90 };
        e->Midi(one, 1);
        e->Midi(one, 0);
        Play(*e, {}, 512);
        Check(e->Sounding() == 0, "ignored MIDI starts a note");
    }
}

void TestAuthenticityEnhancements() {
    // 1. Handheld CH3 sub-32 Hz NR32 timer ISR (A0 = 27.5 Hz, B0 = 30.87 Hz).
    {
        Controls c = Plain(0, 1, 2);
        auto e = Fresh(c, 48000.0);
        for (int midi : { 21, 23 }) {
            e->Reset();
            const double hz = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
            const Audio a = Play(*e, { On(100, midi) }, 144000);
            const double got = Measure(a.left, hz, 48000.0, 24000, 144000);
            Check(got > 0.0 && std::fabs(Cents(got, hz)) < 15.0,
                  "handheld sub-32 Hz CH3 NR32 ISR MIDI " + std::to_string(midi) + ": got " + std::to_string(got) + " Hz");
        }
    }

    // 2. Handheld < 64 Hz Pulse fallback onto CH3 preserves Pulse duty shapes.
    {
        auto renderPulse = [](int shape) {
            auto e = Fresh(Plain(0, 0, shape), 48000.0);
            return Play(*e, { On(100, 28) }, 48000); // E1 (~41.20 Hz)
        };
        const Audio a0 = renderPulse(0); // 12.5 % duty
        const Audio a2 = renderPulse(2); // 50 % duty
        const double hz = 440.0 * std::pow(2.0, (28 - 69) / 12.0);
        const double f0 = Measure(a0.left, hz, 48000.0, 12000, 48000);
        const double f2 = Measure(a2.left, hz, 48000.0, 12000, 48000);
        Check(Rms(a0.left, 12000, 48000) > 0.01 && Rms(a2.left, 12000, 48000) > 0.01,
              "handheld < 64 Hz Pulse fallback sounds across duty shapes");
        Check(f0 > 0.0 && std::fabs(Cents(f0, hz)) < 10.0 && f2 > 0.0 && std::fabs(Cents(f2, hz)) < 10.0,
              "handheld < 64 Hz Pulse fallback plays in tune");
        double maxDiff = 0.0;
        for (std::size_t i = 12000; i < 48000; ++i)
            maxDiff = std::max(maxDiff, std::fabs(static_cast<double>(a0.left[i] - a2.left[i])));
        Check(maxDiff > 0.02, "handheld < 64 Hz Pulse fallback preserves Shape duty difference");
    }

    // 3. Console high-octave FDS and 8-ch Namco 163 fold wavetables on chip up to C8
    // without falling back to the APU Triangle (whose resting Out(2) is 15).
    {
        auto checkHigh = [](int expansion, int expChannels, int midi, const char* label) {
            Controls c = Plain(1, 3, 2);
            c[kRevision] = 1; // Famicom
            c[kExpansion] = expansion;
            c[kExpansionChannels] = expChannels;
            auto e = Fresh(c, 48000.0, 4);
            e->Midi(On(0, midi).midi.data(), 3);
            std::vector<float> bufL(48000, 0.0f);
            std::vector<float> bufR(48000, 0.0f);
            bool triangleQuiet = true;
            for (int i = 0; i < 48000; i += 256) {
                const int n = std::min(256, 48000 - i);
                e->Process(bufL.data() + i, bufR.data() + i, n);
                if (e->MachineAt(0).ConsoleBoard().Apu().Out(2) != 15)
                    triangleQuiet = false;
            }
            const double hz = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
            const double got = Measure(bufL, hz, 48000.0, 8000, 48000);
            Check(triangleQuiet, std::string(label) + " stays on cartridge chip (APU Triangle untouched)");
            Check(got > 0.0 && std::fabs(Cents(got, hz)) < 15.0,
                  std::string(label) + " plays in tune: got " + std::to_string(got) + " Hz");
        };
        checkHigh(4, 1, 96, "FDS C7");
        checkHigh(4, 1, 108, "FDS C8");
        checkHigh(3, 8, 108, "8-ch N163 C8");
    }

    // 4. Native FDS (0..32) and VRC6 Saw (0..42) volume resolution during release.
    {
        auto countSteps = [](int expansion, int part, int shape) {
            Controls c = Plain(1, part, shape);
            c[kRevision] = 1; // Famicom
            c[kExpansion] = expansion;
            c[kBoards] = 1;
            c[kAttack] = 0.0;
            c[kDecay] = 0.0;
            c[kSustain] = 15;
            c[kRelease] = 2000.0;
            auto e = Fresh(c, 48000.0);
            const Audio a = Play(*e, { On(0, 69), Off(4800, 69) }, 96000);
            const double full = Rms(a.left, 1600, 4656); // ~14 cycles of 440 Hz
            int steps = 0;
            double last = full;
            for (std::size_t off = 5600; off + 764 <= 92000; off += 800) {
                const double r = Rms(a.left, off, off + 764);
                if (last - r > 0.005 * full) {
                    ++steps;
                    last = r;
                }
            }
            return steps;
        };
        const int fdsSteps = countSteps(4, 3, 2);
        const int vrc6SawSteps = countSteps(1, 4, 0);
        Check(fdsSteps > 20, "FDS native 0..32 volume steps exceed 4-bit count: " + std::to_string(fdsSteps));
        Check(vrc6SawSteps > 25, "VRC6 Saw native 0..42 volume steps exceed 4-bit count: " + std::to_string(vrc6SawSteps));
    }
}

} // namespace

int main() {
    TestPartitionAndDeterminism();
    TestTrimAutomationPartition();
    TestPresets();
    TestReferenceLevel();
    TestRates();
    TestKeyboard();
    TestOversampling();
    TestDouble();
    TestVoices();
    TestModes();
    TestLegatoRange();
    TestPolyRange();
    TestInactiveMachineControls();
    TestHeldPitchRecovery();
    TestArpRange();
    TestArpChordEdits();
    TestPedalAndPanic();
    TestReset();
    TestEnvelope();
    TestChipUnits();
    TestCartridgeUnits();
    TestLoad();
    TestHeldWaves();
    TestTouch();
    TestBounds();
    TestAuthenticityEnhancements();
    if (failures == 0)
        std::printf("Reboot.Engine: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
