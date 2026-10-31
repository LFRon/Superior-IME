// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace superior::ui {

/// Where the status bar sits inside the screen's available area.
///
/// The strings match `settings.json` (`statusPlacement`), so an unknown value
/// falls back to the default instead of failing to draw.
enum class Placement { BottomRight, BottomLeft, TopRight, TopLeft };

[[nodiscard]] Placement placementFromString(std::string_view value) noexcept;
[[nodiscard]] std::string_view placementName(Placement placement) noexcept;

/// Inset used when an overlay is anchored or clamped to a screen edge.
inline constexpr int kOverlayMargin = 12;

struct Rect {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
};

struct Point {
    int x = 0;
    int y = 0;
};

struct Size {
    int width = 0;
    int height = 0;

    friend constexpr bool operator==(const Size &, const Size &) = default;
};

/// Position of an overlay of `windowSize` anchored inside `available`.
///
/// `extraBottom` lifts the window above another overlay (the candidate panel
/// sits above the status bar). The result is clamped so a window larger than
/// the screen still starts at the margin instead of going off-screen.
[[nodiscard]] Point overlayPosition(Point windowSize, Placement placement, Rect available,
                                    int margin, int extraBottom = 0) noexcept;

/// Keeps `uiScale` inside the range the settings page allows.
[[nodiscard]] double clampScale(double scale) noexcept;

/// One screen as Qt describes it: logical geometry plus its scale factor.
///
/// `logical` is `QScreen::geometry()`; the same screen in device pixels is
/// `logical × dpr`, which is the grid Fcitx5 anchors arrive on.
struct ScreenSpace {
    Rect logical;
    double dpr = 1.0;

    friend constexpr bool operator==(const ScreenSpace &, const ScreenSpace &) = default;
};

/// Converts a caret rectangle from session device pixels to the overlay's own
/// logical pixels, using the screen that actually contains it.
///
/// Qt places windows in logical coordinates and multiplies by
/// `devicePixelRatio` on the way out, while an X server (and XIM, and every
/// Fcitx5 frontend that is not the D-Bus V2 path) speaks device pixels. Feeding
/// the device rect straight to `move()` therefore pushed the candidate window
/// `(dpr - 1) × position` away from the caret — invisible at 100 %, a visible
/// gap at 125 %.
///
/// The division is per screen, not global. Dividing the absolute coordinate by
/// one factor is only right on a single screen: `2000 / 1.5` is not "80 pixels
/// into the second screen", it is 53 logical pixels *past* a 1280-wide first
/// one. Each screen is mapped from its own origin instead. Exactly enough for
/// the case that exists — X11 has a single factor per session — and the closest
/// thing available when factors differ, because Qt exposes each screen's
/// logical geometry but not its origin in the device grid.
///
/// The screen has to be picked from the device point for the same reason: the
/// logical point is the output.
///
/// `screens.front()` is the fallback, so the caller should put the primary
/// screen first. `screenIndex`, when given, receives the screen that was used,
/// so the caller can ask that same screen for its available geometry.
[[nodiscard]] Rect caretToLogical(double left, double top, double width, double height,
                                  std::span<const ScreenSpace> screens,
                                  std::size_t *screenIndex = nullptr) noexcept;

} // namespace superior::ui
