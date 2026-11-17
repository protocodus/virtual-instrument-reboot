#include "RebootEngine.h"

#include <algorithm>
#include <cmath>

namespace protocodus::reboot {

namespace {

int Clamp(int v, int lo, int hi) noexcept { return v < lo ? lo : v > hi ? hi : v; }
int Round(double v) noexcept { return static_cast<int>(std::floor(v + 0.5)); }

// The swing of one channel at full volume, a 50 % square, at each board's
// mixer output: the handheld's DAC from +1 to -1, the console's pulse at 15
// (Console::kPulse15), the PSG's and the AY's output bit or DAC from 0 to 1
// (Boards/*.h).
double FullSwing(BoardKind kind) noexcept {
    switch (kind) {
    case BoardKind::Handheld: return 2.0;
    case BoardKind::Console: return Console::kPulse15;
    default: return 1.0;
    }
}

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
    for (Group& g : groups_) {
        for (Machine& m : g.machines)
            m.Prepare(rate_ * oversampling_);
        for (Machine& m : g.twins)
            m.Prepare(rate_ * oversampling_);
    }
    for (Decimator& d : decimators_)
        d.Prepare(oversampling_);
    stage_.Prepare(static_cast<double>(rate_));
    for (Slot& s : slots_)
        s = Slot {};
    heldCount_ = 0;
    sustain_ = false;
    bend_ = wheel_ = 0.0;
    Apply(kAllLayers, true);
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
    int resets = 0;
    bool voicing = changed(kMode);
    for (int l = 0; l < kLayers; ++l) {
        const int m = kMachineBase[l];
        // A layer that is off runs no machines, so turning it on starts them.
        if (changed(m + kMBoard) || changed(m + kMRevision) || changed(m + kMExpansion)
            || changed(m + kMExpansionChannels) || changed(m + kMBoards) || changed(kLayerBase[l] + kOn)
            || changed(kDouble))
            resets |= 1 << l;
        voicing = voicing || changed(kLayerBase[l] + kPart);
    }
    Apply(resets, resets != 0 || voicing);
    for (int l = 0; l < kLayers; ++l) {
        Group& g = groups_[l];
        if (changed(kMachineBase[l] + kMBias) && !(resets >> l & 1))
            for (int b = 0; b < g.boards; ++b) {
                g.machines[b].SetBias(g.settings.bias);
                g.twins[b].SetBias(g.settings.bias);
            }
    }
}

// Reads the controls; resets the machines of the layers in `resets` first,
// and with `revoice` plays the held keys again.
void Engine::Apply(int resets, bool revoice) {
    const Controls& c = controls_;
    mode_ = Clamp(Round(c[kMode]), 0, 2);
    arpRate_ = Clamp(Round(c[kArpRate]), 1, 8);
    bendRange_ = Clamp(Round(c[kBendRange]), 0, 12);
    vibratoDepth_ = c[kVibratoDepth];
    velocity_ = c[kVelocity];
    double_ = c[kDouble] >= 0.5;
    detune_ = c[kDetune];
    for (int l = 0; l < kLayers; ++l) {
        const int base = kLayerBase[l];
        Layer& L = layers_[l];
        L.on = c[base + kOn] >= 0.5;
        L.part = Clamp(Round(c[base + kPart]), 0, Machine::kParts - 1);
        L.shape = Clamp(Round(c[base + kShape]), 0, 7);
        L.level = Clamp(Round(c[base + kLevel]), 0, 15);
        L.attack = Clamp(Round(c[base + kAttack]), 0, 30);
        L.decay = Clamp(Round(c[base + kDecay]), 0, 30);
        L.sustain = Clamp(Round(c[base + kSustain]), 0, 15);
        L.release = Clamp(Round(c[base + kRelease]), 0, 30);
        L.transpose = Clamp(Round(c[base + kTranspose]), -24, 24);
        L.fine = c[base + kFine];
        L.mod = Clamp(Round(c[base + kMod]), 0, 63);

        const int m = kMachineBase[l];
        Group& g = groups_[l];
        g.settings.board = static_cast<BoardKind>(Clamp(Round(c[m + kMBoard]), 0, static_cast<int>(BoardKind::Count) - 1));
        g.settings.revision = Clamp(Round(c[m + kMRevision]), 0, 2);
        g.settings.expansion = Clamp(Round(c[m + kMExpansion]), 0, Console::kCartridges - 1);
        g.settings.expansionChannels = Clamp(Round(c[m + kMExpansionChannels]), 1, 8);
        g.settings.bias = Clamp(Round(c[m + kMBias]), 0, 127);
        g.boards = Clamp(Round(c[m + kMBoards]), 1, kMaxBoards);
        g.path = c[m + kMPath] >= 0.5 ? Path::Line : Path::Pin;
        g.swing = 2.0 * kReferenceRms / FullSwing(g.settings.board);
        if (resets >> l & 1) {
            // The layer's machines start at cycle 0 on this block boundary,
            // so their frames fall together.
            for (int b = 0; b < g.boards; ++b) {
                g.machines[b].Reset(g.settings);
                if (double_)
                    g.twins[b].Reset(g.settings);
            }
            g.machines[0].FramePeriod(g.periodNum, g.periodDen);
            g.frameHz = g.machines[0].ClockHz() * static_cast<double>(g.periodDen) / static_cast<double>(g.periodNum);
            g.tickNum = g.periodNum;
        }
        g.vibratoStep = c[kVibratoRate] / g.frameHz;
        g.vibratoDelay = Round(c[kVibratoDelay] / 1000.0 * g.frameHz);
        g.glideFrames = Round(c[kGlide] / 1000.0 * g.frameHz);
    }
    gain_ = std::pow(10.0, c[kVolume] / 20.0);
    if (revoice)
        Revoice(resets);
}

// Ends every note (after a reset, the machines are silent already) and
// plays the held keys again.
void Engine::Revoice(int resets) {
    for (int s = 0; s < kSlots; ++s) {
        if (resets >> LayerOf(s) & 1)
            slots_[s] = Slot {};
        else
            End(s);
    }
    mono_[0] = mono_[1] = -1;
    if (heldCount_ == 0)
        return;
    switch (mode_) {
    case 0:
        for (int i = 0; i < heldCount_; ++i)
            for (int l = 0; l < kLayers; ++l)
                StartNote(l, held_[i].key, held_[i].velocity, 0.0);
        break;
    case 1: {
        const Key& top = held_[heldCount_ - 1];
        for (int l = 0; l < kLayers; ++l)
            mono_[l] = StartNote(l, top.key, top.velocity, 0.0);
        break;
    }
    default: {
        const int i = HeldIndex(ArpKey(0));
        for (int l = 0; l < kLayers; ++l) {
            groups_[l].arpIndex = 0, groups_[l].arpCount = 0;
            mono_[l] = StartNote(l, held_[i].key, held_[i].velocity, 0.0);
        }
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
    if (sustain_) {
        held_[i].down = false;
        return;
    }
    Lift(held_[i].key);
}

void Engine::SetSustain(bool down) {
    sustain_ = down;
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
    sustain_ = false;
    heldCount_ = 0;
    for (int s = 0; s < kSlots; ++s)
        if (slots_[s].busy && slots_[s].gate)
            ReleaseSlot(s);
}

void Engine::Panic() {
    sustain_ = false;
    heldCount_ = 0;
    for (int s = 0; s < kSlots; ++s)
        End(s);
}

void Engine::Reset() {
    sustain_ = false;
    heldCount_ = 0;
    for (Decimator& d : decimators_)
        d.Reset();
    stage_.Reset();
    // The machines reset, and the slots cleared with them.
    Apply(kAllLayers, true);
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
        for (int l = 0; l < kLayers; ++l)
            StartNote(l, key, velocity, 0.0);
        break;
    case 1:
        for (int l = 0; l < kLayers; ++l) {
            const int s = mono_[l];
            if (s >= 0 && slots_[s].gate) {
                Retarget(s, key, velocity, true);
            } else {
                if (s >= 0)
                    End(s);
                mono_[l] = StartNote(l, key, velocity, 0.0);
            }
        }
        break;
    default:
        // Each layer's arpeggio steps on its own machines' frame; a key
        // struck while it runs joins it.
        for (int l = 0; l < kLayers; ++l) {
            if (mono_[l] >= 0 && slots_[mono_[l]].gate)
                continue;
            Group& g = groups_[l];
            g.arpIndex = 0, g.arpCount = 0;
            while (ArpKey(g.arpIndex) != key)
                ++g.arpIndex;
            if (mono_[l] >= 0)
                End(mono_[l]);
            mono_[l] = StartNote(l, key, velocity, 0.0);
        }
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
        for (int l = 0; l < kLayers; ++l) {
            const int s = mono_[l];
            if (s < 0 || !slots_[s].gate)
                continue;
            if (heldCount_ == 0) {
                ReleaseSlot(s);
            } else {
                const Key& top = held_[heldCount_ - 1];
                if (slots_[s].key != top.key)
                    Retarget(s, top.key, top.velocity, true);
            }
        }
        break;
    default:
        if (heldCount_ == 0) {
            for (int l = 0; l < kLayers; ++l)
                if (mono_[l] >= 0 && slots_[mono_[l]].gate)
                    ReleaseSlot(mono_[l]);
        } else {
            for (Group& g : groups_)
                g.arpIndex %= heldCount_;
        }
        break;
    }
}

// Starts `key` on a channel of the layer's part, or of its fallback when the
// part cannot reach it: a free channel, the one freed longest ago, else the
// oldest released note's, else the oldest note's. Returns the slot or -1.
int Engine::StartNote(int layer, int key, int velocity, double glide) {
    const Layer& L = layers_[layer];
    if (!L.on)
        return -1;
    const Group& g = groups_[layer];
    const Machine& m = g.machines[0];
    Part part = m.PartOf(L.part);
    if (part.count == 0)
        return -1;
    Slot probe;
    probe.key = key, probe.layer = layer;
    const double hz = Pitch(probe, -Spread());
    std::int64_t period = 0;
    if (!m.PeriodFor(part.first, part.voice, hz, L.shape % m.Shapes(part.first, part.voice), period)) {
        part = m.FallbackOf(part);
        if (part.count == 0
            || !m.PeriodFor(part.first, part.voice, hz, L.shape % m.Shapes(part.first, part.voice), period))
            return -1;
    }

    // A slot's rank: 0 free, 1 released, 2 held, judged with the slot it
    // would also take over and the note taking it over now.
    const auto rankOf = [&](int s) {
        const Slot& n = slots_[s];
        const Slot& owner = n.claimedBy >= 0 ? slots_[n.claimedBy] : n;
        if (!owner.busy)
            return 0;
        return owner.gate ? 2 : 1;
    };
    int best = -1, bestRank = 3;
    std::uint64_t bestAge = 0;
    for (int b = 0; b < g.boards; ++b) {
        const int first = layer * kGroupSlots + b * Machine::kMaxChannels;
        for (int ch = part.first; ch < part.first + part.count; ++ch) {
            const int s = first + ch;
            const int also = m.Claims(ch, part.voice);
            int rank = rankOf(s);
            if (also >= 0) {
                const int other = rankOf(first + also);
                rank = other > rank ? other : rank;
            }
            const std::uint64_t age = slots_[s].age;
            if (rank < bestRank || (rank == bestRank && age < bestAge))
                best = s, bestRank = rank, bestAge = age;
        }
    }
    if (best < 0)
        return -1;

    End(best);
    if (slots_[best].claimedBy >= 0)
        End(slots_[best].claimedBy);
    const int also = m.Claims(Channel(best), part.voice);
    const int claim = also >= 0 ? best - Channel(best) + also : -1;
    if (claim >= 0) {
        End(claim);
        if (slots_[claim].claimedBy >= 0)
            End(slots_[claim].claimedBy);
    }

    Slot& n = slots_[best];
    n = Slot {};
    n.busy = true, n.gate = true;
    n.key = key, n.velocity = velocity, n.layer = layer;
    n.voice = part.voice;
    n.age = ++age_;
    n.glide = glide;
    n.glideLeft = glide != 0.0 ? g.glideFrames : 0;
    if (claim >= 0) {
        n.claim = claim;
        slots_[claim].claimedBy = best;
    }
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
    const int frames = groups_[n.layer].glideFrames;
    const double offset = glide && frames > 0 ? n.key + n.glide - key : 0.0;
    n.key = key, n.velocity = velocity;
    n.glide = offset;
    n.glideLeft = offset != 0.0 ? frames : 0;
    ChannelSettings s;
    if (!Compose(slot, s, -Spread())) {
        const int layer = n.layer;
        End(slot);
        mono_[layer] = StartNote(layer, key, velocity, 0.0);
        return;
    }
    Send(slot, s, false);
}

void Engine::ReleaseSlot(int slot) {
    Slot& n = slots_[slot];
    n.gate = false;
    n.stage = Stage::Released;
    n.count = 0;
    Settle(n);
    if (n.step == 0)
        End(slot);
}

void Engine::End(int slot) {
    Slot& n = slots_[slot];
    if (!n.busy)
        return;
    if (n.started) {
        MachineOf(slot).Stop(Channel(slot));
        if (double_)
            TwinOf(slot).Stop(Channel(slot));
    }
    if (n.claim >= 0)
        slots_[n.claim].claimedBy = -1;
    for (int& m : mono_)
        if (m == slot)
            m = -1;
    n.busy = false, n.gate = false, n.started = false;
    n.claim = -1;
    n.age = ++age_;
}

// The envelope's moves that take no frame.
void Engine::Settle(Slot& n) const noexcept {
    const Layer& L = layers_[n.layer];
    if (n.stage == Stage::Attack && L.attack == 0) {
        n.step = 15;
        n.stage = Stage::Decay;
        n.count = 0;
    }
    if (n.stage == Stage::Decay && (L.decay == 0 || n.step <= L.sustain))
        n.stage = Stage::Sustained;
    if (n.stage == Stage::Sustained)
        n.step = L.sustain;
    if (n.stage == Stage::Released && L.release == 0)
        n.step = 0;
}

// One frame of the envelope, a volume step every so many frames.
void Engine::StepEnvelope(Slot& n) const noexcept {
    const Layer& L = layers_[n.layer];
    switch (n.stage) {
    case Stage::Attack:
        if (++n.count >= L.attack) {
            n.count = 0;
            if (++n.step >= 15) {
                n.step = 15;
                n.stage = Stage::Decay;
            }
        }
        break;
    case Stage::Decay:
        if (++n.count >= L.decay) {
            n.count = 0;
            --n.step;
        }
        break;
    case Stage::Released:
        if (n.step > 0 && ++n.count >= L.release) {
            n.count = 0;
            --n.step;
        }
        break;
    default:
        break;
    }
    Settle(n);
}

double Engine::Pitch(const Slot& n, double cents) const {
    const Layer& L = layers_[n.layer];
    double semitones = n.key + L.transpose + (L.fine + cents + tune_) / 100.0 + n.glide + bend_ * bendRange_;
    const double depth = vibratoDepth_ + 50.0 * wheel_;
    if (depth > 0.0 && n.frames > groups_[n.layer].vibratoDelay)
        semitones += depth / 100.0 * std::sin(6.283185307179586 * n.phase);
    return 440.0 * std::pow(2.0, (semitones - 69.0) / 12.0);
}

int Engine::Volume(const Slot& n) const noexcept {
    const Layer& L = layers_[n.layer];
    const double touch = 1.0 - velocity_ * (1.0 - n.velocity / 127.0);
    return Clamp(Round(L.level * touch * n.step / 15.0), 0, 15);
}

// The slot's settings now. False when its pitch is out of its channel's
// reach, and then the period it last wrote.
bool Engine::Compose(int slot, ChannelSettings& s, double cents) const {
    const Slot& n = slots_[slot];
    const Layer& L = layers_[n.layer];
    const Machine& m = MachineOf(slot);
    const int ch = Channel(slot);
    s.voice = n.voice;
    s.shape = L.shape % m.Shapes(ch, n.voice);
    s.mod = L.mod;
    s.volume = Volume(n);
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
    if (start)
        MachineOf(slot).Start(ch, s);
    else if (!Same(s, n.written))
        MachineOf(slot).Change(ch, s);
    n.written = s;
    if (!double_)
        return;
    // Where the detune takes the note out of the channel's reach, the
    // Double set plays the first set's period.
    ChannelSettings t;
    if (!Compose(slot, t, Spread()))
        t = s;
    if (start)
        TwinOf(slot).Start(ch, t);
    else if (!Same(t, n.twin))
        TwinOf(slot).Change(ch, t);
    n.twin = t;
}

void Engine::Write(int slot) {
    ChannelSettings s;
    Compose(slot, s, -Spread());
    Send(slot, s, false);
}

// A layer's machines' frame: its arpeggio, then each of its notes'
// envelope, glide, vibrato and pitch wheel.
void Engine::Tick(int layer) {
    Group& g = groups_[layer];
    if (mode_ == 2 && heldCount_ > 0 && ++g.arpCount >= arpRate_) {
        g.arpCount = 0;
        g.arpIndex = (g.arpIndex + 1) % heldCount_;
        const int key = ArpKey(g.arpIndex);
        const int velocity = held_[HeldIndex(key)].velocity;
        if (mono_[layer] >= 0 && slots_[mono_[layer]].gate)
            Retarget(mono_[layer], key, velocity, false);
    }
    const int first = layer * kGroupSlots;
    const int last = first + g.boards * Machine::kMaxChannels;
    for (int s = first; s < last; ++s) {
        Slot& n = slots_[s];
        if (!n.busy)
            continue;
        ++n.frames;
        if (n.frames > g.vibratoDelay) {
            n.phase += g.vibratoStep;
            n.phase -= std::floor(n.phase);
        }
        if (n.glideLeft > 0) {
            n.glide -= n.glide / n.glideLeft;
            --n.glideLeft;
        }
        StepEnvelope(n);
        if (n.step == 0 && n.stage == Stage::Released) {
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
        for (int l = 0; l < kLayers; ++l) {
            if (!layers_[l].on)
                continue;
            Group& g = groups_[l];
            for (float* bus : layerBus_)
                for (int i = 0; i < m; ++i)
                    bus[i] = 0.0f;
            const std::int64_t end = g.machines[0].CycleAt(m);
            for (;;) {
                const std::int64_t tick = (g.tickNum + g.periodDen - 1) / g.periodDen;
                if (tick >= end)
                    break;
                for (int b = 0; b < g.boards; ++b) {
                    g.machines[b].RunTo(tick);
                    if (double_)
                        g.twins[b].RunTo(tick);
                }
                Tick(l);
                g.tickNum += g.periodNum;
            }
            for (int b = 0; b < g.boards; ++b) {
                g.machines[b].Render(m, g.path, layerBus_[0], layerBus_[1]);
                if (double_)
                    g.twins[b].Render(m, g.path, layerBus_[2], layerBus_[3]);
            }
            const float swing = static_cast<float>(g.swing);
            if (double_) {
                // Each set's own stereo folds to its side, at the level one
                // side of a single set has.
                for (int i = 0; i < m; ++i) {
                    bus_[0][i] += 0.5f * swing * (layerBus_[0][i] + layerBus_[1][i]);
                    bus_[1][i] += 0.5f * swing * (layerBus_[2][i] + layerBus_[3][i]);
                }
            } else {
                for (int i = 0; i < m; ++i) {
                    bus_[0][i] += swing * layerBus_[0][i];
                    bus_[1][i] += swing * layerBus_[1][i];
                }
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
