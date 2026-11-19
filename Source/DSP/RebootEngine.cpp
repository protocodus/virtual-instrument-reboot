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
    for (Machine& m : machines_)
        m.Prepare(rate_ * oversampling_);
    for (Machine& m : twins_)
        m.Prepare(rate_ * oversampling_);
    for (Decimator& d : decimators_)
        d.Prepare(oversampling_);
    stage_.Prepare(static_cast<double>(rate_));
    for (Slot& s : slots_)
        s = Slot {};
    heldCount_ = 0;
    pedal_ = false;
    bend_ = wheel_ = 0.0;
    Apply(true, true);
    trimHeard_ = trim_;
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
    const Controls old = controls_;
    controls_ = c;
    const auto changed = [&](int i) { return old[i] != c[i]; };
    const bool reset = changed(kBoard) || changed(kRevision) || changed(kExpansion) || changed(kExpansionChannels)
        || changed(kBoards) || changed(kDouble);
    Apply(reset, reset || changed(kMode) || changed(kPart));
    if (changed(kBias) && !reset)
        for (int b = 0; b < boards_; ++b) {
            machines_[b].SetBias(settings_.bias);
            twins_[b].SetBias(settings_.bias);
        }
}

// Reads the controls; with `reset` starts the machines again first, and with
// `revoice` plays the held keys again.
void Engine::Apply(bool reset, bool revoice) {
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
    settings_.revision = Clamp(Round(c[kRevision]), 0, 2);
    settings_.expansion = Clamp(Round(c[kExpansion]), 0, Console::kCartridges - 1);
    settings_.expansionChannels = Clamp(Round(c[kExpansionChannels]), 1, 8);
    settings_.bias = Clamp(Round(c[kBias]), 0, 127);
    boards_ = Clamp(Round(c[kBoards]), 1, kMaxBoards);
    path_ = c[kPath] >= 0.5 ? Path::Line : Path::Pin;
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
    }
    const Machine& m = machines_[0];
    const Part part = m.PartOf(part_);
    trim_ = part.count > 0 ? LevelTrim(m, path_, part_, shape_ % m.Shapes(part.first, part.voice)) : 1.0;
    attack_ = c[kAttack] / 1000.0 * frameHz_;
    decay_ = c[kDecay] / 1000.0 * frameHz_;
    release_ = c[kRelease] / 1000.0 * frameHz_;
    vibratoStep_ = c[kVibratoRate] / frameHz_;
    vibratoDelay_ = Round(c[kVibratoDelay] / 1000.0 * frameHz_);
    glideFrames_ = Round(c[kGlide] / 1000.0 * frameHz_);
    gain_ = std::pow(10.0, c[kVolume] / 20.0);
    if (revoice)
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
            StartNote(held_[i].key, held_[i].velocity);
        break;
    case 1: {
        const Key& top = held_[heldCount_ - 1];
        mono_ = StartNote(top.key, top.velocity);
        break;
    }
    default: {
        const int i = HeldIndex(ArpKey(0));
        arpIndex_ = 0, arpCount_ = 0;
        mono_ = StartNote(held_[i].key, held_[i].velocity);
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

// The `index`-th lowest held key.
int Engine::ArpKey(int index) const noexcept {
    for (int i = 0; i < heldCount_; ++i) {
        int below = 0;
        for (int j = 0; j < heldCount_; ++j)
            below += held_[j].key < held_[i].key;
        if (below == index)
            return held_[i].key;
    }
    return held_[0].key;
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
    Strike(key, Clamp(velocity, 1, 127));
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
    pedal_ = down;
    if (down)
        return;
    for (int i = heldCount_ - 1; i >= 0; --i)
        if (i < heldCount_ && !held_[i].down)
            Lift(held_[i].key);
}

// NaN lands at the bottom of the range.
void Engine::SetBend(double amount) { bend_ = amount > -1.0 ? (amount < 1.0 ? amount : 1.0) : -1.0; }
void Engine::SetWheel(double amount) { wheel_ = amount > 0.0 ? (amount < 1.0 ? amount : 1.0) : 0.0; }
// NaN plays at pitch rather than a semitone flat.
void Engine::SetTune(double cents) { tune_ = std::isfinite(cents) ? std::clamp(cents, -100.0, 100.0) : 0.0; }

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
    for (Decimator& d : decimators_)
        d.Reset();
    stage_.Reset();
    // The machines reset, and the slots cleared with them.
    Apply(true, true);
    trimHeard_ = trim_;
}

int Engine::Sounding() const noexcept {
    int n = 0;
    for (const Slot& s : slots_)
        n += s.busy;
    return n;
}

// A key that was just added to held_.
void Engine::Strike(int key, int velocity) {
    switch (mode_) {
    case 0:
        StartNote(key, velocity);
        break;
    case 1:
        if (mono_ >= 0 && slots_[mono_].gate) {
            Retarget(mono_, key, velocity, true);
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
        arpIndex_ = 0, arpCount_ = 0;
        while (ArpKey(arpIndex_) != key)
            ++arpIndex_;
        if (mono_ >= 0)
            End(mono_);
        mono_ = StartNote(key, velocity);
        break;
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
    switch (mode_) {
    case 0:
        for (int s = 0; s < kSlots; ++s)
            if (slots_[s].busy && slots_[s].gate && slots_[s].key == key)
                ReleaseSlot(s);
        break;
    case 1:
        if (mono_ < 0 || !slots_[mono_].gate)
            break;
        if (heldCount_ == 0) {
            ReleaseSlot(mono_);
        } else {
            const Key& top = held_[heldCount_ - 1];
            if (slots_[mono_].key != top.key)
                Retarget(mono_, top.key, top.velocity, true);
        }
        break;
    default:
        if (heldCount_ == 0) {
            if (mono_ >= 0 && slots_[mono_].gate)
                ReleaseSlot(mono_);
        } else {
            arpIndex_ %= heldCount_;
        }
        break;
    }
}

// Starts `key` on a channel of the part, or of its fallback when the part
// cannot reach it: a free channel, the one freed longest ago, else the
// oldest released note's, else the oldest note's. Returns the slot or -1.
int Engine::StartNote(int key, int velocity) {
    const Machine& m = machines_[0];
    Part part = m.PartOf(part_);
    if (part.count == 0)
        return -1;
    Slot probe;
    probe.key = key;
    const double hz = Pitch(probe, -Spread());
    std::int64_t period = 0;
    if (!m.PeriodFor(part.first, part.voice, hz, shape_ % m.Shapes(part.first, part.voice), period)) {
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
    if (best < 0)
        return -1;

    End(best);
    Slot& n = slots_[best];
    n = Slot {};
    n.busy = true, n.gate = true;
    n.key = key, n.velocity = velocity;
    n.voice = part.voice;
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

// A legato move of a Mono or Arp note to `key`, gliding from where it is
// when asked; on a channel that cannot reach the key, a new note.
void Engine::Retarget(int slot, int key, int velocity, bool glide) {
    Slot& n = slots_[slot];
    const double offset = glide && glideFrames_ > 0 ? n.key + n.glide - key : 0.0;
    n.key = key, n.velocity = velocity;
    n.glide = offset;
    n.glideLeft = offset != 0.0 ? glideFrames_ : 0;
    ChannelSettings s;
    if (!Compose(slot, s, -Spread())) {
        End(slot);
        mono_ = StartNote(key, velocity);
        return;
    }
    Send(slot, s, false);
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

void Engine::Write(int slot) {
    ChannelSettings s;
    Compose(slot, s, -Spread());
    Send(slot, s, false);
}

// The machines' frame: the arpeggio, whose every step starts the envelope
// again, then each note's envelope, glide, vibrato and pitch wheel.
void Engine::Tick() {
    int restarted = -1;
    if (mode_ == 2 && heldCount_ > 0 && ++arpCount_ >= arpRate_) {
        arpCount_ = 0;
        arpIndex_ = (arpIndex_ + 1) % heldCount_;
        const int key = ArpKey(arpIndex_);
        const int velocity = held_[HeldIndex(key)].velocity;
        if (mono_ >= 0 && slots_[mono_].gate) {
            Restart(slots_[mono_]);
            Retarget(mono_, key, velocity, false);
            restarted = mono_;
        }
    }
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
        Write(s);
    }
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
        // The trim moves across the block, so a change of part or shape
        // does not click.
        const float from = static_cast<float>(trimHeard_);
        const float step = static_cast<float>((trim_ - trimHeard_) / m);
        trimHeard_ = trim_;
        if (double_) {
            // Each set's own stereo folds to its side, at the level one
            // side of a single set has.
            for (int i = 0; i < m; ++i) {
                const float g = 0.5f * (from + step * static_cast<float>(i + 1));
                bus_[0][i] = g * (bus_[0][i] + bus_[1][i]);
                bus_[1][i] = g * (bus_[2][i] + bus_[3][i]);
            }
        } else {
            for (int i = 0; i < m; ++i) {
                const float g = from + step * static_cast<float>(i + 1);
                bus_[0][i] *= g;
                bus_[1][i] *= g;
            }
        }
        decimators_[0].Process(bus_[0], n);
        decimators_[1].Process(bus_[1], n);
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
