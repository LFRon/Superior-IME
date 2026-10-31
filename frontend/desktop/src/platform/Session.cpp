// SPDX-License-Identifier: GPL-3.0-or-later
#include "Session.hpp"

#include <cctype>

namespace superior::ui {

namespace {

bool mentions(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty() || needle.size() > haystack.size()) {
        return false;
    }
    const auto lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        std::size_t j = 0;
        for (; j < needle.size(); ++j) {
            if (lower(haystack[i + j]) != lower(needle[j])) {
                break;
            }
        }
        if (j == needle.size()) {
            return true;
        }
    }
    return false;
}

/// `XDG_SESSION_DESKTOP` and `XDG_CURRENT_DESKTOP` are set by different
/// launchers, disagree in the wild, and are not consistently cased, so every
/// desktop is looked for in both.
bool isDesktop(std::string_view sessionDesktop, std::string_view currentDesktop,
               std::string_view name) noexcept {
    return mentions(sessionDesktop, name) || mentions(currentDesktop, name);
}

} // namespace

Session classifySession(std::string_view qpaPlatform, std::string_view sessionDesktop,
                        std::string_view currentDesktop) noexcept {
    Session session;
    // The plugin decides the window system, never the session type: `xcb` inside
    // a Wayland session means XWayland, and that is X11 for every purpose here.
    session.system = qpaPlatform.starts_with("wayland") ? WindowSystem::Wayland
                                                        : WindowSystem::X11;

    // Treeland first: a Treeland session also advertises DDE, and only one of
    // the two implements the candidate-panel convention.
    if (isDesktop(sessionDesktop, currentDesktop, "treeland")) {
        session.desktop = Desktop::Treeland;
    } else if (isDesktop(sessionDesktop, currentDesktop, "deepin") ||
               mentions(currentDesktop, "DDE")) {
        session.desktop = Desktop::Deepin;
    } else if (isDesktop(sessionDesktop, currentDesktop, "gnome")) {
        session.desktop = Desktop::Gnome;
    } else if (isDesktop(sessionDesktop, currentDesktop, "kde")) {
        session.desktop = Desktop::Kde;
    }
    return session;
}

} // namespace superior::ui
