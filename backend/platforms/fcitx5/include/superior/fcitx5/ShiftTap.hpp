// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>

namespace superior::fcitx5 {

/// Detects a deliberate Shift tap and ignores Shift held as a modifier.
///
/// This is the whole "轻按 Shift 切换中英文" state machine, kept free of Fcitx5
/// and Qt types so it can be unit tested. The agent only translates key events
/// into calls on this class.
///
/// Rules:
/// - a tap is a press followed by a release within `kTapWindow`;
/// - any other key pressed while Shift is down turns the gesture into a
///   modifier use (capital letter, shortcut) and never toggles;
/// - both physical Shift keys are tracked separately, so Shift_L+Shift_R
///   tapped together still counts as one tap and releasing only one of them
///   does not toggle;
/// - auto-repeat of a held Shift does not restart the tap window.
class ShiftTapDetector {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    enum class ShiftKey : std::uint8_t { Left, Right };
    enum class Result : std::uint8_t { None, Toggle };

    static constexpr std::chrono::milliseconds kTapWindow{400};

    /// Feed a Shift press. Safe to call for auto-repeat: the first press owns
    /// the timestamp.
    Result shiftPressed(ShiftKey key, TimePoint at) noexcept;

    /// Feed a Shift release. Returns `Toggle` when this release completes a
    /// tap that should switch between Chinese and English.
    Result shiftReleased(ShiftKey key, TimePoint at) noexcept;

    /// Feed any non-Shift key press. While Shift is held this cancels the tap.
    void otherKeyPressed() noexcept;

    void reset() noexcept;

    [[nodiscard]] bool shiftDown() const noexcept { return leftDown_ || rightDown_; }

private:
    Result finishGesture(TimePoint at) noexcept;

    bool leftDown_ = false;
    bool rightDown_ = false;
    bool otherKey_ = false;
    bool timing_ = false;
    TimePoint pressedAt_{};
};

} // namespace superior::fcitx5
