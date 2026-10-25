// SPDX-License-Identifier: GPL-3.0-or-later
#include "WheelStep.hpp"

namespace superior::ui {

namespace {
constexpr int kWheelNotch = 120;
} // namespace

int pageStepForWheel(int angleDelta) noexcept {
    if (angleDelta <= -kWheelNotch) {
        return 1; // wheel down scrolls forward
    }
    if (angleDelta >= kWheelNotch) {
        return -1;
    }
    return 0;
}

} // namespace superior::ui
