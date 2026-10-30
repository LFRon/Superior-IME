// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string_view>

namespace superior::ui {

/// Where a scale factor came from.
///
/// Logged with the resolved ratio: "the toolbar is the wrong size" then has an
/// answer in the log instead of needing a debugger, and a port that adds a
/// fourth source cannot do it silently.
enum class ScaleSource {
    /// Nothing platform-specific answered; the desktop is treated as 1:1.
    Fallback,
    /// X11: the `Xft.dpi` X resource, which is what every Xft toolkit scales by.
    X11XftDpi,
    /// Wayland: `wl_output.scale` from the compositor.
    WaylandOutput,
};

[[nodiscard]] constexpr const char *scaleSourceName(ScaleSource source) noexcept {
    switch (source) {
    case ScaleSource::X11XftDpi:
        return "x11-xft-dpi";
    case ScaleSource::WaylandOutput:
        return "wayland-output";
    case ScaleSource::Fallback:
        break;
    }
    return "fallback";
}

/// The desktop's own scale factor, as the platform reported it.
struct SystemScale {
    /// 1.0 = 96 dpi / output scale 1. Never zero or negative.
    double factor = 1.0;
    ScaleSource source = ScaleSource::Fallback;
    /// The number the platform reported: the `Xft.dpi` value, the
    /// `wl_output.scale`, or 0 when nothing answered.
    int raw = 0;
};

/// Extracts `Xft.dpi` from an X resource database string.
///
/// Returns 0 when the resource is absent or outside a plausible range: a value
/// like `Xft.dpi: 0` or a runaway line is a typo, not a scaling request, and
/// treating it as one would make the overlay unusable.
[[nodiscard]] inline int parseXftDpi(std::string_view resources) noexcept {
    constexpr std::string_view key = "Xft.dpi:";
    const std::size_t at = resources.find(key);
    if (at == std::string_view::npos) {
        return 0;
    }
    std::size_t index = at + key.size();
    while (index < resources.size() &&
           (resources[index] == ' ' || resources[index] == '\t')) {
        ++index;
    }
    int value = 0;
    bool digits = false;
    while (index < resources.size() && resources[index] >= '0' && resources[index] <= '9') {
        digits = true;
        value = value * 10 + (resources[index] - '0');
        if (value > 10000) {
            return 0;
        }
        ++index;
    }
    // 48 dpi (0.5x) … 960 dpi (10x). Outside that it is not a display setting.
    if (!digits || value < 48 || value > 960) {
        return 0;
    }
    return value;
}

/// The ratio the strategy layer should apply.
///
/// The toolkit has already multiplied every logical pixel by `toolkitDpr` (Qt
/// derives it from the same platform value, but only at start-up, so an overlay
/// that runs for days keeps the ratio it was born with). Dividing by it means
/// the final on-screen size is `system × user` exactly once, whatever the
/// toolkit cached, and it stays correct if the toolkit ever catches up.
[[nodiscard]] inline double scaleForWindows(const SystemScale &system, double toolkitDpr,
                                            double userScale) noexcept {
    const double os = system.factor > 0.0 ? system.factor : 1.0;
    const double dpr = toolkitDpr > 0.0 ? toolkitDpr : 1.0;
    const double user = userScale > 0.0 ? userScale : 1.0;
    return user * os / dpr;
}

/// Asks the running desktop for its scale factor.
///
/// This is the only place that names a platform API for scaling; the windows
/// never see anything but the resulting ratio (`setScale`). The platform
/// modules behind it are compiled in when their library is available:
/// `ScreenScaleX11.cpp` reads the X resource database over XCB, and
/// `ScreenScaleWayland.cpp` reads `wl_output.scale` from the compositor.
[[nodiscard]] SystemScale readSystemScale();

} // namespace superior::ui
