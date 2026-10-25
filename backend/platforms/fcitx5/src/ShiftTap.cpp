// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/fcitx5/ShiftTap.hpp"

namespace superior::fcitx5 {

ShiftTapDetector::Result ShiftTapDetector::shiftPressed(ShiftKey key, TimePoint at) noexcept {
    bool &slot = key == ShiftKey::Left ? leftDown_ : rightDown_;
    if (slot) {
        // Auto-repeat: keep the original timestamp so holding Shift never
        // looks like a fresh tap.
        return Result::None;
    }
    slot = true;
    if (!timing_) {
        timing_ = true;
        otherKey_ = false;
        pressedAt_ = at;
    }
    return Result::None;
}

ShiftTapDetector::Result ShiftTapDetector::shiftReleased(ShiftKey key, TimePoint at) noexcept {
    bool &slot = key == ShiftKey::Left ? leftDown_ : rightDown_;
    if (!slot) {
        return Result::None;
    }
    slot = false;
    if (leftDown_ || rightDown_) {
        // The other Shift is still held; wait for the gesture to finish.
        return Result::None;
    }
    return finishGesture(at);
}

ShiftTapDetector::Result ShiftTapDetector::finishGesture(TimePoint at) noexcept {
    if (!timing_) {
        // A stray release without a matching press. Do not leave stale state
        // behind, but never toggle from it.
        otherKey_ = false;
        return Result::None;
    }
    timing_ = false;
    const bool quick = at >= pressedAt_ && (at - pressedAt_) < kTapWindow;
    const bool tap = quick && !otherKey_;
    otherKey_ = false;
    return tap ? Result::Toggle : Result::None;
}

void ShiftTapDetector::otherKeyPressed() noexcept {
    if (timing_) {
        otherKey_ = true;
    }
}

void ShiftTapDetector::reset() noexcept {
    leftDown_ = false;
    rightDown_ = false;
    otherKey_ = false;
    timing_ = false;
}

} // namespace superior::fcitx5
