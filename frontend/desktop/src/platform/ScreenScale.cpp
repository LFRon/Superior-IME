// SPDX-License-Identifier: GPL-3.0-or-later
#include "ScreenScale.hpp"

#include <cstdlib>

#include "ScalePlatform.hpp"

namespace superior::ui {

namespace {

bool waylandSession() {
    const char *display = std::getenv("WAYLAND_DISPLAY");
    return display != nullptr && display[0] != '\0';
}

} // namespace

SystemScale readSystemScale() {
    // A Wayland session reports its scale as `wl_output.scale`; the overlay runs
    // on XWayland there, so the compositor is the only source that knows about
    // a scale changed after the X server started.
    if (waylandSession()) {
        if (const int scale = detail::waylandOutputScale(); scale > 0) {
            return SystemScale{static_cast<double>(scale), ScaleSource::WaylandOutput, scale};
        }
    }
    // X11 (and XWayland, whose compositor writes `Xft.dpi`): the same resource
    // every Xft toolkit reads, so the toolbar ends up the same size as theirs.
    if (const int dpi = detail::x11XftDpi(); dpi > 0) {
        return SystemScale{static_cast<double>(dpi) / 96.0, ScaleSource::X11XftDpi, dpi};
    }
    return SystemScale{};
}

} // namespace superior::ui
