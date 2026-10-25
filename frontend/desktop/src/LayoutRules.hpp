// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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

/// Position of an overlay of `windowSize` anchored inside `available`.
///
/// `extraBottom` lifts the window above another overlay (the candidate panel
/// sits above the status bar). The result is clamped so a window larger than
/// the screen still starts at the margin instead of going off-screen.
[[nodiscard]] Point overlayPosition(Point windowSize, Placement placement, Rect available,
                                    int margin, int extraBottom = 0) noexcept;

/// Keeps `uiScale` inside the range the settings page allows.
[[nodiscard]] double clampScale(double scale) noexcept;

} // namespace superior::ui
