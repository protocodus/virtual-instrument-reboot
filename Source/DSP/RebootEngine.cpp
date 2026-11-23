#include "RebootEngine.h"

#include "LevelTrims.h"

#include <algorithm>
#include <cmath>

namespace protocodus::reboot {

namespace {

int Clamp(int v, int lo, int hi) noexcept { return v < lo ? lo : v > hi ? hi : v; }
int Round(double v) noexcept { return static_cast<int>(std::floor(v + 0.5)); }

bool Same(const ChannelSettings& a, const ChannelSettings& b) noexcept {
    return a.voice == b.voice && a.period == b.period && a.volume == b.volume && a.shape == b.shape && a.mod == b.mod
        && a.pace == b.pace && a.up == b.up && a.loop == b.loop && a.sweep == b.sweep && a.sweepPace == b.sweepPace
        && a.spread == b.spread;
}

// Arp rate and Vibrato rate with Sync, in beats (quarter notes), in their
// controls' order (RebootControls.h): an arpeggio's step, longer as the
// knob turns; a vibrato's cycle, shorter as the knob turns. Ours.
constexpr double kArpBeats[8] = { 1.0 / 16.0, 1.0 / 12.0, 1.0 / 8.0, 1.0 / 6.0, 1.0 / 4.0, 1.0 / 3.0, 1.0 / 2.0, 1.0 };
constexpr double kVibratoBeats[8] = { 4.0, 2.0, 1.0, 2.0 / 3.0, 1.0 / 2.0, 1.0 / 3.0, 1.0 / 4.0, 1.0 / 6.0 };

// The pace code whose step is nearest `cycles`, within the unit's.
int PaceFor(double cycles, const EnvelopeUnit& unit) noexcept {
    return Clamp(Round(cycles / unit.cycles), 1, unit.paces);
}

} // namespace

void Engine::Prepare(double sampleRate, int oversampling) {
    // A rate no host runs at (none, NaN, absurd) is taken as 48 kHz.
    if (!(sampleRate >= 8000.0 && sampleRate <= 768000.0))
        sampleRate = 48000.0;
    rate_ = Round(sampleRate);
    oversampling_ = oversampling >= 8 ? 8 : oversampling >= 4 ? 4 : oversampling >= 2 ? 2 : 1;
    while (oversampling_ > 1 && rate_ * oversampling_ > kMaxInternalRate)
        oversampling_ /= 2;
    const auto curve5B = Console::Make5BCurve();
    for (int u = 0; u < 2 * kMaxBoards; ++u)
        machines_[u].Prepare(rate_ * oversampling_, curve5B, u);
    decimator_.Prepare(oversampling_);
    stage_.Prepare(static_cast<double>(rate_));
    for (Slot& s : slots_)
        s = Slot {};
    heldCount_ = 0;
    pedal_ = false;
    bend_ = wheel_ = 0.0;
    Apply(true);
    trimFramesLeft_ = 0;
}

void Engine::SetControls(const Controls& raw) {
    // Any value a front end or automation sends, NaN included, lands in the
    // control's range.
    Controls c = raw;
    for (int i = 0; i < kControlCount; ++i) {
        const ControlInfo& info = kControlInfo[static_cast<std::size_t>(i)];
        const double v = c[i];
        if (!(v >= info.min && v <= info.max))
            c[i] = v != v ? info.initial : v < info.min ? info.min : info.max;
    }
    if (c.value == controls_.value)
        return;
    retryPitch_ |= c[kTranspose] != controls_[kTranspose] || c[kFine] != controls_[kFine]
        || c[kBendRange] != controls_[kBendRange] || c[kPolyChannels] != controls_[kPolyChannels];
    controls_ = c;
    Apply(false);
}

// Compare the effective hardware. A stored setting of an absent cartridge
// must not reset the clock, silence the release tails or retrigger held keys.
void Engine::Apply(bool forceReset) {
    const MachineSettings old = settings_;
    const int oldBoards = boards_, oldMode = mode_, oldPart = part_;
    const bool wasDouble = double_;
    const double oldDetune = detune_;
    const int oldOrder = arpOrder_;
    const int oldEnvelope = envelope_;
    const bool wasSync = sync_;
    const Controls& c = controls_;
    mode_ = Clamp(Round(c[kMode]), 0, 2);
    arpRate_ = Clamp(Round(c[kArpRate]), 1, 8);
    bendRange_ = Clamp(Round(c[kBendRange]), 0, 12);
    vibratoDepth_ = c[kVibratoDepth];
    velocity_ = c[kVelocity];
    double_ = c[kDouble] >= 0.5;
    detune_ = c[kDetune];
    chip_ = c[kPolyChannels] >= 0.5;
    arpOrder_ = Clamp(Round(c[kArpOrder]), 0, 3);
    arpRetrigger_ = c[kArpRetrigger] >= 0.5;
    envelope_ = Clamp(Round(c[kEnvelope]), 0, 2);
    sweep_ = Clamp(Round(c[kSweep]), -7, 7);
    sweepPace_ = Clamp(Round(c[kSweepPace]), 1, 8);
    spread_ = c[kSides] >= 0.5;
    sync_ = c[kSync] >= 0.5;
    arpSync_ = Clamp(Round(c[kArpSync]), 0, 7);
    vibratoSync_ = Clamp(Round(c[kVibratoSync]), 0, 7);
    for (Machine& m : machines_)
        m.SetHeadphones(c[kLoad] >= 0.5);
    const int ticks = Clamp(Round(c[kDriverRate]), 1, 8);
    if (arpOrder_ != oldOrder)
        ArpAnchor();
    part_ = Clamp(Round(c[kPart]), 0, Machine::kParts - 1);
    shape_ = Clamp(Round(c[kShape]), 0, 7);
    level_ = Clamp(Round(c[kLevel]), 0, 15);
    sustain_ = Clamp(Round(c[kSustain]), 0, 15);
    transpose_ = Clamp(Round(c[kTranspose]), -24, 24);
    fine_ = c[kFine];
    mod_ = Clamp(Round(c[kMod]), 0, 63);

    settings_.board = static_cast<BoardKind>(Clamp(Round(c[kBoard]), 0, static_cast<int>(BoardKind::Count) - 1));
    const bool console = settings_.board == BoardKind::Console;
    const int revisions = console ? static_cast<int>(Console::kRevisions) : static_cast<int>(Handheld::kRevisions);
    settings_.revision = Clamp(Round(c[kRevision]), 0, revisions - 1);
    settings_.expansion = console && settings_.revision == Console::kFamicom
        ? Clamp(Round(c[kExpansion]), 0, Console::kCartridges - 1) : Console::kNone;
    settings_.expansionChannels = settings_.expansion == Console::kN163 ? Clamp(Round(c[kExpansionChannels]), 1, 8) : 1;
    settings_.bias = Clamp(Round(c[kBias]), 0, 127);
    boards_ = Clamp(Round(c[kBoards]), 1, kMaxBoards);
    path_ = c[kPath] >= 0.5 ? Path::Line : Path::Pin;
    const bool reset = forceReset || old.board != settings_.board || old.revision != settings_.revision
        || old.expansion != settings_.expansion || old.expansionChannels != settings_.expansionChannels
        || oldBoards != boards_ || wasDouble != double_;
    if (reset) {
        // Each unit is switched on and stands for its own warm-up, so no two
        // play from the same moment of their frames; its driver counts its
        // frames from that power-on.
        for (int u = 0; u < 2 * kMaxBoards; ++u)
            if (Active(u))
                machines_[u].Reset(settings_, Pull(u / kMaxBoards));
    } else {
        for (int u = 0; u < 2 * kMaxBoards; ++u) {
            if (!Active(u))
                continue;
            if (console && old.bias != settings_.bias)
                machines_[u].SetBias(settings_.bias);
            if (detune_ != oldDetune)
                machines_[u].SetTrim(Pull(u / kMaxBoards));
        }
    }
    // The driver ticks Driver rate times a frame of the board as reset, on
    // the nominal clock, which is all it knows; its ticks run on from each
    // unit's power-on. More than one a frame is a driver on a timer
    // interrupt, which both machines have (the handheld's timer, Pan Docs
    // "Timer and Divider Registers"; on the console a cartridge's cycle
    // counter or the DMC's IRQ), as LSDj's FAST mode and an NSF's
    // play-speed field use; whole ticks a frame are ours.
    if (reset || ticks != ticks_) {
        ticks_ = ticks;
        machines_[0].FramePeriod(periodNum_, periodDen_);
        frameHz_ = machines_[0].ClockHz() * static_cast<double>(periodDen_) / static_cast<double>(periodNum_);
        periodDen_ *= ticks_;
        for (int u = 0; u < 2 * kMaxBoards; ++u)
            if (Active(u))
                Pace(u);
    }
    const Machine& m = machines_[0];
    const Part part = m.PartOf(part_);
    const double targetTrim = part.count > 0 ? LevelTrim(m, path_, part_, shape_ % m.Shapes(part.first, part.voice)) : 1.0;
    if (targetTrim != trim_) {
        const int duration = kTrimRampFrames * oversampling_;
        const double current = trimFramesLeft_ == 0 ? trim_ :
            trimStart_ + trimStep_ * static_cast<float>(duration - trimFramesLeft_);
        trimStart_ = static_cast<float>(current);
        trimStep_ = static_cast<float>((targetTrim - current) / duration);
        trimFramesLeft_ = duration;
        trim_ = targetTrim;
    }
    Times();
    // With Sync on, a running arpeggio steps as if its key were struck now.
    if (sync_ && !wasSync)
        arpDue_ = FirstStep();
    gain_ = std::pow(10.0, c[kVolume] / 20.0);
    // A new envelope takes a trigger on the chip, so the held keys play again.
    if (reset || oldMode != mode_ || oldPart != part_ || oldEnvelope != envelope_)
        Revoice(reset);
}

// The driver holds its times in ticks. With Sync, Arp rate and Vibrato
// rate are note values at the song's tempo: the arpeggio's steps are due on
// the song's time (ArpDue), and the vibrato moves as far a tick as its
// cycle at that tempo asks.
void Engine::Times() {
    const Controls& c = controls_;
    const double tickHz = frameHz_ * ticks_;
    attack_ = c[kAttack] / 1000.0 * tickHz;
    decay_ = c[kDecay] / 1000.0 * tickHz;
    release_ = c[kRelease] / 1000.0 * tickHz;
    vibratoStep_ = (sync_ ? tempo_ / 60.0 / kVibratoBeats[vibratoSync_] : c[kVibratoRate]) / tickHz;
    vibratoDelay_ = Round(c[kVibratoDelay] / 1000.0 * tickHz);
    glideFrames_ = Round(c[kGlide] / 1000.0 * tickHz);
    const double unit = static_cast<double>(std::int64_t { 1 } << kSongBits);
    halfTick_ = static_cast<std::int64_t>(rate_ / tickHz / 2.0 * unit);
    const std::int64_t step = static_cast<std::int64_t>(kArpBeats[arpSync_] * 60.0 / tempo_ * rate_ * unit + 0.5);
    // A new tempo or note value keeps the next step's place in its step.
    if (arpStep_ > 0 && step != arpStep_)
        arpDue_ = static_cast<std::int64_t>(static_cast<double>(arpDue_) * (static_cast<double>(step) / static_cast<double>(arpStep_)));
    arpStep_ = step;
}

void Engine::SetTempo(double bpm) {
    bpm = bpm >= 1.0 ? (bpm <= 999.0 ? bpm : 999.0) : bpm == bpm ? 1.0 : tempo_;
    if (bpm == tempo_)
        return;
    tempo_ = bpm;
    Times();
}

void Engine::SetSongPosition(double beats) {
    song_ = beats >= 0.0 && beats <= 1.0e9 ? beats : -1.0;
}

// Sync: how long the step of a key struck now lasts. While the song plays,
// the step starts on the line of the note value's grid nearest the key, so
// it lasts from half a step to one and a half (ours: a sequencer's
// arpeggiator keeps the song's grid in the same way).
std::int64_t Engine::FirstStep() const noexcept {
    if (song_ < 0.0)
        return arpStep_;
    const double steps = song_ / kArpBeats[arpSync_];
    const double phase = steps - static_cast<double>(static_cast<std::int64_t>(steps));
    return static_cast<std::int64_t>((phase < 0.5 ? 1.0 - phase : 2.0 - phase) * static_cast<double>(arpStep_));
}

// Sync: whether unit 0's tick `at` takes the arpeggio's next step, which
// it does when the step is due nearer it than the tick after, so no step
// is off by more than half a tick and the steps keep the song's tempo, as a
// tracker's rows do at a tempo its frames do not divide (mixing lengths of
// n and n + 1). A step shorter than a tick waits for the next.
bool Engine::ArpDue(std::int64_t at) noexcept {
    if (arpDue_ - at >= halfTick_)
        return false;
    arpDue_ += arpStep_;
    if (arpDue_ - at < halfTick_)
        arpDue_ = at + arpStep_;
    return true;
}

// Ends every note (after a reset, the machines are silent already) and
// plays the held keys again.
void Engine::Revoice(bool reset) {
    for (int s = 0; s < kSlots; ++s) {
        if (reset)
            slots_[s] = Slot {};
        else
            End(s);
    }
    mono_ = -1;
    if (heldCount_ == 0)
        return;
    switch (mode_) {
    case 0:
        for (int i = 0; i < heldCount_; ++i)
            held_[i].waitingForPitch = StartNote(held_[i].key, held_[i].velocity) < 0;
        break;
    case 1: {
        const Key& top = held_[heldCount_ - 1];
        mono_ = StartNote(top.key, top.velocity);
        break;
    }
    default: {
        arpUp_ = true;
        const Key& first = NextArpKey(-1);
        arpKey_ = first.key, arpRank_ = ArpRank(first), arpCount_ = 0, arpDue_ = FirstStep();
        mono_ = StartNote(first.key, first.velocity);
        break;
    }
    }
}

void Engine::Midi(const std::uint8_t* bytes, int size) {
    if (size < 1)
        return;
    const int status = bytes[0] & 0xF0;
    const int d1 = size > 1 ? bytes[1] & 0x7F : 0;
    const int d2 = size > 2 ? bytes[2] & 0x7F : 0;
    switch (status) {
    case 0x90:
        if (size < 3)
            return;
        if (d2 > 0)
            NoteOn(d1, d2);
        else
            NoteOff(d1);
        break;
    case 0x80:
        if (size >= 2)
            NoteOff(d1);
        break;
    case 0xB0:
        if (size < 3)
            return;
        switch (d1) {
        case 1: SetWheel(d2 / 127.0); break;
        case 64: SetSustain(d2 >= 64); break;
        case 120: Panic(); break;
        case 121:
            SetBend(0.0);
            SetWheel(0.0);
            SetSustain(false);
            break;
        case 123: ReleaseAll(); break;
        default: break;
        }
        break;
    case 0xE0:
        if (size >= 3)
            SetBend((((d2 << 7) | d1) - 8192) / 8192.0);
        break;
    default:
        break;
    }
}

int Engine::HeldIndex(int key) const noexcept {
    for (int i = 0; i < heldCount_; ++i)
        if (held_[i].key == key)
            return i;
    return -1;
}

// A held key's place in the arpeggio's order, never below 0: its pitch (up,
// and up and down), its pitch from the top (down) or when it was struck
// (as played).
std::int64_t Engine::ArpRank(const Key& k) const noexcept {
    return arpOrder_ == 1 ? 127 - k.key : arpOrder_ == 3 ? static_cast<std::int64_t>(k.struck) : k.key;
}

// The held key nearest past rank `after`, upward (sign 1) or downward (-1).
const Engine::Key* Engine::ArpSeek(std::int64_t after, int sign) const noexcept {
    const Key* best = nullptr;
    for (int i = 0; i < heldCount_; ++i) {
        const std::int64_t r = sign * ArpRank(held_[i]);
        if (r > sign * after && (!best || r < sign * ArpRank(*best)))
            best = &held_[i];
    }
    return best;
}

// The next held key after rank `after`, wrapping to the first; up and down
// turns at either end without playing it twice. Live chord edits do not
// move the cursor: it remembers the last played key's rank, not its array
// index. Needs a held key.
const Engine::Key& Engine::NextArpKey(std::int64_t after) noexcept {
    if (arpOrder_ != 2)
        if (const Key* k = ArpSeek(after, 1))
            return *k;
    if (arpOrder_ == 2) {
        if (const Key* k = ArpSeek(after, arpUp_ ? 1 : -1))
            return *k;
        arpUp_ = !arpUp_;
        if (const Key* k = ArpSeek(after, arpUp_ ? 1 : -1))
            return *k;
    }
    return *ArpSeek(-1, 1);
}

// A new order keeps the cursor on the key last played, so the next step is
// its neighbour in the new order.
void Engine::ArpAnchor() noexcept {
    const int i = HeldIndex(arpKey_);
    arpRank_ = i >= 0 ? ArpRank(held_[i]) : arpOrder_ == 1 ? 127 - arpKey_ : arpOrder_ == 3 ? 0 : arpKey_;
}

void Engine::NoteOn(int key, int velocity) {
    key = Clamp(key, 0, 127);
    if (velocity <= 0) {
        NoteOff(key);
        return;
    }
    if (HeldIndex(key) >= 0)
        Lift(key);
    held_[heldCount_++] = { key, Clamp(velocity, 1, 127), true, false, ++struck_ };
    Strike(held_[heldCount_ - 1]);
}

void Engine::NoteOff(int key) {
    const int i = HeldIndex(Clamp(key, 0, 127));
    if (i < 0)
        return;
    if (pedal_) {
        held_[i].down = false;
        return;
    }
    Lift(held_[i].key);
}

void Engine::SetSustain(bool down) {
    if (pedal_ == down)
        return;
    pedal_ = down;
    if (down)
        return;
    // The pedal releases its keys together. Retargeting after each removal
    // would play intervening keys and put a Mono release on the wrong pitch.
    int kept = 0;
    for (int i = 0; i < heldCount_; ++i) {
        if (held_[i].down)
            held_[kept++] = held_[i];
        else if (mode_ == 0)
            ReleaseKey(held_[i].key);
    }
    if (kept != heldCount_) {
        heldCount_ = kept;
        if (mode_ != 0)
            FollowHeld();
    }
}

// NaN lands at the bottom of the range.
void Engine::SetBend(double amount) {
    const double next = amount > -1.0 ? (amount < 1.0 ? amount : 1.0) : -1.0;
    retryPitch_ |= next != bend_;
    bend_ = next;
}
void Engine::SetWheel(double amount) { wheel_ = amount > 0.0 ? (amount < 1.0 ? amount : 1.0) : 0.0; }
// NaN plays at pitch rather than a semitone flat.
void Engine::SetTune(double cents) {
    const double next = std::isfinite(cents) ? std::clamp(cents, -100.0, 100.0) : 0.0;
    retryPitch_ |= next != tune_;
    tune_ = next;
}

void Engine::ReleaseAll() {
    pedal_ = false;
    heldCount_ = 0;
    for (int s = 0; s < kSlots; ++s)
        if (slots_[s].busy && slots_[s].gate)
            ReleaseSlot(s);
}

void Engine::Panic() {
    pedal_ = false;
    heldCount_ = 0;
    for (int s = 0; s < kSlots; ++s)
        End(s);
}

void Engine::Reset() {
    pedal_ = false;
    heldCount_ = 0;
    decimator_.Reset();
    stage_.Reset();
    // The machines reset, and the slots cleared with them.
    Apply(true);
    trimFramesLeft_ = 0;
}

int Engine::Sounding() const noexcept {
    int n = 0;
    for (const Slot& s : slots_)
        n += s.busy;
    return n;
}

// A key that was just added to held_.
void Engine::Strike(Key& note) {
    const int key = note.key, velocity = note.velocity;
    switch (mode_) {
    case 0:
        note.waitingForPitch = StartNote(key, velocity) < 0;
        break;
    case 1:
        if (mono_ >= 0 && slots_[mono_].gate) {
            Retarget(key, velocity, true);
        } else {
            if (mono_ >= 0)
                End(mono_);
            mono_ = StartNote(key, velocity);
        }
        break;
    default:
        // The arpeggio steps on the machines' frame; a key struck while it
        // runs joins it.
        if (mono_ >= 0 && slots_[mono_].gate)
            break;
        arpKey_ = key, arpRank_ = ArpRank(note), arpCount_ = 0, arpDue_ = FirstStep();
        arpUp_ = true;
        if (mono_ >= 0)
            End(mono_);
        mono_ = StartNote(key, velocity);
        break;
    }
}

// A note that was unplayable at onset has no hardware slot yet. Retry it
// on a pitch change or when a channel becomes free. A recovering Poly key
// may take a free channel, but a wheel movement must not steal a sounding key.
// A stolen Poly note already had its onset and must not be resurrected.
void Engine::RetryHeldPitches() {
    retryPitch_ = false;
    if (mode_ == 0) {
        for (int i = 0; i < heldCount_; ++i) {
            Key& k = held_[i];
            if (k.waitingForPitch)
                k.waitingForPitch = StartNote(k.key, k.velocity, false) < 0;
        }
    } else if (mono_ < 0 && heldCount_ > 0) {
        const int index = mode_ == 1 ? heldCount_ - 1 : HeldIndex(arpKey_);
        if (index >= 0)
            mono_ = StartNote(held_[index].key, held_[index].velocity);
    }
}

// Removes a held key and ends what it holds.
void Engine::Lift(int key) {
    const int i = HeldIndex(key);
    if (i < 0)
        return;
    for (int j = i + 1; j < heldCount_; ++j)
        held_[j - 1] = held_[j];
    --heldCount_;
    if (mode_ == 0)
        ReleaseKey(key);
    else
        FollowHeld();
}

void Engine::ReleaseKey(int key) {
    for (int s = 0; s < boards_ * Machine::kMaxChannels; ++s)
        if (slots_[s].busy && slots_[s].gate && slots_[s].key == key)
            ReleaseSlot(s);
}

// Mono uses the newest remaining key. Arp keeps its current step until
// the next frame due; both start their release on the last sounding pitch.
void Engine::FollowHeld() {
    if (heldCount_ == 0) {
        if (mono_ >= 0 && slots_[mono_].gate)
            ReleaseSlot(mono_);
    } else if (mode_ == 1) {
        const Key& top = held_[heldCount_ - 1];
        if (mono_ < 0)
            mono_ = StartNote(top.key, top.velocity);
        else if (slots_[mono_].key != top.key)
            Retarget(top.key, top.velocity, true);
    }
}

// Starts `key` on a channel of the part, or of its fallback when the part
// cannot reach it: a free channel, the one freed longest ago, else the
// oldest released note's, else the oldest note's. Returns the slot or -1.
int Engine::StartNote(int key, int velocity, bool maySteal) {
    const Machine& m = machines_[0];
    const Part chosen = m.PartOf(part_);
    if (chosen.count == 0)
        return -1;
    Slot probe;
    probe.key = key;
    const double hz = Pitch(probe, 0);
    const auto reaches = [&](const Part& p) {
        std::int64_t period = 0;
        return p.count > 0 && m.PeriodFor(p.first, p.voice, hz, shape_ % m.Shapes(p.first, p.voice), period);
    };
    // The parts that may take the key, in the order they are tried: the
    // chosen one, or its fallback when it cannot reach the pitch; with Poly
    // channels, in Poly, the chip's other pitched parts after it, as one
    // machine plays a chord on its unlike voices (mGB's POLY channel plays
    // the handheld's two pulses and wave so).
    Part parts[Machine::kParts + 1];
    int count = 0;
    const bool fallback = !reaches(chosen);
    const Part first = fallback ? m.FallbackOf(chosen) : chosen;
    if (reaches(first))
        parts[count++] = first;
    if (mode_ == 0 && chip_ && chosen.voice != Voice::Noise)
        for (int i = 0; i < Machine::kParts; ++i) {
            const Part other = m.PartOf(i);
            if (other.voice != Voice::Noise && other.first != first.first && other.first != chosen.first && reaches(other))
                parts[count++] = other;
        }
    if (count == 0)
        return -1;

    // A slot's rank: 0 free, 1 released, 2 held. A later part is taken only
    // when every channel of the parts before it is held, so a melody keeps
    // its voice and a chord spills onto the chip's others; when all are
    // held, the oldest note of them all gives way.
    int best = -1, bestRank = 3, bestPart = 0;
    std::uint64_t bestAge = 0;
    for (int p = 0; p < count && bestRank >= 2; ++p)
        for (int b = 0; b < boards_; ++b)
            for (int ch = parts[p].first; ch < parts[p].first + parts[p].count; ++ch) {
                const int s = b * Machine::kMaxChannels + ch;
                const Slot& n = slots_[s];
                const int rank = !n.busy ? 0 : n.gate ? 2 : 1;
                if (rank < bestRank || (rank == bestRank && n.age < bestAge))
                    best = s, bestRank = rank, bestAge = n.age, bestPart = p;
            }
    if (best < 0 || (!maySteal && bestRank != 0))
        return -1;
    const Part part = parts[bestPart];

    End(best);
    Slot& n = slots_[best];
    n = Slot {};
    n.busy = true, n.gate = true;
    n.key = key, n.velocity = velocity;
    n.voice = part.voice;
    // Only the chosen part's fallback goes back to it when it can; a
    // chord's spill stays on the voice it took.
    n.fallback = fallback && bestPart == 0;
    n.age = ++age_;
    // A note at sustain 0 keeps its channel while the key holds it, silent,
    // so that raising the sustain brings it back.
    for (int set = 0; set < Sets(); ++set) {
        Settle(n.set[set]);
        BeginChip(best, set);
        ChannelSettings s;
        Compose(best, set, s);
        Send(best, set, s, true);
    }
    n.started = true;
    return best;
}

// A legato move of a Mono or Arp note to `key`, gliding from its current pitch.
// Each set glides from where its own driver has the pitch.
void Engine::Retarget(int key, int velocity, bool glide) {
    Slot& n = slots_[mono_];
    for (Program& p : n.set) {
        const double offset = glide && glideFrames_ > 0 ? n.key + p.glide - key : 0.0;
        p.glide = offset;
        p.glideLeft = offset != 0.0 ? glideFrames_ : 0;
    }
    n.key = key, n.velocity = velocity;
    Write(mono_);
}

// The envelope from the start, as a new note's: the arpeggio's steps.
void Engine::Restart(Slot& n) const noexcept {
    for (Program& p : n.set) {
        p.stage = Stage::Attack;
        p.env = 0.0;
        p.restart = true;
        Settle(p);
    }
}

void Engine::ReleaseSlot(int slot) {
    Slot& n = slots_[slot];
    n.gate = false;
    for (int set = 0; set < Sets(); ++set) {
        Program& p = n.set[set];
        if (p.ended)
            continue;
        p.stage = Stage::Released;
        p.from = p.env;
        Settle(p);
        if (!p.chip) {
            if (p.env <= 0.0)
                EndSet(slot, set);
            continue;
        }
        // The release of a note the chip's unit plays. Under a frame it is
        // cut. The handheld's unit falls from where it is at the release's
        // pace, from a trigger, as a driver releases a note on it. The
        // console's starts only from 15, so a falling envelope runs on to 0,
        // and one that would not end (looping) goes to the driver's release
        // from where it is.
        const int unit = UnitOf(slot, set);
        const int volume = ChipVolume(p, unit);
        if (release_ < 1.0 || volume == 0) {
            EndSet(slot, set);
        } else if (!p.chipFull) {
            const Machine& m = machines_[unit];
            const EnvelopeUnit u = m.EnvelopeOf(Channel(slot));
            const double tick = static_cast<double>(periodNum_) / static_cast<double>(periodDen_);
            p.chipFrom = volume, p.chipUp = false, p.chipLoop = false;
            p.chipPace = PaceFor(release_ * tick / volume, u);
            p.chipStep = p.chipPace * u.cycles;
            p.chipAt = m.Now();
            ChannelSettings s;
            if (!Compose(slot, set, s))
                s.volume = 0, s.pace = 0;
            Send(slot, set, s, true);
        } else if (p.chipLoop || p.chipUp) {
            ToDriver(slot, set);
        }
    }
}

// A set's channel stops when its own release runs out; the note ends when
// every set's has.
void Engine::EndSet(int slot, int set) {
    Slot& n = slots_[slot];
    if (!n.busy || n.set[set].ended)
        return;
    if (n.started)
        machines_[UnitOf(slot, set)].Stop(Channel(slot));
    n.set[set].ended = true;
    for (int k = 0; k < Sets(); ++k)
        if (!n.set[k].ended)
            return;
    End(slot);
}

void Engine::End(int slot) {
    Slot& n = slots_[slot];
    if (!n.busy)
        return;
    if (n.started)
        for (int set = 0; set < Sets(); ++set)
            if (!n.set[set].ended)
                machines_[UnitOf(slot, set)].Stop(Channel(slot));
    if (mono_ == slot)
        mono_ = -1;
    n.busy = false, n.gate = false, n.started = false;
    n.age = ++age_;
    if (mode_ == 0)
        retryPitch_ = true;
}

// Envelope Chip: what the note asks of the channel's own envelope unit
// from its trigger. Attack climbs from 0 at the pace nearest its time over
// the unit's steps, to the unit's top whatever the level, where the unit
// stops. Else the unit falls from the level (the handheld's and the disk
// unit's) or from its top (the console's and the 5B's, whose envelopes
// start nowhere else) at the pace nearest the step the driver's own decay
// would take, and runs on past the sustain level to 0: a unit stops only
// at an end (ours, README How it works). With nothing to fall (Sustain 15)
// the driver holds the level as it does without. A unit the chip's
// channels share (the 5B's) is the newest note's: the notes it played go
// on under the driver from where they are (ours: a trigger restarts it on
// every channel that listens to it).
void Engine::BeginChip(int slot, int set) {
    Slot& n = slots_[slot];
    Program& p = n.set[set];
    const Machine& m = machines_[UnitOf(slot, set)];
    const int ch = Channel(slot);
    const EnvelopeUnit u = m.EnvelopeOf(ch);
    p.chip = false;
    if (envelope_ == 0 || u.paces == 0)
        return;
    const double tick = static_cast<double>(periodNum_) / static_cast<double>(periodDen_);
    int from = u.top, pace = 0;
    const bool up = attack_ >= 1.0 && u.climbs;
    if (up) {
        from = 0;
        pace = PaceFor(attack_ * tick / u.top, u);
    } else {
        if (u.fromVolume) {
            const double touch = 1.0 - velocity_ * (1.0 - n.velocity / 127.0);
            from = m.QuantizeVolume(ch, n.voice, level_ / 15.0 * touch);
        }
        if (from == 0 || sustain_ >= 15)
            return;
        pace = PaceFor(decay_ * tick / (from * (15 - sustain_) / 15.0), u);
    }
    if (u.shared)
        FreeUnit(slot, set);
    p.chip = true;
    p.chipFull = up || !u.fromVolume;
    p.chipUp = up;
    p.chipLoop = envelope_ == 2 && u.loops && !up;
    p.chipTop = u.top;
    p.chipFrom = from;
    p.chipPace = pace;
    p.chipStep = pace * u.cycles;
    p.chipAt = m.Now();
}

// Where the unit's envelope is now, as the driver counts its steps from the
// trigger (ours: it can be a step from the chip's own, whose steps fall on
// the chip's sequencer, as any driver's own count could be).
int Engine::ChipVolume(const Program& p, int unit) const noexcept {
    const std::int64_t elapsed = machines_[unit].Now() - p.chipAt;
    const std::int64_t steps = elapsed <= 0 ? 0 : static_cast<std::int64_t>(static_cast<double>(elapsed) / p.chipStep);
    const int top = p.chipTop;
    if (p.chipUp)
        return steps >= top - p.chipFrom ? top : p.chipFrom + static_cast<int>(steps);
    if (p.chipLoop)
        return top - static_cast<int>((top - p.chipFrom + steps) % (top + 1));
    return steps >= p.chipFrom ? 0 : p.chipFrom - static_cast<int>(steps);
}

// Every other note on the machine that a shared unit plays goes on under
// the driver, before the unit is triggered again.
void Engine::FreeUnit(int slot, int set) {
    const int unit = UnitOf(slot, set);
    const Machine& m = machines_[unit];
    for (int other = 0; other < kSlots; ++other) {
        if (other == slot || !slots_[other].busy || !slots_[other].started)
            continue;
        for (int k = 0; k < Sets(); ++k) {
            Program& q = slots_[other].set[k];
            if (q.ended || !q.chip || UnitOf(other, k) != unit || !m.EnvelopeOf(Channel(other)).shared)
                continue;
            ToDriver(other, k);
            WriteSet(other, k);
        }
    }
}

// The driver takes a note from the unit where the unit has it: onto a
// channel without one (a fallback), or to release a looping one.
void Engine::ToDriver(int slot, int set) {
    Slot& n = slots_[slot];
    Program& p = n.set[set];
    const int volume = ChipVolume(p, UnitOf(slot, set));
    p.chip = false;
    p.full = p.chipFull;
    const double touch = 1.0 - velocity_ * (1.0 - n.velocity / 127.0);
    const double share = p.full ? 1.0 : level_ / 15.0 * touch;
    p.env = share > 0.0 ? std::min(15.0, 15.0 * volume / p.chipTop / share) : 0.0;
    if (n.gate) {
        p.stage = Stage::Decay;
    } else {
        p.stage = Stage::Released;
        p.from = p.env;
    }
    Settle(p);
}

// The envelope's moves that take no frame. A stage shorter than a frame
// takes none (ours: the driver moves on the frame).
void Engine::Settle(Program& p) const noexcept {
    if (p.stage == Stage::Attack && attack_ < 1.0) {
        p.env = 15.0;
        p.stage = Stage::Decay;
    }
    if (p.stage == Stage::Decay && (decay_ < 1.0 || p.env <= sustain_))
        p.stage = Stage::Sustained;
    if (p.stage == Stage::Sustained)
        p.env = sustain_;
    if (p.stage == Stage::Released && release_ < 1.0)
        p.env = 0.0;
}

// One frame of the envelope: each stage moves at the rate that takes it
// its time, attack from 0 to 15, decay from 15 to the sustain level, release
// from where the key left it to 0, so a change of a time is heard at once.
void Engine::StepEnvelope(Program& p) const noexcept {
    switch (p.stage) {
    case Stage::Attack:
        p.env += 15.0 / attack_;
        if (p.env >= 15.0) {
            p.env = 15.0;
            p.stage = Stage::Decay;
        }
        break;
    case Stage::Decay:
        p.env -= (15.0 - sustain_) / decay_;
        if (p.env <= sustain_)
            p.stage = Stage::Sustained;
        break;
    case Stage::Released:
        p.env -= p.from / release_;
        if (p.env < 0.0)
            p.env = 0.0;
        break;
    default:
        break;
    }
    Settle(p);
}

// The pitch the set's driver asks of a unit at the nominal clock: a unit
// whose crystal runs off plays as far off.
double Engine::Pitch(const Slot& n, int set) const {
    const Program& p = n.set[set];
    double semitones = n.key + transpose_ + (fine_ + tune_) / 100.0 + p.glide + bend_ * bendRange_;
    const double depth = vibratoDepth_ + 50.0 * wheel_;
    if (depth > 0.0 && p.frames > vibratoDelay_)
        semitones += depth / 100.0 * std::sin(6.283185307179586 * p.phase);
    return 440.0 * std::pow(2.0, (semitones - 69.0) / 12.0);
}

// The normalised level [0, 1] the set's envelope has reached, at the level
// and touch.
double Engine::Gain(const Slot& n, int set) const noexcept {
    const double touch = 1.0 - velocity_ * (1.0 - n.velocity / 127.0);
    const Program& p = n.set[set];
    const double gain = (p.full ? 1.0 : (level_ / 15.0) * touch) * (p.env / 15.0);
    return gain < 0.0 ? 0.0 : gain > 1.0 ? 1.0 : gain;
}

// The slot's settings now on the set's unit. False when its pitch is out of
// its channel's reach, and then the period it last wrote.
bool Engine::Compose(int slot, int set, ChannelSettings& s) const {
    const Slot& n = slots_[slot];
    const Machine& m = machines_[UnitOf(slot, set)];
    const int ch = Channel(slot);
    const Program& p = n.set[set];
    s.voice = n.voice;
    s.shape = shape_ % m.Shapes(ch, n.voice);
    s.mod = mod_;
    s.sweep = sweep_;
    s.sweepPace = sweepPace_;
    s.spread = spread_;
    if (p.chip) {
        s.volume = ChipVolume(p, UnitOf(slot, set));
        s.pace = p.chipPace;
        s.up = p.chipUp;
        s.loop = p.chipLoop;
    } else {
        s.volume = m.QuantizeVolume(ch, n.voice, Gain(n, set));
    }
    if (m.PeriodFor(ch, n.voice, Pitch(n, set), s.shape, s.period))
        return true;
    s.period = n.started ? n.set[set].written.period : s.period;
    return false;
}

// Sends the set's unit the slot's settings as a start or a change; it
// hears only what differs from what it last heard.
void Engine::Send(int slot, int set, const ChannelSettings& s, bool start) {
    Program& p = slots_[slot].set[set];
    Machine& machine = machines_[UnitOf(slot, set)];
    const int ch = Channel(slot);
    if (start)
        machine.Start(ch, s);
    else if (!Same(s, p.written))
        machine.Change(ch, s);
    p.written = s;
}

// The player moves a note to the fallback part when its own part cannot
// reach the pitch, and back once it can, keeping the note's envelope, glide
// and vibrato on every set. Prefer the same machine, then a free compatible
// channel on another one. A bend must not steal another held note: if all
// the fallbacks are busy, the note waits silently at its present slot until
// the pitch or room changes. True when the note moved; the first set's
// driver decides for both.
bool Engine::HandOff(int slot) {
    const Slot& n = slots_[slot];
    const int board = BoardOf(slot);
    const Machine& m = machines_[board];
    const int ch = Channel(slot);
    const double hz = Pitch(n, 0);
    std::int64_t period;
    if (!n.fallback && m.PeriodFor(ch, n.voice, hz, shape_ % m.Shapes(ch, n.voice), period))
        return false;
    const Part primary = m.PartOf(part_);
    const Part dest = n.fallback ? primary : m.FallbackOf(primary);
    if (dest.count == 0 || !m.PeriodFor(dest.first, dest.voice, hz, shape_ % m.Shapes(dest.first, dest.voice), period))
        return false;
    for (int i = 0; i < boards_; ++i) {
        const int base = ((board + i) % boards_) * Machine::kMaxChannels;
        for (int c = dest.first; c < dest.first + dest.count; ++c) {
            const int next = base + c;
            if (slots_[next].busy)
                continue;
            slots_[next] = n;
            slots_[next].voice = dest.voice;
            slots_[next].fallback = !n.fallback;
            if (slot == mono_)
                mono_ = next;
            End(slot);
            for (int set = 0; set < Sets(); ++set) {
                if (slots_[next].set[set].ended)
                    continue;
                Program& p = slots_[next].set[set];
                if (p.restart) {
                    p.restart = false;
                    BeginChip(next, set);
                }
                if (p.chip && machines_[UnitOf(next, set)].EnvelopeOf(c).paces == 0)
                    ToDriver(next, set);
                ChannelSettings s;
                Compose(next, set, s);
                Send(next, set, s, true);
            }
            return true;
        }
    }
    return false;
}

// The set's unit is sent the note as it stands; a pitch its channel cannot
// reach is written silent, at the period it last had.
void Engine::WriteSet(int slot, int set) {
    Program& p = slots_[slot].set[set];
    // A step of the arpeggio that starts the envelope again starts the
    // chip's unit again, by a trigger.
    bool start = false;
    if (p.restart) {
        p.restart = false;
        BeginChip(slot, set);
        start = p.chip;
    }
    ChannelSettings s;
    if (!Compose(slot, set, s))
        s.volume = 0, s.pace = 0;
    Send(slot, set, s, start);
}

// A command to every set's unit at once.
void Engine::Write(int slot) {
    if (HandOff(slot))
        return;
    for (int set = 0; set < Sets(); ++set)
        if (!slots_[slot].set[set].ended)
            WriteSet(slot, set);
}

std::int64_t Engine::Pull(int set) const noexcept {
    if (!double_)
        return 0;
    const double cents = set == 0 ? -detune_ / 2.0 : detune_ / 2.0;
    return static_cast<std::int64_t>(std::floor((std::pow(2.0, cents / 1200.0) - 1.0) * static_cast<double>(Timebase::kTrimUnit) + 0.5));
}

// A unit's driver counts its frames from the unit's power-on: its next
// frame is the first boundary after now.
void Engine::Pace(int unit) noexcept {
    const std::int64_t now = machines_[unit].Now();
    tickNum_[unit] = periodNum_ * (now * periodDen_ / periodNum_ + 1);
}

// A unit's frame, `at` into the block (in 2^-kSongBits host samples). On
// the first unit's, the player's arpeggio, whose every step starts the
// envelope again on every set; then, for each note on this unit, its
// driver's envelope, glide, vibrato and pitch wheel.
void Engine::Tick(int unit, std::int64_t at) {
    tickNum_[unit] += periodNum_;
    const int set = unit / kMaxBoards, board = unit % kMaxBoards;
    int restarted = -1;
    if (unit == 0 && mode_ == 2 && heldCount_ > 0 && (sync_ ? ArpDue(at) : ++arpCount_ >= arpRate_)) {
        arpCount_ = 0;
        const Key& next = NextArpKey(arpRank_);
        arpKey_ = next.key, arpRank_ = ArpRank(next);
        if (mono_ >= 0 && slots_[mono_].gate) {
            // Without Arp retrigger a step moves the pitch only (LSDj keeps
            // the two apart: its chord command moves the pitch, and a
            // retrigger is another command).
            if (arpRetrigger_)
                Restart(slots_[mono_]);
            Retarget(next.key, next.velocity, false);
            restarted = arpRetrigger_ ? mono_ : -1;
        } else {
            mono_ = StartNote(next.key, next.velocity);
            restarted = mono_;
        }
    }
    int writes[Machine::kMaxChannels];
    int count = 0;
    for (int ch = 0; ch < Machine::kMaxChannels; ++ch) {
        const int s = board * Machine::kMaxChannels + ch;
        Slot& n = slots_[s];
        Program& p = n.set[set];
        if (!n.busy || p.ended)
            continue;
        ++p.frames;
        if (p.frames > vibratoDelay_) {
            p.phase += vibratoStep_;
            p.phase -= std::floor(p.phase);
        }
        if (p.glideLeft > 0) {
            p.glide -= p.glide / p.glideLeft;
            --p.glideLeft;
        }
        // A step's new envelope starts on this frame, as a new note's does.
        if (s != restarted)
            StepEnvelope(p);
        if (p.chip ? !n.gate && !p.chipUp && !p.chipLoop && ChipVolume(p, unit) == 0
                   : p.env <= 0.0 && p.stage == Stage::Released) {
            EndSet(s, set);
            continue;
        }
        writes[count++] = s;
    }
    // A write can move a note to a free slot. Finish every envelope first
    // so no note advances twice (or loses a frame) when it changes channels.
    for (int i = 0; i < count; ++i)
        if (set != 0 || !HandOff(writes[i]))
            WriteSet(writes[i], set);
    if (retryPitch_)
        RetryHeldPitches();
}

void Engine::Process(float* left, float* right, int frames) {
    const int most = StepBuffer::kMaxFrames / oversampling_;
    while (frames > 0) {
        const int n = frames < most ? frames : most;
        const int m = n * oversampling_;   // at the machines' rate
        for (float* bus : bus_)
            for (int i = 0; i < m; ++i)
                bus[i] = 0.0f;
        std::int64_t end[2 * kMaxBoards];
        for (int u = 0; u < 2 * kMaxBoards; ++u)
            end[u] = Active(u) ? machines_[u].CycleAt(m) : 0;
        // The units' frames in the order they fall. Every unit is brought to
        // the moment of the one whose frame it is, so what the player sends
        // then reaches each unit at that moment on its own clock.
        for (;;) {
            int due = -1;
            std::int64_t at = 0;
            for (int u = 0; u < 2 * kMaxBoards; ++u) {
                if (!Active(u) || NextTick(u) >= end[u])
                    continue;
                const std::int64_t position = machines_[u].Position(NextTick(u));
                if (due < 0 || position < at)
                    due = u, at = position;
            }
            if (due < 0)
                break;
            for (int u = 0; u < 2 * kMaxBoards; ++u) {
                if (!Active(u) || u == due)
                    continue;
                const std::int64_t cycle = machines_[u].CycleAtPosition(at);
                if (cycle > machines_[u].Now())
                    machines_[u].RunTo(cycle);
            }
            const std::int64_t tick = NextTick(due);
            machines_[due].RunTo(tick);
            machines_[due].ServiceAt(tick);
            Tick(due, (at << (kSongBits - StepBuffer::kSubBits)) / oversampling_);
        }
        // The arpeggio's next step, from the next block's start.
        if (sync_ && mode_ == 2 && heldCount_ > 0)
            arpDue_ -= static_cast<std::int64_t>(n) << kSongBits;
        if (song_ >= 0.0)
            song_ += n * tempo_ / 60.0 / rate_;
        for (int u = 0; u < 2 * kMaxBoards; ++u)
            if (Active(u))
                machines_[u].Render(m, path_, bus_[u < kMaxBoards ? 0 : 2], bus_[u < kMaxBoards ? 1 : 3]);
        const auto mix = [&](int i, float g) {
            if (double_) {
                // Each stereo set folds to its side at the single-set level.
                const float half = 0.5f * g;
                bus_[0][i] = half * (bus_[0][i] + bus_[1][i]);
                bus_[1][i] = half * (bus_[2][i] + bus_[3][i]);
            } else {
                bus_[0][i] *= g;
                bus_[1][i] *= g;
            }
        };
        const int ramped = m < trimFramesLeft_ ? m : trimFramesLeft_;
        const int elapsed = kTrimRampFrames * oversampling_ - trimFramesLeft_;
        for (int i = 0; i < ramped; ++i)
            mix(i, trimStart_ + trimStep_ * static_cast<float>(elapsed + i + 1));
        trimFramesLeft_ -= ramped;
        for (int i = ramped; i < m; ++i)
            mix(i, static_cast<float>(trim_));
        decimator_.Process(bus_[0], bus_[1], n);
        const float gain = static_cast<float>(gain_);
        for (int i = 0; i < n; ++i) {
            std::array<float, 2> frame { bus_[0][i] * gain, bus_[1][i] * gain };
            stage_.Process(frame);
            left[i] = frame[0];
            right[i] = frame[1];
        }
        left += n, right += n, frames -= n;
    }
}

} // namespace protocodus::reboot
