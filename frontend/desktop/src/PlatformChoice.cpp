// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlatformChoice.hpp"

namespace superior::ui {

namespace {

/// True when a `QT_QPA_PLATFORM` value already selects an X11 plugin
/// (`xcb`, `dxcb;xcb`, …). Those values are left untouched so a distribution's
/// own tuning (Deepin's `dxcb`) keeps working.
///
/// Only the **first** entry counts. Qt reads the value as a preference list and
/// uses the first plugin that loads, so `wayland;xcb` runs Wayland. Testing the
/// whole string for "xcb" accepted exactly that and quietly produced a native
/// Wayland window: no override-redirect, so the toolbar could not stay on top,
/// could not be moved (there is no compositor move for it), and the dock listed
/// it as another application window. Treeland exports precisely
/// `QT_QPA_PLATFORM=wayland;xcb`.
bool selectsX11(const std::string &value) {
    const std::string first = value.substr(0, value.find(';'));
    return first.find("xcb") != std::string::npos;
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
