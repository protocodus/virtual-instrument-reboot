// The engine as the shared demo renderer plays it (PresetDemoRenderer.h),
// for every tool that renders MIDI through Reboot.
#pragma once

#include "DSP/RebootEngine.h"
#include "PresetDemoRenderer.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

class RebootVoice final : public protocodus::demos::Voice {
public:
    RebootVoice(const protocodus::reboot::Controls& controls, double sampleRate)
        : engine(std::make_unique<protocodus::reboot::Engine>()) {
        engine->Prepare(sampleRate);
        engine->SetControls(controls);
    }

    // Notes, controllers and the pitch wheel go to the engine as a plug-in
    // hands them over; anything else in a phrase is an error, not a
    // silently dropped event.
    void Midi(const std::uint8_t* bytes, int size) override {
        const int kind = bytes[0] & 0xF0;
        if (kind != 0x80 && kind != 0x90 && kind != 0xB0 && kind != 0xE0)
            throw std::runtime_error("unsupported MIDI message " + std::to_string(bytes[0]));
        engine->Midi(bytes, size);
    }

    void Render(float* left, float* right, int frames) override { engine->Process(left, right, frames); }

    protocodus::reboot::Engine& Engine() { return *engine; }

private:
    std::unique_ptr<protocodus::reboot::Engine> engine;
};
