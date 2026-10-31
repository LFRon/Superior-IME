// SPDX-License-Identifier: GPL-3.0-or-later
#include "ScreenScale.hpp"

#include "ScalePlatform.hpp"

namespace superior::ui {

SystemScale readSystemScale(WindowSystem system) {
    if (system == WindowSystem::Wayland) {
        if (const int scale = detail::waylandOutputScale(); scale > 0) {
            return SystemScale{static_cast<double>(scale), ScaleSource::WaylandOutput, scale};
        }
        return SystemScale{};
    }
    // X11, including XWayland: the same `Xft.dpi` resource every Xft toolkit
    // reads, so the toolbar ends up the same size as theirs.
    if (const int dpi = detail::x11XftDpi(); dpi > 0) {
        return SystemScale{static_cast<double>(dpi) / 96.0, ScaleSource::X11XftDpi, dpi};
    }
    return SystemScale{};
}

} // namespace superior::ui
