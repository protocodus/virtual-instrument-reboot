#include "RebootEngine.h"

#include "LevelTrims.h"

#include <algorithm>
#include <cmath>

namespace protocodus::reboot {

namespace {

int Clamp(int v, int lo, int hi) noexcept { return v < lo ? lo : v > hi ? hi : v; }
int Round(double v) noexcept { return static_cast<int>(std::floor(v + 0.5)); }

bool Same(const ChannelSettings& a, const ChannelSettings& b) noexcept {
    return a.voice == b.voice && a.period == b.period && a.volume == b.volume && a.shape == b.shape && a.mod == b.mod;
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
    for (Machine& m : machines_)
        m.Prepare(rate_ * oversampling_, curve5B);
    for (Machine& m : twins_)
        m.Prepare(rate_ * oversampling_, curve5B);
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
        || c[kBendRange] != controls_[kBendRange] || c[kDetune] != controls_[kDetune];
    controls_ = c;
    Apply(false);
}

// Compare the effective hardware. A stored setting of an absent cartridge
// must not reset the clock, silence the release tails or retrigger held keys.
void Engine::Apply(bool forceReset) {
    const MachineSettings old = settings_;
    const int oldBoards = boards_, oldMode = mode_, oldPart = part_;
    const bool wasDouble = double_;
    const Controls& c = controls_;
    mode_ = Clamp(Round(c[kMode]), 0, 2);
    arpRate_ = Clamp(Round(c[kArpRate]), 1, 8);
    bendRange_ = Clamp(Round(c[kBendRange]), 0, 12);
    vibratoDepth_ = c[kVibratoDepth];
    velocity_ = c[kVelocity];
    double_ = c[kDouble] >= 0.5;
    detune_ = c[kDetune];
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
        // The machines start at cycle 0 on this block boundary, so their
        // frames fall together.
        for (int b = 0; b < boards_; ++b) {
            machines_[b].Reset(settings_);
            if (double_)
                twins_[b].Reset(settings_);
        }
        machines_[0].FramePeriod(periodNum_, periodDen_);
        frameHz_ = machines_[0].ClockHz() * static_cast<double>(periodDen_) / static_cast<double>(periodNum_);
        tickNum_ = periodNum_;
    } else if (console && old.bias != settings_.bias) {
        for (int b = 0; b < boards_; ++b) {
            machines_[b].SetBias(settings_.bias);
            if (double_)
                twins_[b].SetBias(settings_.bias);
        }
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
    attack_ = c[kAttack] / 1000.0 * frameHz_;
    decay_ = c[kDecay] / 1000.0 * frameHz_;
    release_ = c[kRelease] / 1000.0 * frameHz_;
    vibratoStep_ = c[kVibratoRate] / frameHz_;
    vibratoDelay_ = Round(c[kVibratoDelay] / 1000.0 * frameHz_);
    glideFrames_ = Round(c[kGlide] / 1000.0 * frameHz_);
    gain_ = std::pow(10.0, c[kVolume] / 20.0);
    if (reset || oldMode != mode_ || oldPart != part_)
        Revoice(reset);
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
        const Key& first = NextArpKey(-1);
        arpKey_ = first.key, arpCount_ = 0;
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

// Next ascending pitch, wrapping to the lowest. Live chord edits do not
// move the cursor: it remembers the last played pitch, not its array index.
const Engine::Key& Engine::NextArpKey(int after) const noexcept {
    const Key* lowest = &held_[0];
    const Key* next = nullptr;
    for (int i = 0; i < heldCount_; ++i) {
        const Key& k = held_[i];
        if (k.key < lowest->key)
            lowest = &k;
        if (k.key > after && (!next || k.key < next->key))
            next = &k;
    }
    return next ? *next : *lowest;
}

void Engine::NoteOn(int key, int velocity) {
    key = Clamp(key, 0, 127);
    if (velocity <= 0) {
        NoteOff(key);
        return;
    }
    if (HeldIndex(key) >= 0)
        Lift(key);
    held_[heldCount_++] = { key, Clamp(velocity, 1, 127), true };
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
        arpKey_ = key, arpCount_ = 0;
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
    Part part = m.PartOf(part_);
    if (part.count == 0)
        return -1;
    Slot probe;
    probe.key = key;
    const double hz = Pitch(probe, -Spread());
    std::int64_t period = 0;
    const bool fallback = !m.PeriodFor(part.first, part.voice, hz, shape_ % m.Shapes(part.first, part.voice), period);
    if (fallback) {
        part = m.FallbackOf(part);
        if (part.count == 0
            || !m.PeriodFor(part.first, part.voice, hz, shape_ % m.Shapes(part.first, part.voice), period))
            return -1;
    }

    // A slot's rank: 0 free, 1 released, 2 held.
    int best = -1, bestRank = 3;
    std::uint64_t bestAge = 0;
    for (int b = 0; b < boards_; ++b)
        for (int ch = part.first; ch < part.first + part.count; ++ch) {
            const int s = b * Machine::kMaxChannels + ch;
            const Slot& n = slots_[s];
            const int rank = !n.busy ? 0 : n.gate ? 2 : 1;
            if (rank < bestRank || (rank == bestRank && n.age < bestAge))
                best = s, bestRank = rank, bestAge = n.age;
        }
    if (best < 0 || (!maySteal && bestRank != 0))
        return -1;

    End(best);
    Slot& n = slots_[best];
    n = Slot {};
    n.busy = true, n.gate = true;
    n.key = key, n.velocity = velocity;
    n.voice = part.voice;
    n.fallback = fallback;
    n.age = ++age_;
    // A note at sustain 0 keeps its channel while the key holds it, silent,
    // so that raising the sustain brings it back.
    Settle(n);
    ChannelSettings s;
    Compose(best, s, -Spread());
    Send(best, s, true);
    n.started = true;
    return best;
}

// A legato move of a Mono or Arp note to `key`, gliding from its current pitch.
void Engine::Retarget(int key, int velocity, bool glide) {
    Slot& n = slots_[mono_];
    const double offset = glide && glideFrames_ > 0 ? n.key + n.glide - key : 0.0;
    n.key = key, n.velocity = velocity;
    n.glide = offset;
    n.glideLeft = offset != 0.0 ? glideFrames_ : 0;
    Write(mono_);
}

// The envelope from the start, as a new note's: the arpeggio's steps.
void Engine::Restart(Slot& n) const noexcept {
    n.stage = Stage::Attack;
    n.env = 0.0;
    Settle(n);
}

void Engine::ReleaseSlot(int slot) {
    Slot& n = slots_[slot];
    n.gate = false;
    n.stage = Stage::Released;
    n.from = n.env;
    Settle(n);
    if (n.env <= 0.0)
        End(slot);
}

void Engine::End(int slot) {
    Slot& n = slots_[slot];
    if (!n.busy)
        return;
    if (n.started) {
        machines_[BoardOf(slot)].Stop(Channel(slot));
        if (double_)
            twins_[BoardOf(slot)].Stop(Channel(slot));
    }
    if (mono_ == slot)
        mono_ = -1;
    n.busy = false, n.gate = false, n.started = false;
    n.age = ++age_;
    if (mode_ == 0)
        retryPitch_ = true;
}

// The envelope's moves that take no frame. A stage shorter than a frame
// takes none (ours: the driver moves on the frame).
void Engine::Settle(Slot& n) const noexcept {
    if (n.stage == Stage::Attack && attack_ < 1.0) {
        n.env = 15.0;
        n.stage = Stage::Decay;
    }
    if (n.stage == Stage::Decay && (decay_ < 1.0 || n.env <= sustain_))
        n.stage = Stage::Sustained;
    if (n.stage == Stage::Sustained)
        n.env = sustain_;
    if (n.stage == Stage::Released && release_ < 1.0)
        n.env = 0.0;
}

// One frame of the envelope: each stage moves at the rate that takes it
// its time, attack from 0 to 15, decay from 15 to the sustain level, release
// from where the key left it to 0, so a change of a time is heard at once.
void Engine::StepEnvelope(Slot& n) const noexcept {
    switch (n.stage) {
    case Stage::Attack:
        n.env += 15.0 / attack_;
        if (n.env >= 15.0) {
            n.env = 15.0;
            n.stage = Stage::Decay;
        }
        break;
    case Stage::Decay:
        n.env -= (15.0 - sustain_) / decay_;
        if (n.env <= sustain_)
            n.stage = Stage::Sustained;
        break;
    case Stage::Released:
        n.env -= n.from / release_;
        if (n.env < 0.0)
            n.env = 0.0;
        break;
    default:
        break;
    }
    Settle(n);
}

double Engine::Pitch(const Slot& n, double cents) const {
    double semitones = n.key + transpose_ + (fine_ + cents + tune_) / 100.0 + n.glide + bend_ * bendRange_;
    const double depth = vibratoDepth_ + 50.0 * wheel_;
    if (depth > 0.0 && n.frames > vibratoDelay_)
        semitones += depth / 100.0 * std::sin(6.283185307179586 * n.phase);
    return 440.0 * std::pow(2.0, (semitones - 69.0) / 12.0);
}

// The normalised level [0, 1] the envelope has reached, at the level and touch.
double Engine::Gain(const Slot& n) const noexcept {
    const double touch = 1.0 - velocity_ * (1.0 - n.velocity / 127.0);
    const double gain = (level_ / 15.0) * touch * (n.env / 15.0);
    return gain < 0.0 ? 0.0 : gain > 1.0 ? 1.0 : gain;
}

// The slot's settings now. False when its pitch is out of its channel's
// reach, and then the period it last wrote.
bool Engine::Compose(int slot, ChannelSettings& s, double cents) const {
    const Slot& n = slots_[slot];
    const Machine& m = machines_[BoardOf(slot)];
    const int ch = Channel(slot);
    s.voice = n.voice;
    s.shape = shape_ % m.Shapes(ch, n.voice);
    s.mod = mod_;
    s.volume = m.QuantizeVolume(ch, n.voice, Gain(n));
    if (m.PeriodFor(ch, n.voice, Pitch(n, cents), s.shape, s.period))
        return true;
    s.period = n.started ? n.written.period : s.period;
    return false;
}

// Sends the slot's settings to its machine as a start or a change, and with
// Double the same note half the detune up to the Double set's machine;
// each hears only what differs from what it last heard.
void Engine::Send(int slot, const ChannelSettings& s, bool start) {
    Slot& n = slots_[slot];
    const int ch = Channel(slot);
    Machine& machine = machines_[BoardOf(slot)];
    if (start)
        machine.Start(ch, s);
    else if (!Same(s, n.written))
        machine.Change(ch, s);
    n.written = s;
    if (!double_)
        return;
    // Where the detune takes the note out of the channel's reach, the
    // Double set plays the first set's period.
    ChannelSettings t;
    if (!Compose(slot, t, Spread()))
        t = s;
    Machine& twin = twins_[BoardOf(slot)];
    if (start)
        twin.Start(ch, t);
    else if (!Same(t, n.twin))
        twin.Change(ch, t);
    n.twin = t;
}

// Preserve the note's envelope, glide and vibrato across a channel handoff.
// Prefer the same machine, then a free compatible channel on another one.
// A bend must not steal another held note: if all the fallbacks are busy,
// the note waits silently at its present slot until the pitch or room changes.
void Engine::Write(int slot) {
    const Slot& n = slots_[slot];
    const int board = BoardOf(slot);
    const Machine& m = machines_[board];
    ChannelSettings s;
    const bool fits = Compose(slot, s, -Spread());
    if (n.fallback || !fits) {
        const Part primary = m.PartOf(part_);
        const Part dest = n.fallback ? primary : m.FallbackOf(primary);
        std::int64_t period;
        if (dest.count > 0 && m.PeriodFor(dest.first, dest.voice, Pitch(n, -Spread()),
                                          shape_ % m.Shapes(dest.first, dest.voice), period)) {
            for (int i = 0; i < boards_; ++i) {
                const int base = ((board + i) % boards_) * Machine::kMaxChannels;
                for (int ch = dest.first; ch < dest.first + dest.count; ++ch) {
                    const int next = base + ch;
                    if (slots_[next].busy)
                        continue;
                    slots_[next] = n;
                    slots_[next].voice = dest.voice;
                    slots_[next].fallback = !n.fallback;
                    if (slot == mono_)
                        mono_ = next;
                    End(slot);
                    Compose(next, s, -Spread());
                    Send(next, s, true);
                    return;
                }
            }
        }
        if (!fits)
            s.volume = 0;
    }
    Send(slot, s, false);
}

// The machines' frame: the arpeggio, whose every step starts the envelope
// again, then each note's envelope, glide, vibrato and pitch wheel.
void Engine::Tick() {
    int restarted = -1;
    if (mode_ == 2 && heldCount_ > 0 && ++arpCount_ >= arpRate_) {
        arpCount_ = 0;
        const Key& next = NextArpKey(arpKey_);
        arpKey_ = next.key;
        if (mono_ >= 0 && slots_[mono_].gate) {
            Restart(slots_[mono_]);
            Retarget(next.key, next.velocity, false);
        } else {
            mono_ = StartNote(next.key, next.velocity);
        }
        restarted = mono_;
    }
    int writes[kSlots];
    int count = 0;
    for (int s = 0; s < boards_ * Machine::kMaxChannels; ++s) {
        Slot& n = slots_[s];
        if (!n.busy)
            continue;
        ++n.frames;
        if (n.frames > vibratoDelay_) {
            n.phase += vibratoStep_;
            n.phase -= std::floor(n.phase);
        }
        if (n.glideLeft > 0) {
            n.glide -= n.glide / n.glideLeft;
            --n.glideLeft;
        }
        // A step's new envelope starts on this frame, as a new note's does.
        if (s != restarted)
            StepEnvelope(n);
        if (n.env <= 0.0 && n.stage == Stage::Released) {
            End(s);
            continue;
        }
        writes[count++] = s;
    }
    // A write can move a note to a free slot. Finish every envelope first
    // so no note advances twice (or loses a frame) when it changes channels.
    for (int i = 0; i < count; ++i)
        Write(writes[i]);
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
        const std::int64_t end = machines_[0].CycleAt(m);
        for (;;) {
            const std::int64_t tick = (tickNum_ + periodDen_ - 1) / periodDen_;
            std::int64_t next = tick;
            for (int b = 0; b < boards_; ++b) {
                next = Earliest(next, machines_[b].NextEvent());
                if (double_)
                    next = Earliest(next, twins_[b].NextEvent());
            }
            if (next >= end)
                break;
            for (int b = 0; b < boards_; ++b) {
                machines_[b].RunTo(next);
                machines_[b].ServiceAt(next);
                if (double_) {
                    twins_[b].RunTo(next);
                    twins_[b].ServiceAt(next);
                }
            }
            if (next == tick) {
                Tick();
                tickNum_ += periodNum_;
            }
        }
        for (int b = 0; b < boards_; ++b) {
            machines_[b].Render(m, path_, bus_[0], bus_[1]);
            if (double_)
                twins_[b].Render(m, path_, bus_[2], bus_[3]);
        }
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
