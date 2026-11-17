cmake_minimum_required(VERSION 3.22)

project(Reboot VERSION 0.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Reboot is Rack-first and has no plug-in yet: this builds the JUCE-free
# engine, its offline tools and its tests.
option(REBOOT_BUILD_TOOLS "Build the offline demo renderer" ON)
option(REBOOT_ENABLE_SANITIZERS "Instrument development builds with ASan and UBSan" OFF)
option(REBOOT_BUILD_FUZZER "Build RebootDspFuzz as a Clang libFuzzer target" OFF)
option(REBOOT_REQUIRE_PYTHON_TESTS "Fail configuration when the Python checks cannot run" OFF)

include(CTest)

if(REBOOT_ENABLE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "REBOOT_ENABLE_SANITIZERS requires Clang or GCC")
    endif()
    add_compile_options(-fsanitize=address,undefined,float-cast-overflow
                        -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined,float-cast-overflow)
endif()

if(NOT MSVC)
    # The Rack toolchain never joins a multiply and an add into one rounding,
    # and neither does this build, so a render is the same bits wherever it
    # is built.
    add_compile_options(-ffp-contract=off)
endif()

set(REBOOT_DSP_SOURCES Source/DSP/RebootEngine.cpp)
set(REBOOT_DSP_HEADERS
    Source/DSP/Boards/Board.h
    Source/DSP/Boards/Console.h
    Source/DSP/Boards/Handheld.h
    Source/DSP/Boards/Micro.h
    Source/DSP/Boards/Psg.h
    Source/DSP/Chips/Ay8910.h
    Source/DSP/Chips/Beeper.h
    Source/DSP/Chips/Famicom.h
    Source/DSP/Chips/GameBoyApu.h
    Source/DSP/Chips/Lfsr.h
    Source/DSP/Chips/NesApu.h
    Source/DSP/Chips/Sn76489.h
    Source/DSP/Chips/Timer.h
    Source/DSP/Driver/Machine.h
    Source/DSP/Driver/Waves.h
    Source/DSP/OutputStage.h
    Source/DSP/RebootControls.h
    Source/DSP/RebootEngine.h
    Source/DSP/RebootPresets.h
    Source/DSP/Render/BlepKernel.h
    Source/DSP/Render/Decimator.h
    Source/DSP/Render/Filters.h
    Source/DSP/Render/HalfbandKernel.h
    Source/DSP/Render/StepBuffer.h
    Source/DSP/Render/Timebase.h)

if(MSVC)
    set(REBOOT_WARNINGS /W4 /permissive-)
else()
    set(REBOOT_WARNINGS -Wall -Wextra -Wpedantic -Wshadow)
endif()

add_library(RebootDSP STATIC ${REBOOT_DSP_SOURCES} ${REBOOT_DSP_HEADERS})
target_include_directories(RebootDSP PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_compile_features(RebootDSP PUBLIC cxx_std_20)
set_target_properties(RebootDSP PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(RebootDSP PRIVATE ${REBOOT_WARNINGS})

# The Rack Extension toolchain is C++17 with -fno-builtin and no exceptions.
# Compiling the shipping engine that way here keeps Source/DSP portable for
# the Rack adapter.
if(NOT MSVC)
    add_library(RebootDSPCxx17 OBJECT ${REBOOT_DSP_SOURCES})
    target_include_directories(RebootDSPCxx17 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    set_target_properties(RebootDSPCxx17 PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_compile_options(RebootDSPCxx17 PRIVATE ${REBOOT_WARNINGS} -Werror -fno-builtin -fno-exceptions)
endif()

find_package(Threads REQUIRED)

if(REBOOT_BUILD_TOOLS)
    # The README demos from Docs/audio/midi (Tools/PresetDemoRenderer.h is
    # the shared portfolio copy; do not edit it here).
    add_executable(RenderRebootPresetDemos Tools/RenderPresetDemos.cpp)
    target_include_directories(RenderRebootPresetDemos PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Tools")
    target_link_libraries(RenderRebootPresetDemos PRIVATE RebootDSP Threads::Threads)
    target_compile_options(RenderRebootPresetDemos PRIVATE ${REBOOT_WARNINGS})
endif()

if(BUILD_TESTING)
    if(REBOOT_REQUIRE_PYTHON_TESTS)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
    else()
        find_package(Python3 COMPONENTS Interpreter)
        if(NOT Python3_Interpreter_FOUND)
            message(WARNING "Python 3 not found: the generator and demo-source checks are not registered")
        endif()
    endif()

    # Each chip against the numbers its documents give: periods, sequences,
    # registers' side effects, and the lazy catch-up against running every
    # event.
    add_executable(RebootChipTests Tests/ChipTests.cpp)
    target_include_directories(RebootChipTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    target_compile_options(RebootChipTests PRIVATE ${REBOOT_WARNINGS})
    add_test(NAME Reboot.Chips COMMAND RebootChipTests)

    # Each board's parts in tune, at their levels, through both paths.
    add_executable(RebootBoardTests Tests/BoardTests.cpp)
    target_include_directories(RebootBoardTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    target_compile_options(RebootBoardTests PRIVATE ${REBOOT_WARNINGS})
    add_test(NAME Reboot.Boards COMMAND RebootBoardTests)

    # The playing layer and the engine around the machines.
    add_executable(RebootEngineTests Tests/EngineTests.cpp)
    target_link_libraries(RebootEngineTests PRIVATE RebootDSP)
    target_compile_options(RebootEngineTests PRIVATE ${REBOOT_WARNINGS})
    add_test(NAME Reboot.Engine COMMAND RebootEngineTests)
    set_tests_properties(Reboot.Engine PROPERTIES TIMEOUT 600)

    # U11: every control reaches held and new notes after long play, and U4:
    # the parameter-extremes matrix, one rotation shard of it here
    # (Tests/ParameterMatrix.h is the shared portfolio copy; do not edit it here).
    add_executable(RebootParameterMatrixTests Tests/ParameterMatrixTests.cpp)
    target_link_libraries(RebootParameterMatrixTests PRIVATE RebootDSP Threads::Threads)
    target_compile_options(RebootParameterMatrixTests PRIVATE ${REBOOT_WARNINGS})
    add_test(NAME Reboot.ParameterReach COMMAND RebootParameterMatrixTests --reach --jobs 4)
    set_tests_properties(Reboot.ParameterReach PROPERTIES LABELS unit TIMEOUT 1800)
    add_test(NAME Reboot.ParameterMatrix COMMAND RebootParameterMatrixTests --rotation --jobs 4)
    set_tests_properties(Reboot.ParameterMatrix PROPERTIES LABELS fuzz TIMEOUT 3600)

    # The Rack's SDK refuses a global whose value is worked out when the
    # module loads. GCC folds library calls on constants and hides them, so
    # Clang is asked, with the SDK's -Wall -Wextra -Werror.
    find_program(REBOOT_CLANGXX NAMES clang++)
    if(REBOOT_CLANGXX)
        add_test(NAME Reboot.NoLoadTimeGlobals
            COMMAND ${REBOOT_CLANGXX} -std=c++17 -fsyntax-only -fno-builtin
                    -Wall -Wextra -Werror -Wglobal-constructors
                    -I${CMAKE_CURRENT_SOURCE_DIR}/Source/DSP
                    ${CMAKE_CURRENT_SOURCE_DIR}/Source/DSP/RebootEngine.cpp)
    endif()

    # U4 input fuzzing. RebootDspFuzzReplay is the harness with
    # FuzzHarness.h's main, replaying Tests/Fixtures/fuzz/ and then seeded
    # inputs; with REBOOT_BUILD_FUZZER (Clang) RebootDspFuzz is a libFuzzer
    # target over coverage-instrumented DSP.
    file(GLOB REBOOT_FUZZ_REGRESSIONS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/fuzz/*")
    add_executable(RebootDspFuzzReplay Tests/DspFuzz.cpp)
    target_link_libraries(RebootDspFuzzReplay PRIVATE RebootDSP)
    target_compile_definitions(RebootDspFuzzReplay PRIVATE PROTOCODUS_FUZZ_MAIN)
    add_test(NAME Reboot.DspFuzzReplay COMMAND RebootDspFuzzReplay -seed=1 -runs=300 ${REBOOT_FUZZ_REGRESSIONS})
    set_tests_properties(Reboot.DspFuzzReplay PROPERTIES TIMEOUT 600)
    if(REBOOT_BUILD_FUZZER)
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            message(FATAL_ERROR "REBOOT_BUILD_FUZZER requires Clang")
        endif()
        add_executable(RebootDspFuzz Tests/DspFuzz.cpp ${REBOOT_DSP_SOURCES})
        target_include_directories(RebootDspFuzz PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
        target_compile_options(RebootDspFuzz PRIVATE
            -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(RebootDspFuzz PRIVATE -fsanitize=fuzzer,address,undefined)
        # libFuzzer fuzzes only when given a corpus directory; it grows in the
        # build tree, and a crash's input is written to the build directory.
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fuzz-corpus")
        add_test(NAME Reboot.DspFuzz
                 COMMAND RebootDspFuzz -seed=1 -max_total_time=60 "-artifact_prefix=${CMAKE_CURRENT_BINARY_DIR}/"
                         "${CMAKE_CURRENT_BINARY_DIR}/fuzz-corpus")
        set_tests_properties(Reboot.DspFuzz PROPERTIES TIMEOUT 600 LABELS fuzz)
    endif()

    if(Python3_Interpreter_FOUND)
        # The generated tables are what their generators write.
        add_test(NAME Reboot.BlepKernel
                 COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/Tools/generate_blep.py"
                         --check "${CMAKE_CURRENT_SOURCE_DIR}/Source/DSP/Render/BlepKernel.h")
        add_test(NAME Reboot.Waves
                 COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/Tools/generate_waves.py"
                         --check "${CMAKE_CURRENT_SOURCE_DIR}/Source/DSP/Driver/Waves.h")
        add_test(NAME Reboot.HalfbandKernel
                 COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/Tools/generate_halfband.py"
                         --check "${CMAKE_CURRENT_SOURCE_DIR}/Source/DSP/Render/HalfbandKernel.h")
        # The committed demo phrases are what the writer writes (G3a).
        add_test(NAME Reboot.DemoMidiSources
                 COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/Tools/write_preset_midi.py" --check)
    endif()

    if(REBOOT_BUILD_TOOLS)
        # Every demo renders finite, audible, under the ceiling, 8-12 s long
        # and bit-identical on a second render.
        add_test(NAME Reboot.AudioDemos
                 COMMAND RenderRebootPresetDemos --jobs 4 --check "${CMAKE_CURRENT_SOURCE_DIR}/Docs/audio/midi")
        set_tests_properties(Reboot.AudioDemos PROPERTIES TIMEOUT 600 LABELS audio)

        # U7: the demos' realtime factor on one thread against the budget in
        # Tools/RenderPresetDemos.cpp. Timing means nothing in an unoptimised
        # or instrumented build, so only Release without sanitizers has it.
        if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT REBOOT_ENABLE_SANITIZERS)
            add_test(NAME Reboot.CpuBudget
                     COMMAND RenderRebootPresetDemos --cpu --json "${CMAKE_CURRENT_BINARY_DIR}/cpu.json"
                             "${CMAKE_CURRENT_SOURCE_DIR}/Docs/audio/midi")
            set_tests_properties(Reboot.CpuBudget PROPERTIES TIMEOUT 900 LABELS perf RUN_SERIAL TRUE)
        endif()
    endif()

    # Every test carries exactly one tier label so CI selects tiers with
    # `ctest -L`; tests not labelled above are unit contracts.
    get_property(REBOOT_TESTS DIRECTORY PROPERTY TESTS)
    foreach(test IN LISTS REBOOT_TESTS)
        get_test_property(${test} LABELS test_labels)
        if(NOT test_labels)
            set_tests_properties(${test} PROPERTIES LABELS unit)
        endif()
    endforeach()
endif()
