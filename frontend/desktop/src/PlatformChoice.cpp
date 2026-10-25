// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlatformChoice.hpp"

namespace superior::ui {

namespace {

/// True when a `QT_QPA_PLATFORM` value already selects an X11 plugin
/// (`xcb`, `dxcb;xcb`, …). Those values are left untouched so a distribution's
/// own tuning (Deepin's `dxcb`) keeps working.
bool selectsX11(const std::string &value) {
    return value.find("xcb") != std::string::npos;
}

} // namespace

PlatformDecision chooseQtPlatform(const std::string &override, const std::string &requested,
                                  const std::string &waylandDisplay, const std::string &display,
                                  const std::string &sessionType) {
    // Our own knob always wins: it exists exactly for "I want the native
    // Wayland plugin" or "force offscreen" debugging.
    if (!override.empty()) {
        return {override, "override"};
    }

    const bool waylandSession = !waylandDisplay.empty() || sessionType == "wayland";

    if (waylandSession) {
        if (!display.empty()) {
            // XWayland is available: override-redirect windows keep their own
            // stacking, so the status bar stays on top and never takes focus.
            if (selectsX11(requested)) {
                return {requested, "session-x11-plugin"};
            }
            return {"xcb", "xwayland"};
        }
        // No XWayland at all. Keep whatever the session set; otherwise native
        // Wayland, which is worse than xcb but still draws something.
        if (!requested.empty()) {
            return {requested, "session-default"};
        }
        return {"wayland", "wayland-only"};
    }

    // X11 session: the session default (if any) is already an X11 plugin, so
    // honouring it keeps distribution-specific behaviour.
    if (!requested.empty()) {
        return {requested, "session-default"};
    }
    if (!display.empty()) {
        return {"xcb", "x11"};
    }

    // Headless or unusual session: leave the choice to Qt so the overlay can
    // still run with `-platform offscreen` and friends.
    return {"", "default"};
}

} // namespace superior::ui
