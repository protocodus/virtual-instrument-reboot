#pragma once

// The home micro's beeper: bits 4 (EAR) and 3 (MIC) of the ULA's port $FE,
// which drive one pin, 28, that feeds the speaker and both sockets. The chip
// has no counters of its own; the CPU toggles the bits, so its sound is the
// program's timing (Driver: the ROM's BEEPER loop, below).
//
// Port $FE: "a zero in bit 3 activates the MIC output, whilst a one in bit 4
// activates the EAR output and the internal speaker" (World of Spectrum,
// "Spectrum technical information", the 48K's tech notes, read 2026-11-17).
// Out() is the two bits, EAR high, as the board's pin-level table is indexed.

#include <cstdint>

namespace protocodus::reboot::zx {

// The 48K's CPU clock and the 128K's, in hertz.
constexpr std::int64_t kClock48 = 3500000;
constexpr std::int64_t kClock128 = 3546900;

// The ROM's BEEPER routine ($03B5, The Complete Spectrum ROM Disassembly,
// Logan and O'Hara, SkoolKit edition, read 2026-11-17) flips EAR every
// 4 * HL + 118 T-states, both half-cycles alike, counted from its Z80
// instruction timings: OUT 11, XOR n 7, LD B,H 4, LD C,A 4, BIT 4,A 8, then
// either JR taken 12, LD C,L 4, INC C 4 (the half-cycle path) or JR not taken
// 7, LD A,D 4, OR E 4, JR not taken 7, LD A,C 4, LD C,L 4, DEC DE 6 (the full
// cycle), then JP (IX) 8, L mod 4 NOPs, INC B 4, INC C 4, and the DEC C / JR NZ
// loop of 16 per count with its LD C,$3F, DEC B, JP NZ of 21 per H. Both paths
// come to 4 * HL + 118; the book's own allowance is 120.5. Interrupts are off
// and the ROM is uncontended, so only the OUT itself could be held by the ULA
// (README, Known gaps).
constexpr std::int64_t kBeeperOverhead = 118;
constexpr std::int64_t BeeperHalfPeriod(std::int64_t hl) noexcept { return 4 * hl + kBeeperOverhead; }

class Beeper {
public:
    // The BEEPER loop writes MIC off (bit 3 set) with every OUT, so a beeper
    // at rest sits at EAR low, MIC off.
    void Reset() noexcept { *this = Beeper {}; }

    [[nodiscard]] std::int64_t Now() const noexcept { return now_; }
    [[nodiscard]] int Out() const noexcept { return code_; }

    template <class Sink>
    void Run(std::int64_t to, Sink&) noexcept { now_ = to; }

    // An OUT to port $FE at Now(); only bits 4 and 3 reach the pin.
    template <class Sink>
    void Write(std::uint8_t value, Sink& sink) {
        const int code = (value >> 3) & 3;
        if (code != code_) {
            code_ = code;
            sink(now_);
        }
    }

private:
    std::int64_t now_ = 0;
    int code_ = 1;
};

} // namespace protocodus::reboot::zx
