// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace superior::ui {

/// Which Qt platform plugin the overlay should use.
///
/// The status bar and the candidate window are override-redirect X11 windows.
/// That is the only way to get "always on top, never takes focus" without
/// compositor cooperation, so a Wayland session must run the overlay through
/// XWayland instead of Qt's native `wayland` plugin — the native one gives a
/// plain toplevel that the compositor may stack anywhere.
///
/// The agent starts the overlay without going through any wrapper script, so
/// the decision has to live in the process. Kept free of Qt so it is unit
/// tested.
struct PlatformDecision {
    /// Value for `QT_QPA_PLATFORM`; empty means "let Qt decide".
    std::string value;
    /// Short machine-readable reason, used for logging and docs.
    std::string reason;
};

/// @param override       `SUPERIOR_UI_PLATFORM`, our own highest-precedence knob
/// @param requested      existing `QT_QPA_PLATFORM` (empty when unset)
/// @param waylandDisplay `WAYLAND_DISPLAY` (empty when not a Wayland session)
/// @param display        `DISPLAY` (empty when XWayland is unavailable)
/// @param sessionType    `XDG_SESSION_TYPE` (`wayland`, `x11`, …)
[[nodiscard]] PlatformDecision chooseQtPlatform(const std::string &override,
                                               const std::string &requested,
                                               const std::string &waylandDisplay,
                                               const std::string &display,
                                               const std::string &sessionType);

} // namespace superior::ui
