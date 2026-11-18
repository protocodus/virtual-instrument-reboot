#pragma once

// The gain that brings each part and shape of each machine to one loudness
// (ours, README How it works): a 50 % square at Engine::kReferenceRms, by
// ITU-R BS.1770's K-weighted mean square over a held C3, C4 and C5. It acts
// after the machine, as a mixing desk's fader would, so the chip, its DACs,
// its mixer and its output stage play as they do; only how loud each part
// arrives changes. Notes a part hands to its fallback (Driver/Machine.h)
// keep the board's own balance against it.
//
// The table is measured through the engine by Tools/RebootLevels.cpp
// (`RebootLevels --write`) and checked by Reboot.Levels.

#include "Driver/Machine.h"
#include "LevelTrimData.h"

namespace protocodus::reboot {

// A row of the table is one machine as it runs, on one path: the handheld's
// two revisions; the front-loader and the PAL console; the Famicom alone and
// with cartridges A, B, D and E; then the Famicom with cartridge C at each
// of its channel counts. Each row holds every part's eight shapes.
constexpr int kTrimVariants = 2 + 2 + 5 + 8;
constexpr int kTrimRows = kTrimVariants * 2;
constexpr int kTrimShapes = 8;

constexpr int TrimVariant(const MachineSettings& s) noexcept {
    if (s.board == BoardKind::Handheld)
        return s.revision;
    if (s.revision != Console::kFamicom)
        return s.revision == Console::kPal ? 3 : 2;
    switch (s.expansion) {
    case Console::kN163: return 9 + s.expansionChannels - 1;
    case Console::kFds: return 7;
    case Console::k5B: return 8;
    default: return 4 + s.expansion;   // none, cartridges A and B
    }
}

constexpr int TrimIndex(const MachineSettings& s, Path path, int part, int shape) noexcept {
    return ((TrimVariant(s) * 2 + static_cast<int>(path)) * Machine::kParts + part) * kTrimShapes + shape;
}

inline double LevelTrim(const Machine& m, Path path, int part, int shape) noexcept {
    return static_cast<double>(kLevelTrims[TrimIndex(m.Settings(), path, part, shape)]);
}

} // namespace protocodus::reboot
