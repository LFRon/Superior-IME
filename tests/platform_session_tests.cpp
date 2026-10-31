// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/Session.hpp"
#include "support/TestSupport.hpp"

namespace {

using superior::ui::classifySession;
using superior::ui::Desktop;
using superior::ui::desktopName;
using superior::ui::WindowSystem;
using superior::ui::windowSystemName;

/// The window system comes from the Qt platform plugin, never from the session
/// type: `xcb` inside a Wayland session means XWayland, and XWayland is X11 for
/// every purpose here (pixels, coordinates, scale source).
void testWindowSystemFollowsThePlugin() {
    CHECK(classifySession("xcb", "Treeland", "DDE").system == WindowSystem::X11);
    CHECK(classifySession("wayland", "Treeland", "DDE").system == WindowSystem::Wayland);
    CHECK(classifySession("wayland-egl", "", "").system == WindowSystem::Wayland);
    // An unknown plugin string is treated as X11, which is what a headless or
    // offscreen run needs (no Wayland-only code path gets taken).
    CHECK(classifySession("", "", "").system == WindowSystem::X11);
    CHECK(classifySession("offscreen", "", "").system == WindowSystem::X11);
}

/// Wayland has no single answer for panel placement, so the desktop has to be
/// identified. `XDG_SESSION_DESKTOP` and `XDG_CURRENT_DESKTOP` are set by
/// different launchers and disagree in the wild, so both are read.
void testDesktopClassification() {
    CHECK(classifySession("xcb", "Treeland", "DDE").desktop == Desktop::Treeland);
    CHECK(classifySession("xcb", "treeland", "").desktop == Desktop::Treeland);
    CHECK(classifySession("xcb", "", "Treeland").desktop == Desktop::Treeland);

    CHECK(classifySession("xcb", "Deepin", "DDE").desktop == Desktop::Deepin);
    CHECK(classifySession("xcb", "", "Deepin").desktop == Desktop::Deepin);
    // DDE on a Wayland session is still DDE: the desktop is what matters for
    // placement, the window system for coordinates.
    CHECK(classifySession("wayland", "DDE", "DDE").desktop == Desktop::Deepin);

    CHECK(classifySession("wayland", "gnome", "GNOME").desktop == Desktop::Gnome);
    CHECK(classifySession("wayland", "KDE", "KDE").desktop == Desktop::Kde);
    CHECK(classifySession("wayland", "sway", "sway").desktop == Desktop::Other);
    CHECK(classifySession("xcb", "", "").desktop == Desktop::Other);
}

/// Treeland must win over DDE: a Treeland session also advertises DDE, and the
/// two have different panel channels.
void testTreelandWinsOverDde() {
    const auto session = classifySession("xcb", "Treeland", "DDE");
    CHECK(session.desktop == Desktop::Treeland);
}

void testNames() {
    CHECK(std::string_view(windowSystemName(WindowSystem::X11)) == "x11");
    CHECK(std::string_view(windowSystemName(WindowSystem::Wayland)) == "wayland");
    CHECK(std::string_view(desktopName(Desktop::Treeland)) == "treeland");
    CHECK(std::string_view(desktopName(Desktop::Other)) == "other");
}

} // namespace

int main() {
    testWindowSystemFollowsThePlugin();
    testDesktopClassification();
    testTreelandWinsOverDde();
    testNames();
    return superior::test::finish("platform session tests");
}
