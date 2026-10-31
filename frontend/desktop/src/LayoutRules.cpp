// SPDX-License-Identifier: GPL-3.0-or-later
#include "LayoutRules.hpp"

#include <algorithm>
#include <cmath>

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

namespace {

/// A scale factor worth dividing by: the guards mirror what `clampScale` refuses
/// to trust, so a broken `devicePixelRatio` maps 1:1 instead of exploding.
double usableDpr(double dpr) noexcept {
    return (dpr > 0.0 && dpr < 100.0) ? dpr : 1.0;
}

} // namespace

Rect caretToLogical(double left, double top, double width, double height,
                    std::span<const ScreenSpace> screens, std::size_t *screenIndex) noexcept {
    if (screens.empty()) {
        const auto round = [](double value) { return static_cast<int>(std::lround(value)); };
        return Rect{round(left), round(top), round(width), round(height)};
    }

    // Pick the screen containing the caret in *device* space, and fall back to
    // the nearest one: a caret can land outside every screen while a screen is
    // being reconfigured, and the nearest screen still beats the wrong ratio.
    std::size_t chosen = 0;
    double best = -1.0;
    for (std::size_t index = 0; index < screens.size(); ++index) {
        const ScreenSpace &screen = screens[index];
        const double factor = usableDpr(screen.dpr);
        const double originX = screen.logical.left * factor;
        const double originY = screen.logical.top * factor;
        const double dx = std::max({originX - left, 0.0, left - (originX + screen.logical.width * factor)});
        const double dy = std::max({originY - top, 0.0, top - (originY + screen.logical.height * factor)});
        const double distance = dx * dx + dy * dy;
        if (best < 0.0 || distance < best) {
            best = distance;
            chosen = index;
        }
        if (distance == 0.0) {
            break;
        }
    }

    const ScreenSpace &screen = screens[chosen];
    const double factor = usableDpr(screen.dpr);
    const double originX = screen.logical.left * factor;
    const double originY = screen.logical.top * factor;
    const auto toLogical = [&](double value, double origin, int logicalOrigin) {
        return logicalOrigin + static_cast<int>(std::lround((value - origin) / factor));
    };
    if (screenIndex != nullptr) {
        *screenIndex = chosen;
    }
    return Rect{toLogical(left, originX, screen.logical.left),
                toLogical(top, originY, screen.logical.top),
                static_cast<int>(std::lround(width / factor)),
                static_cast<int>(std::lround(height / factor))};
}

double clampScale(double scale) noexcept {
    if (!(scale > 0.0)) {
        return 1.0;
    }
    return std::clamp(scale, 0.75, 3.0);
}

} // namespace superior::ui
