// U4 input fuzzing of the JUCE-free engine through the shared portfolio
// harness (Tests/FuzzHarness.h is a byte-identical copy of the master; change
// it in reason-rack-extensions). Every input starts from a fresh engine, so a
// saved crash replays alone; commands and rendered frames are bounded
// whatever the input length.
#include "DSP/RebootEngine.h"
#include "DSP/RebootPresets.h"
#include "FuzzHarness.h"

#include <cmath>
#include <memory>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace protocodus::fuzz;
    namespace rb = protocodus::reboot;
    Input input { data, size };

    auto engine = std::make_unique<rb::Engine>();
    // Any host rate at 1x, 2x, 4x or 8x.
    engine->Prepare(input.Rate(), 1 << input.Pick(4));
    rb::Controls controls = rb::kPresets[input.Pick(rb::kPresets.size())].ToControls();
    engine->SetControls(controls);

    constexpr std::size_t maxFrames = 512;
    Sentinels<maxFrames> block;
    for (int command = 0; command < 64 && !input.Empty(); ++command) {
        switch (input.Pick(8)) {
        case 0: {   // any channel message, including truncated ones
            const std::uint8_t bytes[3] = { input.Byte(), input.Byte(), input.Byte() };
            engine->Midi(bytes, static_cast<int>(input.Pick(4)));
            break;
        }
        case 1:
        case 2: {   // a key down or up, twice as often as the rest
            const std::uint8_t bytes[3] = { static_cast<std::uint8_t>((input.Byte() & 1 ? 0x90 : 0x80) | (input.Byte() & 0x0F)),
                                            static_cast<std::uint8_t>(input.Byte() & 0x7F),
                                            static_cast<std::uint8_t>(input.Byte() & 0x7F) };
            engine->Midi(bytes, 3);
            break;
        }
        case 3:
            // One raw write, which may be any float, to any control.
            controls[static_cast<int>(input.Pick(rb::kControlCount))] = input.Scalar() * 100.0f;
            engine->SetControls(controls);
            break;
        case 4:
            controls = rb::kPresets[input.Pick(rb::kPresets.size())].ToControls();
            engine->SetControls(controls);
            break;
        case 5:
            switch (input.Pick(3)) {
                case 0: engine->SetBend(input.Scalar()); break;
                case 1: engine->SetWheel(input.Scalar()); break;
                default: {
                    // The tempo and the song's place ride on the tune's
                    // draw, so the committed crash inputs replay the paths
                    // they found.
                    const double x = input.Scalar();
                    engine->SetTune(x * 200.0 - 100.0);
                    engine->SetTempo(x * 2000.0 - 500.0);
                    engine->SetSongPosition(x * 2.0e9 - 1.0e8);
                    break;
                }
            }
            break;
        case 6:
            // Keys called directly, out of range included.
            if (input.Byte() & 1) engine->NoteOn(static_cast<int>(input.Byte()) - 64, static_cast<int>(input.Byte()) - 64);
            else engine->NoteOff(static_cast<int>(input.Byte()) - 64);
            break;
        default:
            switch (input.Pick(3)) {
            case 0: engine->Panic(); break;
            case 1: engine->ReleaseAll(); break;
            default: engine->SetSustain(input.Byte() & 1); break;
            }
            break;
        }
        const auto frames = 1 + input.Word() % maxFrames;
        block.Arm();
        engine->Process(block.Left(), block.Right(), static_cast<int>(frames));
        block.Check(frames);
        // The output stage's peak limiter holds every sample at -1.5 dBFS,
        // to the few ulps its float gain can add (the matrix's 1e-6).
        constexpr float ceiling = protocodus::OutputStage<2>::kCeiling + 1.0e-6f;
        for (std::size_t i = 0; i < frames; ++i)
            Require(std::fabs(block.Left()[i]) <= ceiling && std::fabs(block.Right()[i]) <= ceiling);
        Require(engine->Sounding() >= 0 && engine->Sounding() <= rb::Engine::kSlots);
    }
    return 0;
}
