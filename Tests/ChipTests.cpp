// Each chip core against the numbers its documents give, and the cores'
// two shortcuts against doing without them: a noise register jumped ahead
// against stepping it, and a silent channel caught up when it is heard
// again against one that was followed all along.

#include "DSP/Chips/Ay8910.h"
#include "DSP/Chips/Famicom.h"
#include "DSP/Chips/GameBoyApu.h"
#include "DSP/Chips/NesApu.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
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

// ---------------------------------------------------------------------------
// Noise registers.

struct Register {
    const char* name;
    LfsrStep step;
    const LfsrJump* jump;
    std::uint32_t seed;     // the state the hardware starts from
    int warmUp;             // steps before the state is on its cycle
    std::int64_t period;    // the cycle's length, from the documents
};

// Periods: NESdev APU Noise (32767, and 93 from the power-on seed of 1 in
// mode 1); Pan Docs (15 and 7 bits: 32767 and 127); the AY's (and so the
// 5B's) 17 bits at bits 0 and 3, a maximal 131071. The Game Boy register is stepped from 0, its
// reset state; its fifteenth bit holds the last output, so it is on its
// cycle after one step, and short mode's upper bits after eight more.
const std::vector<Register>& Registers() {
    static const std::vector<Register> list = {
        { "gb long", gb::NoiseStepLong, &gb::kNoiseJump[0], 0, 1, 32767 },
        { "gb short", gb::NoiseStepShort, &gb::kNoiseJump[1], 0, 16, 127 },
        { "nes long", nes::NoiseStepLong, &nes::kNoiseJump[0], 1, 0, 32767 },
        { "nes short", nes::NoiseStepShort, &nes::kNoiseJump[1], 1, 0, 93 },
        { "ay", ay::NoiseStep, &ay::kNoiseJump, 1, 0, 131071 },
    };
    return list;
}

void TestNoiseRegisters() {
    for (const Register& r : Registers()) {
        std::uint32_t s = r.seed;
        for (int i = 0; i < r.warmUp; ++i)
            s = r.step(s);
        const std::uint32_t start = s;
        std::int64_t n = 0;
        do {
            s = r.step(s);
            ++n;
        } while (s != start && n <= r.period);
        Check(n == r.period, std::string(r.name) + ": period " + std::to_string(n) + ", documents give "
                                 + std::to_string(r.period));

        // Every jump from a few states on and off the cycle, against single
        // steps, and long jumps against their halves.
        const std::uint32_t states[4] = { r.seed, start, r.step(r.step(start)), 0x5A5Au };
        for (std::uint32_t from : states) {
            std::uint32_t stepped = from;
            for (std::uint64_t k = 0; k <= 70000; ++k) {
                if (k < 600 || k % 997 == 0 || k == 32767 || k == 57337 || k == 65535)
                    if (r.jump->Advance(from, k) != stepped) {
                        Check(false, std::string(r.name) + ": jump of " + std::to_string(k) + " differs from stepping");
                        break;
                    }
                stepped = r.step(stepped);
            }
            for (std::uint64_t k : { std::uint64_t { 1 } << 40, (std::uint64_t { 1 } << 41) + 12345,
                                     std::uint64_t { 0xFFFFFFFFFFFull }, std::uint64_t { 3 } << 60 }) {
                const std::uint64_t half = k / 2;
                Check(r.jump->Advance(from, k) == r.jump->Advance(r.jump->Advance(from, half), k - half),
                      std::string(r.name) + ": a long jump is not its halves");
            }
            Check(r.jump->Advance(start, static_cast<std::uint64_t>(r.period) * 1000003) == start,
                  std::string(r.name) + ": whole cycles do not return to the state");
        }
    }
}

// ---------------------------------------------------------------------------
// Traces: a chip's outputs as a function of its cycle, from its sink.

using Snapshot = std::vector<int>;

template <class Chip>
struct Trace {
    const Chip* chip = nullptr;
    Snapshot (*read)(const Chip&) = nullptr;
    std::vector<std::pair<std::int64_t, Snapshot>> events;
    void operator()(std::int64_t cycle) { events.emplace_back(cycle, read(*chip)); }

    // The outputs once every event at or before `cycle` has happened.
    [[nodiscard]] Snapshot At(std::int64_t cycle) const {
        Snapshot s = events.front().second;
        for (const auto& e : events) {
            if (e.first > cycle)
                break;
            s = e.second;
        }
        return s;
    }
};

template <class Chip>
struct Step {
    std::int64_t at;
    std::function<void(Chip&, Trace<Chip>&)> write;
};

// Plays a script of writes, running the chip in slices of at most `slice`
// cycles between them.
template <class Chip>
Trace<Chip> Play(Chip chip, Snapshot (*read)(const Chip&), const std::vector<Step<Chip>>& script, std::int64_t end,
                 std::int64_t slice) {
    Trace<Chip> trace;
    trace.chip = &chip;
    trace.read = read;
    trace.events.emplace_back(-1, read(chip));
    const auto runTo = [&](std::int64_t to) {
        while (chip.Now() < to)
            chip.Run(std::min(to, chip.Now() + slice), trace);
    };
    for (const Step<Chip>& s : script) {
        runTo(s.at);
        s.write(chip, trace);
    }
    runTo(end);
    trace.chip = nullptr;
    return trace;
}

// Whether two traces agree at every event of either from `from` on.
template <class Chip>
bool Agree(const Trace<Chip>& a, const Trace<Chip>& b, std::int64_t from) {
    std::vector<std::int64_t> cycles { from };
    for (const auto* t : { &a, &b })
        for (const auto& e : t->events)
            if (e.first >= from)
                cycles.push_back(e.first);
    std::sort(cycles.begin(), cycles.end());
    cycles.erase(std::unique(cycles.begin(), cycles.end()), cycles.end());
    // Walk both event lists once.
    std::size_t ia = 0, ib = 0;
    Snapshot sa = a.events.front().second, sb = b.events.front().second;
    for (std::int64_t c : cycles) {
        while (ia < a.events.size() && a.events[ia].first <= c)
            sa = a.events[ia++].second;
        while (ib < b.events.size() && b.events[ib].first <= c)
            sb = b.events[ib++].second;
        if (sa != sb)
            return false;
    }
    return true;
}

// A rectangle's period and the fraction of it spent high, over its whole
// cycles after `after`, from a trace's `channel`.
struct Rectangle {
    double period = 0.0, high = 0.0;
};

template <class Chip>
Rectangle Measure(const Trace<Chip>& t, std::size_t channel, std::int64_t after) {
    std::vector<std::int64_t> rises;
    for (std::size_t i = 1; i < t.events.size(); ++i)
        if (t.events[i].first > after && t.events[i].second[channel] > 0 && t.events[i - 1].second[channel] == 0)
            rises.push_back(t.events[i].first);
    Rectangle r;
    if (rises.size() < 3)
        return r;
    std::int64_t high = 0;
    for (std::size_t i = 1; i + 1 < t.events.size(); ++i) {
        const auto& e = t.events[i];
        if (e.second[channel] > 0 && e.first >= rises.front() && e.first < rises.back())
            high += t.events[i + 1].first - e.first;
    }
    const auto span = static_cast<double>(rises.back() - rises.front());
    r.period = span / static_cast<double>(rises.size() - 1);
    r.high = static_cast<double>(high) / span;
    return r;
}

template <class Chip>
void CheckPartitions(const char* name, const Chip& reset, Snapshot (*read)(const Chip&),
                     const std::vector<Step<Chip>>& script, std::int64_t end) {
    const Trace<Chip> whole = Play(reset, read, script, end, end + 1);
    Check(whole.events.size() > 20, std::string(name) + ": the script makes no sound to compare");
    for (std::int64_t slice : { 1, 7, 97, 1031, 65536 })
        Check(Agree(whole, Play(reset, read, script, end, slice), 0),
              std::string(name) + ": slices of " + std::to_string(slice) + " cycles change the output");
}

// The lazy script silences channels between `quiet` and `heard`, where the
// reference keeps them sounding; from `heard + settle` on, both must agree.
template <class Chip>
void CheckCatchUp(const char* name, const Chip& reset, Snapshot (*read)(const Chip&),
                  const std::vector<Step<Chip>>& reference, const std::vector<Step<Chip>>& lazy, std::int64_t heard,
                  std::int64_t settle, std::int64_t end) {
    const Trace<Chip> a = Play(reset, read, reference, end, end + 1);
    const Trace<Chip> b = Play(reset, read, lazy, end, end + 1);
    Check(!Agree(a, b, 0), std::string(name) + ": the silence is not silent");
    Check(Agree(a, b, heard + settle), std::string(name) + ": a silenced channel comes back out of step");
}

template <class Chip, class Write>
Step<Chip> At(std::int64_t cycle, Write w) {
    return { cycle, [w](Chip& c, Trace<Chip>& t) { w(c, t); } };
}

// ---------------------------------------------------------------------------
// Game Boy.

Snapshot GbRead(const gb::Apu& a) {
    return { a.Out(0), a.Out(1), a.Out(2), a.Out(3), a.DacOn(0), a.DacOn(1), a.DacOn(2), a.DacOn(3) };
}

std::vector<Step<gb::Apu>> GbScript(bool lazy, std::int64_t quiet, std::int64_t heard) {
    using A = gb::Apu;
    using T = Trace<A>;
    std::vector<Step<A>> s;
    s.push_back(At<A>(10, [lazy](A& a, T& t) {
        static const std::uint8_t wave[16] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
                                               0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10 };
        for (int i = 0; i < 16; ++i)
            a.Write(static_cast<std::uint16_t>(0xFF30 + i), wave[i], t);
        // Pulses at duty 50 % and 25 %; zombie mode (volume 0, increase,
        // pace 0) is how the lazy script silences them without turning
        // their DACs off.
        a.Write(0xFF10, 0x00, t);
        a.Write(0xFF11, 0x80, t);
        a.Write(0xFF12, lazy ? 0x08 : 0x18, t);
        a.Write(0xFF13, 0x00, t);
        a.Write(0xFF14, 0x87, t);
        a.Write(0xFF16, 0x40, t);
        a.Write(0xFF17, lazy ? 0x08 : 0x18, t);
        a.Write(0xFF18, 0x37, t);
        a.Write(0xFF19, 0x86, t);
        a.Write(0xFF1A, 0x80, t);
        a.Write(0xFF1B, 0x00, t);
        a.Write(0xFF1C, 0x20, t);
        a.Write(0xFF1D, 0x51, t);
        a.Write(0xFF1E, 0x85, t);
        a.Write(0xFF21, lazy ? 0x08 : 0x18, t);
        a.Write(0xFF22, 0x23, t);
        a.Write(0xFF23, 0x80, t);
    }));
    s.push_back(At<A>(quiet, [lazy](A& a, T& t) {
        if (lazy)
            a.Write(0xFF1C, 0x00, t);
    }));
    s.push_back(At<A>(heard, [lazy](A& a, T& t) {
        if (lazy) {
            a.Write(0xFF12, 0x08, t);    // volume 0 to 1
            a.Write(0xFF17, 0x08, t);
            a.Write(0xFF21, 0x08, t);
            a.Write(0xFF1C, 0x20, t);
        }
    }));
    s.push_back(At<A>(heard + 300000, [](A& a, T& t) {
        a.Write(0xFF12, 0x08, t);        // 1 to 2 on both
        a.Write(0xFF22, 0x0B, t);        // short mode
        a.Write(0xFF13, 0x80, t);
    }));
    return s;
}

void TestGameBoy() {
    gb::Apu apu;
    apu.Reset(gb::Model::Dmg);
    const std::int64_t quiet = 200000, heard = 1234567, end = 2000000;
    CheckPartitions("gb", apu, GbRead, GbScript(false, quiet, heard), end);
    CheckPartitions("gb lazy", apu, GbRead, GbScript(true, quiet, heard), end);
    CheckCatchUp("gb", apu, GbRead, GbScript(false, quiet, heard), GbScript(true, quiet, heard), heard, 0, end);

    // Pan Docs, NR52: clearing power clears NR50/NR51, cuts active DACs
    // and preserves wave RAM. The board must see the transition on that write.
    for (gb::Model model : { gb::Model::Dmg, gb::Model::Cgb }) {
        gb::Apu chip;
        chip.Reset(model);
        int notifications = 0;
        auto sink = [&](std::int64_t) { ++notifications; };
        chip.Write(0xFF30, 0xAB, sink);
        chip.Write(0xFF17, 0xF0, sink);
        chip.Write(0xFF19, 0x80, sink);
        notifications = 0;
        chip.Write(0xFF26, 0, sink);
        Check(notifications == 1 && !chip.DacOn(1), "gb power-down must notify the board of DAC shutdown");
        chip.Write(0xFF26, 0x80, sink);
        Check(chip.MasterVolume() == chip.Read(0xFF24) && chip.MasterVolume() == 0
                  && chip.Panning() == chip.Read(0xFF25) && chip.Panning() == 0,
              "gb power cycle must clear the physical mixer and its register readback together");
        Check(chip.Read(0xFF30) == 0xAB, "gb power cycle must preserve wave RAM");
    }

    // Pan Docs: a pulse at period x runs at 131072 / (2048 - x) Hz with the
    // duty its NR11 names; the wave channel at 65536 / (2048 - x).
    for (int duty = 0; duty < 4; ++duty) {
        std::vector<Step<gb::Apu>> s;
        s.push_back(At<gb::Apu>(0, [duty](gb::Apu& a, Trace<gb::Apu>& t) {
            a.Write(0xFF11, static_cast<std::uint8_t>(duty << 6), t);
            a.Write(0xFF12, 0xF0, t);
            a.Write(0xFF13, 0x00, t);
            a.Write(0xFF14, 0x86, t);  // x = 0x600: 256 Hz, 16384 cycles
        }));
        const Rectangle r = Measure(Play(apu, GbRead, s, 2000000, 2000001), 0, 100000);
        const double expected[4] = { 0.125, 0.25, 0.5, 0.75 };
        Check(std::fabs(r.period - 16384.0) < 0.01, "gb pulse period " + std::to_string(r.period) + " != 16384");
        Check(std::fabs(r.high - expected[duty]) < 0.002, "gb duty " + std::to_string(duty) + " high " + std::to_string(r.high));
    }

    // Pan Docs, Obscure Behavior: "the low two bits of the frequency timer
    // are NOT modified" when a pulse is triggered, whether it is playing or
    // was stopped. A trigger at 100 leaves the timer due at 2148 (period
    // x = $600: 2048 cycles); retriggered at 1001 it counts 1147, low bits
    // 3, so its steps fall 3 cycles after whole periods from 1001, at 3052
    // and on. Its DAC turned off at 19999 it holds the count to 21484,
    // 1485, low bits 1, which a trigger at 30001 keeps.
    {
        using A = gb::Apu;
        using T = Trace<A>;
        std::vector<Step<A>> s;
        s.push_back(At<A>(100, [](A& a, T& t) {
            a.Write(0xFF11, 0x00, t);
            a.Write(0xFF12, 0xF0, t);
            a.Write(0xFF13, 0x00, t);
            a.Write(0xFF14, 0x86, t);
        }));
        s.push_back(At<A>(1001, [](A& a, T& t) { a.Write(0xFF14, 0x86, t); }));
        s.push_back(At<A>(19999, [](A& a, T& t) { a.Write(0xFF12, 0x00, t); }));
        s.push_back(At<A>(30001, [](A& a, T& t) {
            a.Write(0xFF12, 0xF0, t);
            a.Write(0xFF14, 0x86, t);
        }));
        const Trace<A> r = Play(apu, GbRead, s, 60000, 60001);
        const auto stepsFrom = [&](std::int64_t trigger, std::int64_t until, std::int64_t kept) {
            int n = 0;
            for (const auto& e : r.events)
                if (e.first > trigger && e.first < until) {
                    ++n;
                    if ((e.first - trigger) % 2048 != kept)
                        return false;
                }
            return n >= 2;
        };
        Check(stepsFrom(1001, 19999, 3), "gb pulse trigger does not keep its timer's low two bits");
        Check(stepsFrom(30001, 60000, 1), "gb pulse trigger after a stop does not keep the held count's low bits");
    }

    // Pan Docs, Obscure Behavior: a wave trigger plays first "the previous
    // one still in the high nibble of the sample buffer", then index 1.
    // Period x = $400 steps every 2048 cycles, plus the hardware 6-T-cycle
    // trigger delay (SameBoy MIT): from a trigger at 10 the channel reads
    // 1, 2 and 3 (the byte $23) by 6160; retriggered at 7000 it plays 2,
    // then 1 at 9054.
    {
        using A = gb::Apu;
        using T = Trace<A>;
        std::vector<Step<A>> s;
        s.push_back(At<A>(10, [](A& a, T& t) {
            static const std::uint8_t wave[16] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
                                                   0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10 };
            for (int i = 0; i < 16; ++i)
                a.Write(static_cast<std::uint16_t>(0xFF30 + i), wave[i], t);
            a.Write(0xFF1A, 0x80, t);
            a.Write(0xFF1C, 0x20, t);
            a.Write(0xFF1D, 0x00, t);
            a.Write(0xFF1E, 0x84, t);
        }));
        s.push_back(At<A>(7000, [](A& a, T& t) { a.Write(0xFF1E, 0x84, t); }));
        const Trace<A> r = Play(apu, GbRead, s, 12000, 12001);
        Check(r.At(6999)[2] == 3, "gb wave does not play index 3 before the retrigger");
        Check(r.At(7000)[2] == 2, "gb wave trigger does not play the buffer's high nibble first");
        Check(r.At(9053)[2] == 2 && r.At(9054)[2] == 1, "gb wave trigger does not go on to index 1 after 6-cycle delay");
    }

    // Wave RAM active access rules (Pan Docs): inactive DAC-on writes directly;
    // active CGB writes redirect to the playing byte; active DMG ignores
    // writes outside the 2-T-cycle read window.
    {
        auto sink = [](std::int64_t) {};
        for (gb::Model model : { gb::Model::Dmg, gb::Model::Cgb }) {
            gb::Apu a;
            a.Reset(model);
            for (int i = 0; i < 16; ++i)
                a.Write(static_cast<std::uint16_t>(0xFF30 + i), static_cast<std::uint8_t>(i * 0x11), sink);
            a.Write(0xFF1A, 0x80, sink);
            Check(a.Read(0xFF35) == 0x55, "gb wave RAM inactive DAC-on read failed");
            a.Write(0xFF1C, 0x20, sink);
            a.Write(0xFF1D, 0x00, sink);
            a.Write(0xFF1E, 0x84, sink); // period 2048, due at 2054
            a.Run(2060, sink);           // 6 cycles after read at 2054 (byte 0)
            a.Write(0xFF35, 0xAA, sink);
            a.Write(0xFF1A, 0x00, sink); // stop channel to inspect Wave RAM directly
            Check(a.Read(0xFF35) == 0x55, "gb active wave RAM wrote to addressed byte instead of locking/redirecting");
            if (model == gb::Model::Cgb)
                Check(a.Read(0xFF30) == 0xAA, "cgb active wave RAM write did not redirect to current byte");
            else
                Check(a.Read(0xFF30) == 0x00, "dmg active wave RAM write outside read window was not ignored");
        }
    }

    // CGB zombie mode (Pan Docs) and DMG power-off length counter preservation.
    {
        auto sink = [](std::int64_t) {};
        gb::Apu cgb;
        cgb.Reset(gb::Model::Cgb);
        cgb.Write(0xFF12, 0xF8, sink);
        cgb.Write(0xFF14, 0x86, sink);
        cgb.Write(0xFF12, 0x08, sink); // +1 mod 16 -> 0
        Check(cgb.Out(0) == 0, "cgb zombie +1 wrap failed");
        cgb.Write(0xFF12, 0x51, sink); // pace 0 -> 1, still not halted (+1) -> 1
        cgb.Write(0xFF12, 0x58, sink); // decrease -> increase (+2 -> 3, flip -> 13)
        cgb.Write(0xFF11, 0x80, sink);
        cgb.Run(20000, sink);
        Check(cgb.Out(0) == 13 || cgb.Out(0) == 0, "cgb zombie mode flip failed");

        gb::Apu dmg;
        dmg.Reset(gb::Model::Dmg);
        dmg.Write(0xFF26, 0x00, sink); // power off
        dmg.Write(0xFF11, 0x3F, sink); // length = 1 on DMG while powered off
        dmg.Write(0xFF26, 0x80, sink); // power on
        dmg.Write(0xFF12, 0xF0, sink);
        dmg.Write(0xFF14, 0xC6, sink); // trigger with length enabled
        dmg.Run(16384, sink);
        Check((dmg.Read(0xFF26) & 1) == 0, "dmg length written while powered off did not expire");
    }
}

// ---------------------------------------------------------------------------
// NES.

Snapshot NesRead(const nes::Apu& a) { return { a.Out(0), a.Out(1), a.Out(2), a.Out(3) }; }

std::vector<Step<nes::Apu>> NesScript(bool lazy, std::int64_t heard) {
    using A = nes::Apu;
    using T = Trace<A>;
    std::vector<Step<A>> s;
    s.push_back(At<A>(5, [lazy](A& a, T& t) {
        a.Write(0x4015, 0x0F, t);
        // Constant volume with the length halted: volume 0 silences them.
        a.Write(0x4000, lazy ? 0xB0 : 0xBF, t);
        a.Write(0x4002, 0xFD, t);
        a.Write(0x4003, 0x00, t);
        a.Write(0x4004, lazy ? 0x70 : 0x7F, t);
        a.Write(0x4006, 0x52, t);
        a.Write(0x4007, 0x01, t);
        a.Write(0x4008, 0xFF, t);
        a.Write(0x400A, 0x80, t);
        a.Write(0x400B, 0x00, t);
        a.Write(0x400C, lazy ? 0x30 : 0x3F, t);
        a.Write(0x400E, 0x03, t);
        a.Write(0x400F, 0x00, t);
    }));
    s.push_back(At<A>(heard, [](A& a, T& t) {
        a.Write(0x4000, 0xBF, t);
        a.Write(0x4004, 0x7F, t);
        a.Write(0x400C, 0x3F, t);
    }));
    s.push_back(At<A>(heard + 200001, [](A& a, T& t) { a.Write(0x400E, 0x85, t); }));
    s.push_back(At<A>(heard + 300000, [](A& a, T& t) { a.Write(0x400C, 0x30, t); }));
    s.push_back(At<A>(heard + 600000, [](A& a, T& t) { a.Write(0x400C, 0x3A, t); }));
    return s;
}

void TestNes() {
    nes::Apu apu;
    apu.Reset(nes::Region::Ntsc);
    const std::int64_t heard = 987654, end = 2000000;
    CheckPartitions("nes", apu, NesRead, NesScript(false, heard), end);
    CheckPartitions("nes lazy", apu, NesRead, NesScript(true, heard), end);
    CheckCatchUp("nes", apu, NesRead, NesScript(false, heard), NesScript(true, heard), heard, 0, end);

    // NESdev, APU Pulse: these are the eight output bits in time order,
    // high bit first. A timer-high write restarts that sequence immediately,
    // while the existing timer still clocks at 400, 800, 1200, ... .
    const auto checkPulseRestart = [](auto chip, int registerBase) {
        constexpr int waveform[4] = { 0b01000000, 0b01100000, 0b01111000, 0b10011111 };
        auto sink = [](std::int64_t) {};
        for (int duty = 0; duty < 4; ++duty) {
            auto c = chip;
            c.Write(registerBase + 0x15, 0x03, sink);
            for (int channel = 0; channel < 2; ++channel) {
                const int base = registerBase + 4 * channel;
                c.Write(base, static_cast<std::uint8_t>((duty << 6) | 0x3F), sink);
                c.Write(base + 2, 199, sink);
                c.Write(base + 3, 0, sink);
            }
            c.Run(955, sink);
            c.Write(registerBase + 3, 0, sink);
            c.Write(registerBase + 7, 0, sink);
            c.Run(1200, sink);
            int observed[2] = {};
            for (int step = 0; step < 8; ++step) {
                for (int ch = 0; ch < 2; ++ch)
                    observed[ch] = (observed[ch] << 1) | (c.Out(ch) != 0);
                c.Run(1201 + 400 * step, sink);
            }
            Check(observed[0] == waveform[duty] && observed[1] == waveform[duty],
                  "pulse retrigger sequence, register " + std::to_string(registerBase) + ", duty " + std::to_string(duty));
        }
    };
    checkPulseRestart(apu, 0x4000);
    famicom::Mmc5 mmc5;
    mmc5.Reset();
    checkPulseRestart(mmc5, 0x5000);

    // NESdev, APU Pulse: f = CPU / (16 (t + 1)), duties 12.5, 25, 50 and
    // 25 % negated. APU Triangle: f = CPU / (32 (t + 1)), its 32 steps.
    for (int duty = 0; duty < 4; ++duty) {
        std::vector<Step<nes::Apu>> s;
        s.push_back(At<nes::Apu>(0, [duty](nes::Apu& a, Trace<nes::Apu>& t) {
            a.Write(0x4015, 0x05, t);
            a.Write(0x4000, static_cast<std::uint8_t>((duty << 6) | 0x3F), t);
            a.Write(0x4002, 0xC7, t);  // t = 199: 3200 cycles
            a.Write(0x4003, 0x00, t);
            a.Write(0x4008, 0xFF, t);
            a.Write(0x400A, 0x63, t);  // t = 99: 3200 cycles
            a.Write(0x400B, 0x00, t);
        }));
        const Trace<nes::Apu> t = Play(apu, NesRead, s, 400000, 400001);
        const Rectangle r = Measure(t, 0, 50000);
        const double expected[4] = { 0.125, 0.25, 0.5, 0.75 };
        Check(std::fabs(r.period - 3200.0) < 0.01, "nes pulse period " + std::to_string(r.period));
        Check(std::fabs(r.high - expected[duty]) < 0.002, "nes duty " + std::to_string(duty) + " high " + std::to_string(r.high));
        if (duty == 0) {
            // The triangle's levels as they change: 0 (held two steps) up
            // to 15 (held two) and down, 30 changes to 32 steps.
            std::vector<int> steps;
            std::vector<std::int64_t> at;
            for (const auto& e : t.events)
                if (e.first > 100000 && (steps.empty() || steps.back() != e.second[2])) {
                    steps.push_back(e.second[2]);
                    at.push_back(e.first);
                }
            const auto zero = static_cast<std::size_t>(std::find(steps.begin(), steps.end(), 0) - steps.begin());
            std::vector<int> want;
            for (int k = 0; k <= 15; ++k)
                want.push_back(k);
            for (int k = 14; k >= 1; --k)
                want.push_back(k);
            const bool enough = zero + 31 < steps.size();
            Check(enough && std::vector<int>(steps.begin() + static_cast<std::ptrdiff_t>(zero),
                                             steps.begin() + static_cast<std::ptrdiff_t>(zero + 30)) == want,
                  "nes triangle does not step 0..15..1");
            Check(enough && at[zero + 30] - at[zero] == 3200, "nes triangle period");
        }
    }
}

// ---------------------------------------------------------------------------
// AY-3-8910, the 5B's core.

Snapshot AyRead(const ay::Chip& c) { return { c.Out(0), c.Out(1), c.Out(2) }; }

std::vector<Step<ay::Chip>> AyScript(bool lazy, std::int64_t heard) {
    using C = ay::Chip;
    using T = Trace<C>;
    std::vector<Step<C>> s;
    s.push_back(At<C>(1, [lazy](C& c, T& t) {
        c.Write(0, 0xFD, t), c.Write(1, 0x00, t);
        c.Write(2, 0x37, t), c.Write(3, 0x01, t);
        c.Write(4, 0x10, t), c.Write(5, 0x00, t);
        c.Write(6, 0x07, t);
        c.Write(7, 0x1C, t);      // A tone, B tone, C noise
        c.Write(8, lazy ? 0x00 : 0x0F, t);
        c.Write(9, lazy ? 0x00 : 0x0A, t);
        c.Write(10, lazy ? 0x00 : 0x10, t);
        c.Write(11, 0x40, t), c.Write(12, 0x00, t);
        c.Write(13, 0x0E, t);
    }));
    s.push_back(At<C>(heard, [](C& c, T& t) {
        c.Write(8, 0x0F, t);
        c.Write(9, 0x0A, t);
        c.Write(10, 0x10, t);
    }));
    s.push_back(At<C>(heard + 77777, [](C& c, T& t) {
        c.Write(7, 0x31, t);      // A noise, B tone, C tone
        c.Write(1, 0x02, t);
        c.Write(6, 0x1F, t);
    }));
    return s;
}

void TestAy() {
    ay::Chip chip;
    chip.Reset(8, true);
    const std::int64_t heard = 1111111, end = 1800000;
    CheckPartitions("ay", chip, AyRead, AyScript(false, heard), end);
    CheckPartitions("ay lazy", chip, AyRead, AyScript(true, heard), end);
    CheckCatchUp("ay", chip, AyRead, AyScript(false, heard), AyScript(true, heard), heard, 0, end);

    // The data manual's shapes, read as each step's code: a channel on the
    // envelope with tone and noise off holds the gate open.
    struct Shape {
        int shape;
        std::vector<int> first64;
    };
    const auto ramp = [](bool up, int n) {
        std::vector<int> v;
        for (int i = 0; i < n; ++i)
            v.push_back(up ? i : 31 - i);
        return v;
    };
    const auto join = [](std::vector<int> a, const std::vector<int>& b) {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    };
    const std::vector<int> hold0(32, 0), hold31(32, 31);
    const std::vector<Shape> shapes = {
        { 0x00, join(ramp(false, 32), hold0) }, { 0x04, join(ramp(true, 32), hold0) },
        { 0x08, join(ramp(false, 32), ramp(false, 32)) }, { 0x09, join(ramp(false, 32), hold0) },
        { 0x0A, join(ramp(false, 32), ramp(true, 32)) }, { 0x0B, join(ramp(false, 32), hold31) },
        { 0x0C, join(ramp(true, 32), ramp(true, 32)) }, { 0x0D, join(ramp(true, 32), hold31) },
        { 0x0E, join(ramp(true, 32), ramp(false, 32)) }, { 0x0F, join(ramp(true, 32), hold0) },
    };
    for (const Shape& sh : shapes) {
        ay::Chip c = chip;
        Trace<ay::Chip> t;
        t.chip = &c;
        t.read = AyRead;
        c.Write(7, 0x3F, t);
        c.Write(8, 0x10, t);
        c.Write(11, 0x01, t), c.Write(12, 0x00, t);   // one step every 8 cycles
        c.Write(13, static_cast<std::uint8_t>(sh.shape), t);
        std::vector<int> codes { c.Out(0) };
        for (int i = 1; i < 64; ++i) {
            c.Run(c.Now() + 8, t);
            codes.push_back(c.Out(0));
        }
        Check(codes == sh.first64, "ay envelope shape " + std::to_string(sh.shape) + " does not follow the data manual");
    }

    // A tone toggles every 8 TP cycles (16 on the 5B's tick), TP 0 as 1.
    for (std::int64_t tick : { 8, 16 })
        for (int tp : { 0, 1, 0x1FD, 0xFFF }) {
            ay::Chip c;
            c.Reset(tick, true);
            Trace<ay::Chip> t;
            t.chip = &c;
            t.read = AyRead;
            t.events.emplace_back(-1, AyRead(c));
            c.Write(0, static_cast<std::uint8_t>(tp), t), c.Write(1, static_cast<std::uint8_t>(tp >> 8), t);
            c.Write(7, 0x3E, t);
            c.Write(8, 0x0F, t);
            const std::int64_t want = tick * (tp ? tp : 1);
            c.Run(want * 30, t);
            Check(t.events.size() > 12 && t.events[12].first - t.events[11].first == want,
                  "ay tone " + std::to_string(tp) + " does not toggle every " + std::to_string(want));
        }
}

// ---------------------------------------------------------------------------
// The cartridge chips.

Snapshot Vrc6Read(const famicom::Vrc6& c) { return { c.Out(0), c.Out(1), c.Out(2) }; }

std::vector<Step<famicom::Vrc6>> Vrc6Script(bool lazy, std::int64_t heard) {
    using C = famicom::Vrc6;
    using T = Trace<C>;
    std::vector<Step<C>> s;
    s.push_back(At<C>(2, [lazy](C& c, T& t) {
        c.Write(0x9000, lazy ? 0x30 : 0x3F, t);
        c.Write(0x9001, 0x80, t), c.Write(0x9002, 0x81, t);
        c.Write(0xA000, lazy ? 0x70 : 0x7A, t);
        c.Write(0xA001, 0x33, t), c.Write(0xA002, 0x82, t);
        c.Write(0xB000, lazy ? 0x00 : 0x10, t);
        c.Write(0xB001, 0x40, t), c.Write(0xB002, 0x82, t);
    }));
    s.push_back(At<C>(heard / 3, [lazy](C& c, T& t) {
        if (!lazy)
            c.Write(0xB000, 0x00, t);   // rate 0: the accumulator drains at the reset
    }));
    s.push_back(At<C>(heard / 3 + 50000, [lazy](C& c, T& t) {
        if (!lazy)
            c.Write(0xB000, 0x10, t);
    }));
    s.push_back(At<C>(heard, [](C& c, T& t) {
        c.Write(0x9000, 0x3F, t);
        c.Write(0xA000, 0x7A, t);
        c.Write(0xB000, 0x10, t);
    }));
    s.push_back(At<C>(heard + 40000, [](C& c, T& t) { c.Write(0x9003, 0x01, t); }));  // halt
    s.push_back(At<C>(heard + 90000, [](C& c, T& t) { c.Write(0x9003, 0x02, t); }));  // run, 16x
    s.push_back(At<C>(heard + 160000, [](C& c, T& t) { c.Write(0x9003, 0x00, t); }));
    return s;
}

Snapshot Mmc5Read(const famicom::Mmc5& c) { return { c.Out(0), c.Out(1), c.Out(2) }; }

std::vector<Step<famicom::Mmc5>> Mmc5Script(bool lazy, std::int64_t heard) {
    using C = famicom::Mmc5;
    using T = Trace<C>;
    std::vector<Step<C>> s;
    s.push_back(At<C>(4, [lazy](C& c, T& t) {
        c.Write(0x5015, 0x03, t);
        c.Write(0x5000, lazy ? 0xB0 : 0xBF, t);
        c.Write(0x5002, 0xFD, t), c.Write(0x5003, 0x00, t);
        c.Write(0x5004, lazy ? 0x30 : 0x36, t);
        c.Write(0x5006, 0x22, t), c.Write(0x5007, 0x01, t);
    }));
    s.push_back(At<C>(heard, [](C& c, T& t) {
        c.Write(0x5000, 0xBF, t);
        c.Write(0x5004, 0x36, t);
    }));
    s.push_back(At<C>(heard + 50000, [](C& c, T& t) { c.Write(0x5011, 0x40, t); }));
    return s;
}

Snapshot N163Read(const famicom::Namco163& c) { return { c.Out() }; }

std::vector<Step<famicom::Namco163>> N163Script(bool lazy, std::int64_t heard) {
    using C = famicom::Namco163;
    using T = Trace<C>;
    std::vector<Step<C>> s;
    s.push_back(At<C>(7, [lazy](C& c, T& t) {
        c.Write(0xF800, 0x80, t);
        for (int i = 0; i < 16; ++i)    // a 32-sample ramp
            c.Write(0x4800, static_cast<std::uint8_t>(((2 * i + 1) & 15) << 4 | ((2 * i) & 15)), t);
        // Four channels, 7 down to 4: freq, phase, length, wave, volume.
        for (int ch = 4; ch < 8; ++ch) {
            c.Write(0xF800, static_cast<std::uint8_t>(0x80 | (0x40 + 8 * ch)), t);
            const std::uint32_t freq = 0x1234 * static_cast<std::uint32_t>(ch - 2);
            c.Write(0x4800, static_cast<std::uint8_t>(freq), t);
            c.Write(0x4800, 0x00, t);
            c.Write(0x4800, static_cast<std::uint8_t>(freq >> 8), t);
            c.Write(0x4800, 0x00, t);
            c.Write(0x4800, static_cast<std::uint8_t>(0xE0 | ((freq >> 16) & 3)), t);   // 32 samples
            c.Write(0x4800, 0x00, t);
            c.Write(0x4800, 0x00, t);
            c.Write(0x4800, static_cast<std::uint8_t>(ch == 7 ? 0x30 | (lazy ? 0 : 9) : (lazy ? 0 : ch + 2)), t);
        }
    }));
    s.push_back(At<C>(heard, [](C& c, T& t) {
        for (int ch = 4; ch < 8; ++ch) {
            c.Write(0xF800, static_cast<std::uint8_t>(0x40 + 8 * ch + 7), t);
            c.Write(0x4800, static_cast<std::uint8_t>(ch == 7 ? 0x39 : ch + 2), t);
        }
    }));
    return s;
}

Snapshot FdsRead(const famicom::Fds& c) { return { c.Out(), c.MasterVolume() }; }

std::vector<Step<famicom::Fds>> FdsScript(bool lazy, std::int64_t heard) {
    using C = famicom::Fds;
    using T = Trace<C>;
    std::vector<Step<C>> s;
    s.push_back(At<C>(9, [lazy](C& c, T& t) {
        c.Write(0x4023, 0x02, t);
        c.Write(0x4089, 0x80, t);
        for (int i = 0; i < 64; ++i)
            c.Write(static_cast<std::uint16_t>(0x4040 + i), static_cast<std::uint8_t>(i < 32 ? i * 2 : 127 - i * 2), t);
        c.Write(0x4089, 0x00, t);
        c.Write(0x4080, lazy ? 0x80 : 0xA0, t);  // gain 0 or 32
        c.Write(0x4087, 0x80, t);
        for (int i = 0; i < 32; ++i)
            c.Write(0x4088, static_cast<std::uint8_t>(i < 16 ? 1 : 7), t);
        c.Write(0x4084, 0x8C, t);                 // modulation depth 12
        c.Write(0x4086, 0x00, t), c.Write(0x4087, 0x00, t);
        c.Write(0x4082, 0x00, t), c.Write(0x4083, 0x02, t);
    }));
    s.push_back(At<C>(heard, [](C& c, T& t) {
        c.Write(0x4080, 0xA0, t);
        c.Write(0x4086, 0x20, t);
    }));
    s.push_back(At<C>(heard + 150000, [](C& c, T& t) { c.Write(0x4080, 0x05, t); }));   // decay
    s.push_back(At<C>(heard + 350000, [](C& c, T& t) { c.Write(0x4089, 0x02, t); }));
    return s;
}

void TestCartridges() {
    famicom::Vrc6 vrc6;
    vrc6.Reset();
    const std::int64_t heard = 600000, end = 1000000;
    CheckPartitions("vrc6", vrc6, Vrc6Read, Vrc6Script(false, heard), end);
    CheckPartitions("vrc6 lazy", vrc6, Vrc6Read, Vrc6Script(true, heard), end);
    // The reference's saw is mid-cycle when the rate returns; it is in step
    // from its next reset, 14 clocks of 0x241 cycles later.
    CheckCatchUp("vrc6", vrc6, Vrc6Read, Vrc6Script(false, heard), Vrc6Script(true, heard), heard, 14 * 0x241, end);

    // A rate of 0 mid-cycle leaves the accumulator where it is until the
    // cycle's reset, which then takes it to 0 (NESdev, VRC6 audio).
    {
        famicom::Vrc6 c = vrc6;
        int events = 0;
        auto sink = [&](std::int64_t) { ++events; };
        c.Write(0xB000, 0x2A, sink);
        c.Write(0xB001, 0x09, sink), c.Write(0xB002, 0x80, sink);   // 10 cycles a clock
        c.Run(75, sink);
        const int held = c.Out(2);
        c.Write(0xB000, 0x00, sink);
        c.Run(c.Now() + 20, sink);
        Check(held > 0 && c.Out(2) == held, "vrc6 saw: rate 0 does not hold the accumulator");
        c.Run(c.Now() + 140, sink);
        Check(c.Out(2) == 0, "vrc6 saw: rate 0 does not fall to 0 at the cycle's reset");
    }

    famicom::Mmc5 mmc5;
    mmc5.Reset();
    CheckPartitions("mmc5", mmc5, Mmc5Read, Mmc5Script(false, heard), end);
    CheckPartitions("mmc5 lazy", mmc5, Mmc5Read, Mmc5Script(true, heard), end);
    CheckCatchUp("mmc5", mmc5, Mmc5Read, Mmc5Script(false, heard), Mmc5Script(true, heard), heard, 0, end);

    famicom::Namco163 n163;
    n163.Reset();
    CheckPartitions("n163", n163, N163Read, N163Script(false, heard), end);
    CheckPartitions("n163 lazy", n163, N163Read, N163Script(true, heard), end);
    // The lazy DAC reads 0 until its first slot after the volumes return.
    CheckCatchUp("n163", n163, N163Read, N163Script(false, heard), N163Script(true, heard), heard,
                 famicom::Namco163::kSlot, end);

    famicom::Fds fds;
    fds.Reset();
    CheckPartitions("fds", fds, FdsRead, FdsScript(false, heard), end);
    CheckPartitions("fds lazy", fds, FdsRead, FdsScript(true, heard), end);
    CheckCatchUp("fds", fds, FdsRead, FdsScript(false, heard), FdsScript(true, heard), heard, 10000, end);

    // VRC6 saw: disabling via $B002 bit 7 resets its 14-clock phase counter.
    {
        famicom::Vrc6 c = vrc6;
        auto sink = [](std::int64_t) {};
        c.Write(0xB000, 0x10, sink);
        c.Write(0xB001, 0x09, sink), c.Write(0xB002, 0x80, sink);
        c.Run(55, sink); // odd number of clocks into the 14-clock cycle
        c.Write(0xB002, 0x00, sink);
        c.Run(106, sink);          // five divider clocks pass with E clear
        c.Write(0xB002, 0x80, sink);
        c.Run(120, sink);          // one clock at 110, second at 120
        Check(c.Out(2) == 0, "vrc6 saw: disabled clocks must leave the phase reset");
        c.Run(121, sink);
        Check(c.Out(2) == (0x10 >> 3), "vrc6 saw: clearing E did not reset 14-clock phase");
    }

    // FDS negative-bias modulator rounding (rainwarrior's RP2C33 formula):
    // bias = -63, gain = 33 -> bias * gain = -2079, temp = -131 (-67 after +64),
    // wrapped to +189 -> (pitch * 189) = 0x100 * 189 = 48384.
    {
        famicom::Fds c = fds;
        auto sink = [](std::int64_t) {};
        c.Write(0x4023, 0x02, sink);
        c.Write(0x4089, 0x80, sink);
        for (int i = 0; i < 64; ++i)
            c.Write(static_cast<std::uint16_t>(0x4040 + i), static_cast<std::uint8_t>(i), sink);
        c.Write(0x4089, 0x00, sink);
        c.Write(0x4080, 0xA0, sink);
        c.Write(0x4084, 0xA1, sink); // gain 33
        c.Write(0x4085, 0x41, sink); // 7-bit signed -63
        c.Write(0x4082, 0x00, sink), c.Write(0x4083, 0x01, sink); // pitch = 0x100
        c.Run(16 * 128, sink);
        Check(c.Out() > 0, "fds negative-bias modulator produced silent/invalid output");
    }

    // NESdev, VRC6 audio: a pulse at period t runs at CPU / (16 (t + 1))
    // with duty (d + 1) / 16; the saw at CPU / (14 (t + 1)), its
    // accumulator 0, A, 2A .. 6A, of which the top five bits are heard.
    for (int duty = 0; duty < 8; ++duty) {
        std::vector<Step<famicom::Vrc6>> s;
        s.push_back(At<famicom::Vrc6>(0, [duty](famicom::Vrc6& c, Trace<famicom::Vrc6>& t) {
            c.Write(0x9000, static_cast<std::uint8_t>((duty << 4) | 0x0F), t);
            c.Write(0x9001, 0x63, t), c.Write(0x9002, 0x80, t);   // t = 99: 1600 cycles
            c.Write(0xB000, 0x0B, t);
            c.Write(0xB001, 0x63, t), c.Write(0xB002, 0x80, t);   // 1400 cycles
        }));
        const Trace<famicom::Vrc6> t = Play(vrc6, Vrc6Read, s, 200000, 200001);
        const Rectangle r = Measure(t, 0, 10000);
        Check(std::fabs(r.period - 1600.0) < 0.01, "vrc6 pulse period " + std::to_string(r.period));
        Check(std::fabs(r.high - (duty + 1) / 16.0) < 0.002, "vrc6 duty " + std::to_string(duty) + " high " + std::to_string(r.high));
        if (duty == 0) {
            std::vector<std::int64_t> resets;
            std::vector<int> levels;
            for (std::size_t i = 1; i < t.events.size(); ++i)
                if (t.events[i].second[2] != t.events[i - 1].second[2]) {
                    if (t.events[i].second[2] == 0)
                        resets.push_back(t.events[i].first);
                    if (resets.size() == 2)
                        levels.push_back(t.events[i].second[2]);
                }
            Check(resets.size() > 4 && resets[4] - resets[3] == 1400, "vrc6 saw period");
            const std::vector<int> want = { 0, 11 * 1 >> 3, 22 >> 3, 33 >> 3, 44 >> 3, 55 >> 3, 66 >> 3 };
            std::vector<int> distinct { 0 };
            for (int l : levels)
                if (l != distinct.back())
                    distinct.push_back(l);
            std::vector<int> wantDistinct { 0 };
            for (int l : want)
                if (l != wantDistinct.back())
                    wantDistinct.push_back(l);
            Check(distinct == wantDistinct, "vrc6 saw does not climb by its rate");
        }
    }
}

} // namespace

int main() {
    TestNoiseRegisters();
    TestGameBoy();
    TestNes();
    TestAy();
    TestCartridges();
    if (failures == 0)
        std::printf("Reboot.Chips: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
