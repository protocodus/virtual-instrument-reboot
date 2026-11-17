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

// The Init patch with a pin-path, full-level, unsensitive layer A.
Controls Plain(int board, int part = 0, int shape = 2) {
    Controls c;
    c[kBoard] = board;
    c[kPath] = 0;
    c[kAPart] = part;
    c[kAShape] = shape;
    c[kALevel] = 15;
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

// One channel at full volume, a 50 % square, reads -24 dBFS RMS on every
// board, which the other levels are set against (RebootEngine.h).
void TestReferenceLevel() {
    struct Case {
        const char* name;
        int board;
        int part, shape;
    };
    for (const Case& k : { Case { "handheld", 0, 0, 2 }, Case { "console", 1, 0, 2 }, Case { "psg", 2, 0, 0 },
                           Case { "micro", 3, 0, 0 } }) {
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
    for (int board = 0; board < 4; ++board) {
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
    for (std::size_t p : { 0, 4, 9, 13 }) {
        Controls c = kPresets[p].ToControls();
        if (p == 13)
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
    for (int board = 0; board < 4; ++board) {
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

// Double: a second set of machines mirrors every note, the first set on the
// left and the second on the right, each half the detune off the note.
void TestDouble() {
    // With no detune each side is one set's own sound, bit for bit, on
    // every board, through bends, vibrato, the pedal and releases.
    for (int board = 0; board < 4; ++board) {
        Controls c;
        c[kBoard] = board;
        const Audio one = Play(*Fresh(c), Phrase(), 96000);
        c[kDouble] = 1;
        c[kDetune] = 0;
        const Audio two = Play(*Fresh(c), Phrase(), 96000);
        Check(one.left == one.right, "board " + std::to_string(board) + " is not centred");
        Check(two.left == one.left && two.right == one.left,
              "board " + std::to_string(board) + ": Double with no detune is not the single set on both sides");
    }
    // A3's period on the handheld moves 2.9 cents a step, so each side sits
    // within 3 cents of its half of a 20 cent detune.
    Controls c = Plain(0);
    c[kDouble] = 1;
    c[kDetune] = 20;
    const Audio a = Play(*Fresh(c), { On(0, 57) }, 48000);
    const double a3 = 220.0;
    const double left = Measure(a.left, a3, 48000.0, 9600, 48000), right = Measure(a.right, a3, 48000.0, 9600, 48000);
    Check(std::fabs(Cents(left, a3) + 10.0) < 3.0, "Double's left plays A3 at " + std::to_string(Cents(left, a3)) + " cents");
    Check(std::fabs(Cents(right, a3) - 10.0) < 3.0, "Double's right plays A3 at " + std::to_string(Cents(right, a3)) + " cents");
    // Turned on under a held key, Double plays it on both sides; after the
    // key, both sides fall silent.
    Controls single = Plain(0);
    single[kARelease] = 0;
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
    // The PSG's bass takes tone 3 as well, so with one machine a bass and
    // two tones fill it and a third tone steals.
    {
        Controls c = Plain(2, 0, 0);
        c[kBoards] = 1;
        const auto e = Fresh(c);
        Play(*e, { On(0, 30), On(10, 72), On(20, 76) }, 2000);
        Check(e->Sounding() == 3, "psg: bass and two tones are not three notes");
        const psg::Chip& chip = e->MachineAt(0).PsgBoard().Chip();
        Check(chip.Attenuation(2) == 15 && chip.Attenuation(3) < 15, "psg: the bass does not hold tone 3 silent");
        Play(*e, { On(0, 79) }, 2000);
        Check(e->Sounding() == 3, "psg: a fourth note does not steal");
    }
    // A layer B an octave up doubles every note.
    {
        Controls c = Plain(1);
        c[kBOn] = 1;
        c[kBPart] = 1;
        c[kBTranspose] = 12;
        const auto e = Fresh(c);
        Play(*e, { On(0, 60), On(0, 64) }, 2000);
        Check(e->Sounding() == 4, "two layers do not double the notes");
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
}

void TestPedalAndPanic() {
    Controls c = Plain(0);
    c[kARelease] = 0;
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
    for (int cc : { 120, 123 }) {
        // Two frames a step: 15 steps take half a second.
        Controls slow = Plain(0);
        slow[kARelease] = 2;
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

// The envelope moves the chip's volume one step every so many frames, and
// the handheld's DAC is linear, so the level is the step over 15. A frame
// is 70224 cycles at 4194304 Hz, 803.65 samples; frame k's tick lands
// 112 samples late (the engine's latency). Windows are whole A4 periods.
void TestEnvelope() {
    constexpr double kFrame = 70224.0 / 4194304.0 * 48000.0;
    constexpr double kPeriod = 48000.0 * 298.0 / 131072.0;
    const auto window = [&](double fromFrame, int periods) {
        const auto from = static_cast<std::size_t>(fromFrame * kFrame + 112.0 + 150.0);
        return std::make_pair(from, from + static_cast<std::size_t>(periods * kPeriod));
    };
    const auto level = [&](const Audio& a, std::pair<std::size_t, std::size_t> w) { return AcRms(a.left, w.first, w.second); };
    const Audio full = Play(*Fresh(Plain(0)), { On(0, 69) }, 48000);
    const double reference = level(full, window(40.0, 40));

    // Attack 2: step k from frame 2k, full from frame 30.
    {
        Controls c = Plain(0);
        c[kAAttack] = 2;
        const Audio a = Play(*Fresh(c), { On(0, 69) }, 48000);
        Check(std::fabs(Db(level(a, window(14.0, 10)) / reference) - Db(7.0 / 15.0)) < 0.5, "attack: step 7 is not at frame 14");
        Check(std::fabs(Db(level(a, window(30.0, 40)) / reference)) < 0.2, "attack: not full from frame 30");
    }
    // Decay 3 to sustain 5: step 15 - k from frame 3k, then 5 held.
    {
        Controls c = Plain(0);
        c[kADecay] = 3;
        c[kASustain] = 5;
        const Audio a = Play(*Fresh(c), { On(0, 69) }, 48000);
        Check(std::fabs(Db(level(a, window(15.0, 15)) / reference) - Db(10.0 / 15.0)) < 0.5, "decay: step 10 is not at frame 15");
        Check(std::fabs(Db(level(a, window(40.0, 40)) / reference) - Db(5.0 / 15.0)) < 0.2, "decay: the sustain level does not hold");
    }
    // Sustain moved to 0 and back while the key holds: the note follows.
    {
        Controls c = Plain(0);
        const auto e = Fresh(c);
        Play(*e, { On(0, 69) }, 12000);
        c[kASustain] = 0;
        e->SetControls(c);
        const Audio quiet = Play(*e, {}, 12000);
        c[kASustain] = 15;
        e->SetControls(c);
        const Audio back = Play(*e, {}, 24000);
        // The level moves on the next frame, up to one after the change.
        Check(Settles(quiet.left, kLate + 804) && std::fabs(Db(level(back, { 12000, 24000 }) / reference)) < 0.2,
              "sustain: a held note does not come back from sustain 0");
    }
    // Release 4 from the key at sample 24000, inside frame 29: step 15 - k
    // from frame 29 + 4k, silent from frame 89.
    {
        Controls c = Plain(0);
        c[kARelease] = 4;
        const auto e = Fresh(c);
        const Audio a = Play(*e, { On(0, 69), Off(24000, 69) }, 72000);
        Check(std::fabs(Db(level(a, window(49.0, 25)) / reference) - Db(10.0 / 15.0)) < 0.5, "release: step 10 is not at frame 49");
        Play(*e, {}, 2400);
        Check(e->Sounding() == 0, "release: the note outlives its last step");
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
        changed[kAShape] = 5;
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
}

void TestBounds() {
    // Every board at its loudest: eight machines, every channel, held.
    for (int board = 0; board < 4; ++board) {
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
        for (int board : { 1, 2, 3, 0 }) {
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

} // namespace

int main() {
    TestPartitionAndDeterminism();
    TestPresets();
    TestReferenceLevel();
    TestRates();
    TestKeyboard();
    TestOversampling();
    TestDouble();
    TestVoices();
    TestModes();
    TestPedalAndPanic();
    TestEnvelope();
    TestHeldWaves();
    TestTouch();
    TestBounds();
    if (failures == 0)
        std::printf("Reboot.Engine: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
