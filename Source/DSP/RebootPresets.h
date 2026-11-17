#pragma once

// The factory bank every front end and the demo renderer read: four sounds
// of our own on each board, named for what they do, never after a game, a
// console or a chip. A preset is the Init patch (RebootControls.h) with the
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

inline constexpr std::array<Preset, 16> kPresets { {
    // Handheld.
    { "Square Lead", { { { kALevel, 13 }, { kADecay, 2 }, { kASustain, 11 }, { kARelease, 3 },
                         { kVibratoDepth, 12 }, { kVibratoDelay, 250 } } } },
    { "Wave Bass", { { { kMode, 1 }, { kGlide, 40 }, { kAPart, 1 }, { kAShape, 1 }, { kALevel, 15 },
                       { kARelease, 1 }, { kATranspose, -12 } } } },
    { "Pocket Arp", { { { kRevision, 1 }, { kMode, 2 }, { kArpRate, 2 }, { kAShape, 1 }, { kADecay, 3 },
                        { kASustain, 9 }, { kARelease, 4 } } } },
    { "Static Snare", { { { kAPart, 2 }, { kAShape, 0 }, { kALevel, 15 }, { kADecay, 2 }, { kASustain, 0 },
                          { kARelease, 0 }, { kVelocity, 0.8 } } } },
    // Console.
    { "Triangle Bass", { { { kBoard, 1 }, { kMode, 1 }, { kAPart, 1 }, { kALevel, 15 }, { kARelease, 0 },
                           { kATranspose, -12 } } } },
    { "Pulse Glide", { { { kBoard, 1 }, { kMode, 1 }, { kGlide, 120 }, { kVibratoDepth, 20 },
                         { kVibratoDelay, 300 }, { kAShape, 1 }, { kADecay, 4 }, { kASustain, 10 },
                         { kARelease, 3 }, { kBOn, 1 }, { kBShape, 0 }, { kBLevel, 8 }, { kBDecay, 4 },
                         { kBSustain, 10 }, { kBRelease, 3 }, { kBFine, 12 } } } },
    { "Saw Lead", { { { kBoard, 1 }, { kRevision, 1 }, { kExpansion, 1 }, { kMode, 1 }, { kGlide, 30 },
                      { kVibratoDepth, 10 }, { kVibratoDelay, 200 }, { kAPart, 4 }, { kALevel, 13 },
                      { kARelease, 3 } } } },
    { "Disk Organ", { { { kBoard, 1 }, { kRevision, 1 }, { kExpansion, 4 }, { kBoards, 4 }, { kAPart, 3 },
                        { kAShape, 5 }, { kALevel, 15 }, { kAAttack, 1 }, { kARelease, 4 }, { kAMod, 12 } } } },
    // PSG.
    { "Bell Tones", { { { kBoard, 2 }, { kALevel, 14 }, { kADecay, 2 }, { kASustain, 4 }, { kARelease, 5 },
                        { kVibratoDepth, 8 } } } },
    { "Periodic Bass", { { { kBoard, 2 }, { kMode, 1 }, { kALevel, 15 }, { kARelease, 1 },
                           { kATranspose, -24 } } } },
    { "Noise Burst", { { { kBoard, 2 }, { kAPart, 1 }, { kAShape, 0 }, { kALevel, 15 }, { kADecay, 1 },
                         { kASustain, 0 }, { kARelease, 0 } } } },
    { "Plain Organ", { { { kBoard, 2 }, { kRevision, 2 }, { kBoards, 4 }, { kALevel, 12 }, { kARelease, 1 },
                         { kVibratoDepth, 6 }, { kBOn, 1 }, { kBLevel, 8 }, { kBRelease, 1 },
                         { kBTranspose, 12 } } } },
    // Micro.
    { "Buzz Bass", { { { kBoard, 3 }, { kMode, 1 }, { kAPart, 2 }, { kAShape, 0 }, { kARelease, 0 },
                       { kATranspose, -12 } } } },
    { "One-Bit Melody", { { { kBoard, 3 }, { kRevision, 1 }, { kMode, 1 }, { kAPart, 3 }, { kALevel, 15 },
                            { kARelease, 0 } } } },
    { "Fast Arp", { { { kBoard, 3 }, { kMode, 2 }, { kArpRate, 1 }, { kALevel, 13 }, { kADecay, 2 },
                      { kASustain, 10 }, { kARelease, 3 } } } },
    { "Soft Tones", { { { kBoard, 3 }, { kAAttack, 1 }, { kADecay, 4 }, { kASustain, 10 }, { kARelease, 4 },
                        { kVibratoDepth, 12 }, { kVibratoDelay, 200 } } } },
} };

} // namespace protocodus::reboot
