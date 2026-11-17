// Each board's parts in tune and at their levels, rendered through the
// board as the engine renders it and measured from the samples: a pitch
// against the formula its chip's documents give for the period the board
// chose, a level against the DAC and mixer figures the boards cite.

#include "DSP/Driver/Machine.h"

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

constexpr int kRate = 48000;
constexpr double kNtscCpu = 236250000.0 / 11.0 / 12.0;    // NESdev, Cycle reference chart
constexpr double kPalCpu = 26601712.5 / 16.0;

double KeyHz(int key) { return 440.0 * std::pow(2.0, (key - 69) / 12.0); }
double Cents(double a, double b) { return 1200.0 * std::log2(a / b); }

struct Setup {
    const char* name;
    MachineSettings settings;
};

const std::vector<Setup>& Setups() {
    static const std::vector<Setup> list = {
        { "handheld", { BoardKind::Handheld, 0, 0, 4, 0 } },
        { "handheld color", { BoardKind::Handheld, 1, 0, 4, 0 } },
        { "console", { BoardKind::Console, 0, 0, 4, 0 } },
        { "console pal", { BoardKind::Console, 2, 0, 4, 0 } },
        { "console vrc6", { BoardKind::Console, 1, Console::kVrc6, 4, 0 } },
        { "console mmc5", { BoardKind::Console, 1, Console::kMmc5, 4, 0 } },
        { "console n163 x1", { BoardKind::Console, 1, Console::kN163, 1, 0 } },
        { "console n163 x8", { BoardKind::Console, 1, Console::kN163, 8, 0 } },
        { "console fds", { BoardKind::Console, 1, Console::kFds, 4, 0 } },
        { "console 5b", { BoardKind::Console, 1, Console::k5B, 4, 0 } },
        { "psg sega", { BoardKind::Psg, 0, 0, 4, 0 } },
        { "psg sega handheld", { BoardKind::Psg, 1, 0, 4, 0 } },
        { "psg ti", { BoardKind::Psg, 2, 0, 4, 0 } },
        { "micro 128k", { BoardKind::Micro, 0, 0, 4, 0 } },
        { "micro 48k", { BoardKind::Micro, 1, 0, 4, 0 } },
        { "micro 48k issue 2", { BoardKind::Micro, 2, 0, 4, 0 } },
    };
    return list;
}

// The frequency a period plays, from the documents (each board's PeriodFor
// cites the same formulas; these are written out again on purpose).
double Expected(const MachineSettings& s, Voice voice, int channel, std::int64_t p, int shape) {
    const double period = static_cast<double>(p);
    switch (s.board) {
    case BoardKind::Handheld:
        // Pan Docs: 131072 / (2048 - x), the wave channel 65536 / (2048 - x).
        return (voice == Voice::Wave ? 65536.0 : 131072.0) / period;
    case BoardKind::Console: {
        const double cpu = s.revision == Console::kPal ? kPalCpu : kNtscCpu;
        switch (voice) {
        case Voice::Triangle: return cpu / (32.0 * (period + 1.0));     // APU Triangle
        case Voice::Saw: return cpu / (14.0 * (period + 1.0));          // VRC6 audio
        case Voice::Pulse: return cpu / (16.0 * (period + 1.0));        // APU Pulse, VRC6, MMC5
        case Voice::Wave: {                                             // Namco 163 audio
            const int n = s.expansionChannels;
            const double length = 24 * n <= 128 ? 32.0 : 16.0;
            return cpu * period / (15.0 * n * 65536.0 * length);
        }
        case Voice::ModWave: return cpu * period / 4194304.0;           // FDS audio
        default: return cpu / (32.0 * period);                         // Sunsoft 5B audio
        }
    }
    case BoardKind::Psg: {
        // SMS Power: clock / (32 N); the periodic noise on tone 3 cycles
        // every 16 shifts (Sega) or 15 (TI), each two tone-3 toggles.
        const double clock = 3579545.0;
        if (voice == Voice::Bass)
            return clock / (32.0 * (s.revision == Psg::kTiComputer ? 15.0 : 16.0) * period);
        return clock / (32.0 * period);
    }
    default: {
        const double cpu = s.revision == Micro::k128K ? 3546900.0 : 3500000.0;
        if (channel == Micro::kBeeper)
            return cpu / (2.0 * (4.0 * period + 118.0));               // the ROM's BEEPER loop
        if (voice == Voice::Buzzer)                                     // 32 envelope steps of 16 EP
            return cpu / ((shape & 2 ? 1024.0 : 512.0) * period);
        return cpu / (32.0 * period);                                   // AY at CPU / 2: f / (16 TP)
    }
    }
}

struct Render {
    std::vector<float> left, right;
};

Render Play(Machine& m, const MachineSettings& s, int channel, const ChannelSettings& note, double seconds, Path path,
            int rate = kRate) {
    m.Prepare(rate);
    m.Reset(s);
    m.Start(channel, note);
    Render r;
    const int total = static_cast<int>(seconds * rate);
    r.left.assign(static_cast<std::size_t>(total), 0.0f);
    r.right.assign(static_cast<std::size_t>(total), 0.0f);
    for (int done = 0; done < total;) {
        const int n = total - done < 256 ? total - done : 256;
        m.Render(n, path, r.left.data() + done, r.right.data() + done);
        done += n;
    }
    return r;
}

// The fundamental's frequency: two band-passes at the expected frequency
// leave a near-sine of whatever the signal's true period is, whose upward
// zero crossings are timed by interpolation across the samples.
double Measure(const std::vector<float>& x, double around, int rate, double skipSeconds) {
    struct Biquad {
        double b0, b2, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        double Process(double in) {
            const double y = b0 * in + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1, x1 = in, y2 = y1, y1 = y;
            return y;
        }
    };
    const double w = 2.0 * 3.14159265358979323846 * around / rate;
    const double alpha = std::sin(w) / (2.0 * 3.0);
    const double a0 = 1.0 + alpha;
    Biquad f[2];
    for (Biquad& b : f) {
        b.b0 = alpha / a0;
        b.b2 = -alpha / a0;
        b.a1 = -2.0 * std::cos(w) / a0;
        b.a2 = (1.0 - alpha) / a0;
    }
    double previous = 0.0, first = -1.0, last = -1.0;
    int crossings = 0;
    const auto skip = static_cast<std::size_t>(skipSeconds * rate);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double y = f[1].Process(f[0].Process(x[i]));
        if (i > skip && previous < 0.0 && y >= 0.0) {
            const double t = static_cast<double>(i) - y / (y - previous);
            if (first < 0.0)
                first = t;
            last = t;
            ++crossings;
        }
        previous = y;
    }
    if (crossings < 20)
        return 0.0;
    return (crossings - 1) * rate / (last - first);
}

double Rms(const std::vector<float>& x, double skipSeconds, int rate = kRate) {
    const auto skip = static_cast<std::size_t>(skipSeconds * rate);
    double mean = 0.0;
    for (std::size_t i = skip; i < x.size(); ++i)
        mean += x[i];
    mean /= static_cast<double>(x.size() - skip);
    double sum = 0.0;
    for (std::size_t i = skip; i < x.size(); ++i)
        sum += (x[i] - mean) * (x[i] - mean);
    return std::sqrt(sum / static_cast<double>(x.size() - skip));
}

double Db(double ratio) { return 20.0 * std::log10(ratio); }

void TestPitch(Machine& m) {
    for (const Setup& setup : Setups()) {
        m.Prepare(kRate);
        m.Reset(setup.settings);
        std::vector<Part> parts;
        for (int i = 0; i < Machine::kParts; ++i)
            if (m.PartOf(i).count > 0)
                parts.push_back(m.PartOf(i));
        if (setup.settings.board == BoardKind::Psg)
            parts.push_back(m.FallbackOf(m.PartOf(0)));
        for (const Part& part : parts) {
            if (part.voice == Voice::Noise)
                continue;
            const int shapes = m.Shapes(part.first, part.voice);
            for (int shape = 0; shape < shapes; shape += shapes > 2 ? shapes - 1 : 1) {
                int measured = 0;
                for (int key = 12; key <= 120; ++key) {
                    const double hz = KeyHz(key);
                    std::int64_t period = 0;
                    if (!m.PeriodFor(part.first, part.voice, hz, shape, period))
                        continue;
                    const std::string what = std::string(setup.name) + " channel " + std::to_string(part.first) + " shape "
                                             + std::to_string(shape) + " key " + std::to_string(key);
                    const double f = Expected(setup.settings, part.voice, part.first, period, shape);
                    // The period is the nearest whole one in pitch.
                    const double up = Expected(setup.settings, part.voice, part.first, period + 1, shape);
                    const double down = Expected(setup.settings, part.voice, part.first, period - 1, shape);
                    Check(std::fabs(Cents(f, hz)) <= std::fabs(Cents(up, hz)) + 1.0e-9
                              && std::fabs(Cents(f, hz)) <= std::fabs(Cents(down, hz)) + 1.0e-9,
                          what + ": the period is not the nearest");
                    if (key % 12 != 9 || f < 25.0 || f > 9000.0)
                        continue;
                    ++measured;
                    ChannelSettings note;
                    note.voice = part.voice;
                    note.period = period;
                    note.volume = 15;
                    note.shape = shape;
                    const Render r = Play(m, setup.settings, part.first, note, f < 60.0 ? 2.5 : 1.2, Path::Pin);
                    const double got = Measure(r.left, f, kRate, 0.3);
                    Check(got > 0.0 && std::fabs(Cents(got, f)) < 0.5,
                          what + ": plays " + std::to_string(got) + " Hz, the documents give " + std::to_string(f));
                }
                Check(measured >= 3, std::string(setup.name) + " channel " + std::to_string(part.first)
                                         + ": too few notes measured (" + std::to_string(measured) + ")");
            }
        }
    }
}

// The lowest notes each part reaches, as the boards' notes say.
void TestRanges(Machine& m) {
    struct Range {
        MachineSettings settings;
        int channel;
        Voice voice;
        double lowest;   // Hz
    };
    const std::vector<Range> ranges = {
        { { BoardKind::Handheld, 0, 0, 4, 0 }, 0, Voice::Pulse, 131072.0 / 2048.0 },   // 64 Hz
        { { BoardKind::Console, 0, 0, 4, 0 }, 0, Voice::Pulse, kNtscCpu / (16.0 * 2048.0) },
        { { BoardKind::Console, 0, 0, 4, 0 }, 2, Voice::Triangle, kNtscCpu / (32.0 * 2048.0) },
        { { BoardKind::Psg, 0, 0, 4, 0 }, 0, Voice::Tone, 3579545.0 / (32.0 * 1023.0) },
        { { BoardKind::Psg, 0, 0, 4, 0 }, 3, Voice::Bass, 3579545.0 / (32.0 * 16.0 * 1023.0) },
    };
    for (const Range& r : ranges) {
        m.Prepare(kRate);
        m.Reset(r.settings);
        std::int64_t period = 0;
        Check(m.PeriodFor(r.channel, r.voice, r.lowest * 1.003, 0, period), "a part does not reach its lowest note");
        Check(!m.PeriodFor(r.channel, r.voice, r.lowest * 0.98, 0, period), "a part reaches under its lowest note");
    }
}

ChannelSettings Note(Machine& m, int channel, Voice voice, double hz, int shape, int volume = 15) {
    ChannelSettings n;
    n.voice = voice;
    Check(m.PeriodFor(channel, voice, hz, shape, n.period), "a level note out of reach");
    n.volume = volume;
    n.shape = shape;
    return n;
}

void TestLevels(Machine& m) {
    const double pulse15 = 95.88 / (8128.0 / 15.0 + 100.0);      // NESdev, APU Mixer
    const auto level = [&](const MachineSettings& s, int channel, Voice voice, double hz, int shape, int volume = 15,
                           Path path = Path::Pin) {
        m.Prepare(kRate);
        m.Reset(s);
        const ChannelSettings n = Note(m, channel, voice, hz, shape, volume);
        return Rms(Play(m, s, channel, n, 1.0, path).left, 0.3);
    };
    const MachineSettings handheld { BoardKind::Handheld, 0, 0, 4, 0 };
    const MachineSettings console { BoardKind::Console, 0, 0, 4, 0 };
    const MachineSettings famicom { BoardKind::Console, 1, 0, 4, 0 };

    // Pan Docs: a DAC maps 0-15 to +1 to -1, NR50 at 7 passes it whole, so a
    // 50 % square at 15 swings 2 peak to peak on each side.
    Check(std::fabs(level(handheld, 0, Voice::Pulse, 440.0, 2) - 1.0) < 0.01, "handheld pulse level");
    Check(std::fabs(level(handheld, 2, Voice::Wave, 440.0, 3) - 1.0) < 0.01, "handheld wave level");

    // NESdev, APU Mixer: a pulse at 15 alone is 95.88 / (8128 / 15 + 100);
    // the triangle's 32 steps through the tnd formula.
    const double apu = level(console, 0, Voice::Pulse, 440.0, 2);
    Check(std::fabs(Db(apu / (pulse15 / 2.0))) < 0.05, "console pulse level " + std::to_string(apu));
    {
        double sum = 0.0, sum2 = 0.0;
        for (int k = 0; k < 32; ++k) {
            const int t = k < 16 ? 15 - k : k - 16;
            const double v = t > 0 ? 159.79 / (1.0 / (t / 8227.0) + 100.0) : 0.0;
            sum += v, sum2 += v * v;
        }
        const double rms = std::sqrt(sum2 / 32.0 - (sum / 32.0) * (sum / 32.0));
        const double got = level(console, 2, Voice::Triangle, 220.0, 0);
        Check(std::fabs(Db(got / rms)) < 0.1, "console triangle level " + std::to_string(got) + " against " + std::to_string(rms));
    }

    // The cartridges against the APU's pulse at 15, as Boards/Console.h
    // cites them: VRC6 and MMC5 as loud, the 5B as loud by our convention,
    // the Namco 163's loudest square 12 dB over, the disk unit's 2.4 times.
    const double famicomPulse = level(famicom, 0, Voice::Pulse, 440.0, 2);
    const auto cart = [&](int expansion, int channels, Voice voice, int shape, double hz) {
        const MachineSettings s { BoardKind::Console, 1, expansion, channels, 0 };
        return level(s, Console::kApuChannels, voice, hz, shape) / famicomPulse;
    };
    Check(std::fabs(Db(cart(Console::kVrc6, 4, Voice::Pulse, 7, 440.0))) < 0.05, "vrc6 pulse level");
    Check(std::fabs(Db(cart(Console::kMmc5, 4, Voice::Pulse, 2, 440.0))) < 0.05, "mmc5 pulse level");
    Check(std::fabs(Db(cart(Console::k5B, 4, Voice::Tone, 0, 440.0))) < 0.05, "5b tone level");
    Check(std::fabs(Db(cart(Console::kN163, 1, Voice::Wave, 3, 440.0)) - 12.0) < 0.1, "n163 square level");
    // The disk unit's own 2 kHz low-pass takes a little of a 110 Hz square.
    const double fds = cart(Console::kFds, 4, Voice::ModWave, 3, 110.0);
    Check(fds > 2.4 * 0.97 && fds < 2.4 * 1.001, "fds square level " + std::to_string(fds));

    // SMS Power: an output bit of 0 or 1, 2 dB a step of attenuation.
    for (int revision : { 0, 2 }) {
        const MachineSettings psg { BoardKind::Psg, revision, 0, 4, 0 };
        const double full = level(psg, 0, Voice::Tone, 440.0, 0);
        Check(std::fabs(full - 0.5) < 0.005, "psg tone level " + std::to_string(full));
        const double down = level(psg, 0, Voice::Tone, 440.0, 0, 13);
        Check(std::fabs(Db(down / full) + 4.0) < 0.02, "psg attenuation is not 2 dB a step");
    }

    // The micro: an AY channel at full volume and the beeper's swing at MIC
    // off are one level by our convention, on both DAC tables.
    const MachineSettings micro { BoardKind::Micro, 0, 0, 4, 0 };
    const double ayTone = level(micro, 0, Voice::Tone, 440.0, 0);
    Check(std::fabs(ayTone - 0.5) < 0.005, "ay tone level " + std::to_string(ayTone));
    const double beeper = level(micro, Micro::kBeeper, Voice::Beeper, 440.0, 0);
    Check(std::fabs(Db(beeper / ayTone)) < 0.05, "beeper level " + std::to_string(beeper));
    // World of Spectrum's pin voltages: Issue 2's swing is 3.06 V to Issue
    // 3's 3.04 V on the same scale.
    const double issue2 = level({ BoardKind::Micro, 2, 0, 4, 0 }, Micro::kBeeper, Voice::Beeper, 440.0, 0);
    Check(std::fabs(issue2 / beeper - (3.79 - 0.73) / (3.70 - 0.66)) < 0.003, "issue 2 beeper level");

    // Line: the console's stage is a high-pass at 90 Hz (front-loader) or
    // 37 Hz (Famicom), so a 60 Hz square loses more on the front-loader;
    // the handheld's capacitor passes 440 Hz nearly whole; the PSG and the
    // micro have no line stage, so Line is Pin.
    const double front = level(console, 2, Voice::Triangle, 60.0, 0, 15, Path::Line)
                         / level(console, 2, Voice::Triangle, 60.0, 0, 15, Path::Pin);
    const double fami = level(famicom, 2, Voice::Triangle, 60.0, 0, 15, Path::Line)
                        / level(famicom, 2, Voice::Triangle, 60.0, 0, 15, Path::Pin);
    Check(front < fami && fami < 1.0 && front > 0.5, "console line stages " + std::to_string(front) + " " + std::to_string(fami));
    const double cap = level(handheld, 0, Voice::Pulse, 440.0, 2, 15, Path::Line) / level(handheld, 0, Voice::Pulse, 440.0, 2);
    Check(cap > 0.97 && cap < 1.01, "handheld capacitor at 440 Hz " + std::to_string(cap));
    for (const MachineSettings& s : { MachineSettings { BoardKind::Psg, 0, 0, 4, 0 }, micro }) {
        m.Prepare(kRate);
        m.Reset(s);
        const ChannelSettings n = Note(m, 0, Voice::Tone, 330.0, 0);
        const Render pin = Play(m, s, 0, n, 0.5, Path::Pin);
        const Render line = Play(m, s, 0, n, 0.5, Path::Line);
        Check(pin.left == line.left, "a board without a line stage renders Line differently");
    }

    // Stereo: the handheld and the PSG send every channel to both sides.
    {
        m.Prepare(kRate);
        m.Reset(handheld);
        const Render r = Play(m, handheld, 1, Note(m, 1, Voice::Pulse, 523.0, 1), 0.5, Path::Pin);
        Check(r.left == r.right, "the handheld's sides differ");
    }
}

// Every noise part sounds, and its periodic or short mode too.
void TestNoise(Machine& m) {
    for (const Setup& setup : Setups()) {
        m.Prepare(kRate);
        m.Reset(setup.settings);
        for (int i = 0; i < Machine::kParts; ++i) {
            const Part part = m.PartOf(i);
            if (part.count == 0 || part.voice != Voice::Noise)
                continue;
            for (int shape = 0; shape < m.Shapes(part.first, part.voice); ++shape)
                for (int key : { 36, 60, 84 }) {
                    m.Prepare(kRate);
                    m.Reset(setup.settings);
                    const ChannelSettings n = Note(m, part.first, part.voice, KeyHz(key), shape);
                    const double rms = Rms(Play(m, setup.settings, part.first, n, 0.6, Path::Pin).left, 0.2);
                    Check(std::isfinite(rms) && rms > 0.01, std::string(setup.name) + " noise shape " + std::to_string(shape)
                                                                + " key " + std::to_string(key) + " is silent");
                }
        }
    }
}

// A board stopped goes quiet, and the same note at another host rate plays
// the same pitch.
void TestStopAndRates(Machine& m) {
    for (const Setup& setup : Setups()) {
        m.Prepare(kRate);
        m.Reset(setup.settings);
        const Part part = m.PartOf(setup.settings.board == BoardKind::Micro && !m.MicroBoard().HasAy() ? 3 : 0);
        ChannelSettings n = Note(m, part.first, part.voice, 440.0, 1);
        Render r = Play(m, setup.settings, part.first, n, 0.5, Path::Pin);
        m.Stop(part.first);
        std::vector<float> tail(48000, 0.0f), right(48000, 0.0f);
        for (int done = 0; done < 48000; done += 256)
            m.Render(256 < 48000 - done ? 256 : 48000 - done, Path::Pin, tail.data() + done, right.data() + done);
        double late = 0.0;
        for (std::size_t i = 24000; i < tail.size(); ++i)
            late = std::fmax(late, std::fabs(tail[i]));
        Check(late < 1.0e-3, std::string(setup.name) + ": still sounding half a second after Stop (" + std::to_string(late) + ")");

        const double f = Expected(setup.settings, part.voice, part.first, n.period, 1);
        for (int rate : { 44100, 96000, 192000 }) {
            m.Prepare(rate);
            m.Reset(setup.settings);
            const Render rr = Play(m, setup.settings, part.first, n, 1.0, Path::Pin, rate);
            const double got = Measure(rr.left, f, rate, 0.3);
            Check(std::fabs(Cents(got, f)) < 0.5, std::string(setup.name) + " at " + std::to_string(rate) + " Hz plays "
                                                       + std::to_string(got) + " for " + std::to_string(f));
        }
    }
}

} // namespace

int main() {
    auto machine = std::make_unique<Machine>();
    TestPitch(*machine);
    TestRanges(*machine);
    TestLevels(*machine);
    TestNoise(*machine);
    TestStopAndRates(*machine);
    if (failures == 0)
        std::printf("Reboot.Boards: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
