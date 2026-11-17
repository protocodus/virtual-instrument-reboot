// Renders the README demos (README G3) from the committed phrases in
// Docs/audio/midi, each through the factory preset its file names, through
// the shipping engine. The contract and the fixed rate, block size and seed
// live in the shared PresetDemoRenderer.h.

#include "DSP/RebootPresets.h"
#include "RebootVoice.h"

#include <memory>

int main(int argc, char** argv) {
    protocodus::demos::Instrument instrument;
    instrument.name = "Reboot";
    for (const auto& preset : protocodus::reboot::kPresets)
        instrument.presets.push_back(preset.name);
    // The output stage's limiter holds every sample at -1.5 dBFS.
    instrument.ceiling = protocodus::OutputStage<2>::kCeiling + 1.0e-6f;
    // U7: three times the slowest demo's median, 08-disk-organ at 0.0080x
    // realtime on one thread of the development container, 2026-11-17 (GCC
    // 13.3, Release). No CI run exists yet: re-pin from the runner's first.
    instrument.cpuBudget = 0.024;
    // The machines are deterministic integer cores, so the seed is not used.
    instrument.open = [](std::size_t preset, double sampleRate, int, std::uint32_t) {
        return std::make_unique<RebootVoice>(protocodus::reboot::kPresets[preset].ToControls(), sampleRate);
    };
    return protocodus::demos::Main(argc, argv, instrument);
}
