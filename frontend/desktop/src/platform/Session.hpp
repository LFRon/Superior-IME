// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

namespace superior::ui {

/// Which window system the overlay's own windows live on.
///
/// Not the same question as "which session type is this": an overlay forced onto
/// XWayland has X11 windows, X11 pixels and X11 coordinates even inside a Wayland
/// session. Everything below keys off this, so it is decided once, from the Qt
/// platform plugin actually loaded.
enum class WindowSystem {
    X11,
    Wayland,
};

/// The desktop environment, as far as this project has to care.
///
/// Wayland has no single answer for "place a window next to the caret", so the
/// Wayland branch dispatches on this. X11 does not need it for placement (one
/// mechanism works everywhere) but does use it to know whether a compositor
/// implements the candidate-panel convention over XWayland.
enum class Desktop {
    Treeland,
    Deepin,
    Gnome,
    Kde,
    Other,
};

[[nodiscard]] constexpr const char *windowSystemName(WindowSystem system) noexcept {
    return system == WindowSystem::Wayland ? "wayland" : "x11";
}

[[nodiscard]] constexpr const char *desktopName(Desktop desktop) noexcept {
    switch (desktop) {
    case Desktop::Treeland:
        return "treeland";
    case Desktop::Deepin:
        return "deepin";
    case Desktop::Gnome:
        return "gnome";
    case Desktop::Kde:
        return "kde";
    case Desktop::Other:
        break;
    }
    return "other";
}

/// Everything the platform layer needs to pick an implementation.
struct Session {
    WindowSystem system = WindowSystem::X11;
    Desktop desktop = Desktop::Other;
};

/// Classifies a session from the environment, without touching any API.
///
/// Pure on purpose: the table below is the whole policy, and it is worth a unit
/// test rather than a debugging session. `qpaPlatform` is Qt's platform name
/// (`xcb`, `wayland`, …); the two desktop strings are `XDG_SESSION_DESKTOP` and
/// `XDG_CURRENT_DESKTOP`, which different launchers set differently.
[[nodiscard]] Session classifySession(std::string_view qpaPlatform,
                                      std::string_view sessionDesktop,
                                      std::string_view currentDesktop) noexcept;

} // namespace superior::ui
