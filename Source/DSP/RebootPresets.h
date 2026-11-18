#pragma once

// The factory bank every front end and the demo renderer read: four sounds
// of our own on each board, named for what they do, never after a game, a
// console or a chip. Times are in ms (RebootControls.h). A preset is the Init patch (RebootControls.h) with the
// settings it lists.

#include "RebootControls.h"

#include <array>

namespace protocodus::reboot {

struct Setting {
    int control = kControlCount; // kControlCount: unused
    double value = 0.0;
};

struct Preset {
    const char* name;
    std::array<Setting, 24> settings;

    Controls ToControls() const {
        Controls c;
        for (const Setting& s : settings)
            if (s.control < kControlCount)
                c[s.control] = s.value;
        return c;
    }
};

inline constexpr std::array<Preset, 8> kPresets { {
    // Handheld.
    { "Square Lead", { { { kLevel, 13 }, { kDecay, 130 }, { kSustain, 11 }, { kRelease, 550 },
                         { kVibratoDepth, 12 }, { kVibratoDelay, 250 } } } },
    { "Wave Bass", { { { kMode, 1 }, { kGlide, 40 }, { kPart, 1 }, { kShape, 1 }, { kLevel, 15 },
                       { kSustain, 15 }, { kRelease, 250 }, { kTranspose, -12 } } } },
    { "Pocket Arp", { { { kRevision, 1 }, { kMode, 2 }, { kArpRate, 2 }, { kShape, 1 }, { kDecay, 100 },
                        { kSustain, 8 }, { kRelease, 400 } } } },
    { "Static Snare", { { { kPart, 2 }, { kShape, 0 }, { kLevel, 15 }, { kDecay, 500 }, { kSustain, 0 },
                          { kRelease, 0 }, { kVelocity, 0.8 } } } },
    // Console.
    { "Triangle Bass", { { { kBoard, 1 }, { kMode, 1 }, { kPart, 1 }, { kLevel, 15 }, { kSustain, 15 },
                           { kRelease, 0 }, { kTranspose, -12 } } } },
    { "Pulse Glide", { { { kBoard, 1 }, { kMode, 1 }, { kGlide, 120 }, { kVibratoDepth, 20 },
                         { kVibratoDelay, 300 }, { kShape, 1 }, { kDecay, 330 }, { kSustain, 10 },
                         { kRelease, 500 }, { kDouble, 1 }, { kDetune, 12 } } } },
    { "Saw Lead", { { { kBoard, 1 }, { kRevision, 1 }, { kExpansion, 1 }, { kMode, 1 }, { kGlide, 30 },
                      { kVibratoDepth, 10 }, { kVibratoDelay, 200 }, { kPart, 4 }, { kLevel, 13 },
                      { kSustain, 15 }, { kRelease, 750 } } } },
    { "Disk Organ", { { { kBoard, 1 }, { kRevision, 1 }, { kExpansion, 4 }, { kBoards, 4 }, { kPart, 3 },
                        { kShape, 5 }, { kLevel, 15 }, { kAttack, 250 }, { kSustain, 15 }, { kRelease, 1000 },
                        { kMod, 12 } } } },
} };

} // namespace protocodus::reboot
