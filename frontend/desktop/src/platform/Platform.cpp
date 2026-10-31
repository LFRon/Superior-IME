// SPDX-License-Identifier: GPL-3.0-or-later
#include "Platform.hpp"

#include <QGuiApplication>

#include <cstdlib>
#include <string>

#include "wayland/WaylandPlatform.hpp"
#include "x11/X11Platform.hpp"

namespace superior::ui {

namespace {

std::string env(const char *name) {
    const char *value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string{};
}

} // namespace

Session detectSession() noexcept {
    const std::string platform = QGuiApplication::platformName().toStdString();
    return classifySession(platform, env("XDG_SESSION_DESKTOP"), env("XDG_CURRENT_DESKTOP"));
}

SystemScale readSystemScale(WindowSystem system) noexcept {
    if (system == WindowSystem::Wayland) {
        if (const int scale = wayland::outputScale(); scale > 0) {
            return SystemScale{static_cast<double>(scale), ScaleSource::WaylandOutput, scale};
        }
        return SystemScale{};
    }
    if (const int dpi = x11::xftDpi(); dpi > 0) {
        return SystemScale{static_cast<double>(dpi) / 96.0, ScaleSource::X11XftDpi, dpi};
    }
    return SystemScale{};
}

bool compositorPlacesCandidatePanel(const Session &session) noexcept {
    switch (session.system) {
    case WindowSystem::X11:
        // Over XWayland, Treeland adopts a window that carries its atom.
        return session.desktop == Desktop::Treeland;
    case WindowSystem::Wayland:
        // Native Wayland would need the desktop's own toplevel-tag convention,
        // which Qt does not expose (see `WaylandPlatform.cpp`).
        return false;
    }
    return false;
}

PanelChannel adoptAsCandidatePanel(const Session &session, std::uintptr_t window) noexcept {
    switch (session.system) {
    case WindowSystem::X11:
        if (session.desktop != Desktop::Treeland) {
            return PanelChannel::Unsupported;
        }
        return x11::adoptAsCandidatePanel(window) ? PanelChannel::Applied
                                                 : PanelChannel::Unsupported;
    case WindowSystem::Wayland:
        return session.desktop == Desktop::Treeland ? PanelChannel::Unreachable
                                                    : PanelChannel::Unsupported;
    }
    return PanelChannel::Unsupported;
}

std::string_view panelChannelReason(const Session &session) noexcept {
    switch (session.system) {
    case WindowSystem::X11:
        return session.desktop == Desktop::Treeland
                   ? "xwayland candidate-panel atom"
                   : "the compositor has no candidate-panel convention";
    case WindowSystem::Wayland:
        return session.desktop == Desktop::Treeland
                   ? "native Wayland cannot tag Qt's toplevel; use the XWayland overlay"
                   : "native Wayland has no candidate-panel convention";
    }
    return "unknown";
}

} // namespace superior::ui
