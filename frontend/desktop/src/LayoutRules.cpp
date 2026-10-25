// SPDX-License-Identifier: GPL-3.0-or-later
#include "LayoutRules.hpp"

#include <algorithm>

namespace superior::ui {

Placement placementFromString(std::string_view value) noexcept {
    if (value == "bottomLeft") {
        return Placement::BottomLeft;
    }
    if (value == "topRight") {
        return Placement::TopRight;
    }
    if (value == "topLeft") {
        return Placement::TopLeft;
    }
    return Placement::BottomRight;
}

std::string_view placementName(Placement placement) noexcept {
    switch (placement) {
    case Placement::BottomLeft: return "bottomLeft";
    case Placement::TopRight: return "topRight";
    case Placement::TopLeft: return "topLeft";
    case Placement::BottomRight: break;
    }
    return "bottomRight";
}

Point overlayPosition(Point windowSize, Placement placement, Rect available, int margin,
                      int extraBottom) noexcept {
    const bool right = placement == Placement::BottomRight || placement == Placement::TopRight;
    const bool bottom = placement == Placement::BottomRight || placement == Placement::BottomLeft;

    const int x = right ? available.left + available.width - windowSize.x - margin
                        : available.left + margin;
    const int y = bottom ? available.top + available.height - windowSize.y - margin - extraBottom
                         : available.top + margin + extraBottom;

    // Never place the window outside the usable area, even if it is larger than
    // the screen: prefer the top-left edge over an unreachable position.
    return Point{std::max(available.left + margin, x),
                 std::max(available.top + margin, y)};
}

double clampScale(double scale) noexcept {
    if (!(scale > 0.0)) {
        return 1.0;
    }
    return std::clamp(scale, 0.75, 3.0);
}

} // namespace superior::ui
