#pragma once

#include <cstdint>

namespace protocodus::reboot {

// A reloading divider in master cycles: `due` is the cycle of its next
// reload, every `period` cycles. While its channel cannot be heard a core
// stops visiting it and counts the reloads it skipped when it is heard
// again, so a silent channel costs nothing and resumes exactly in phase.
struct Divider {
    std::int64_t due = 0;
    std::int64_t period = 1;

    // The reloads at cycles before `now`, which it then stands after.
    std::int64_t CatchUp(std::int64_t now) noexcept {
        if (due >= now)
            return 0;
        const std::int64_t k = (now - due - 1) / period + 1;
        due += k * period;
        return k;
    }
};

inline std::int64_t Earliest(std::int64_t a, std::int64_t b) noexcept { return b < a ? b : a; }

} // namespace protocodus::reboot
