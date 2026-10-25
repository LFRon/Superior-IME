// SPDX-License-Identifier: GPL-3.0-or-later
#include "DragHandle.hpp"

#include <algorithm>
#include <cmath>

namespace superior::ui {

namespace {

int scaled(int value, double scale) {
    return static_cast<int>(std::lround(static_cast<double>(value) * scale));
}

} // namespace

bool isDragHandleHit(Point local, int width, int height, double scale) noexcept {
    if (width <= 0 || height <= 0) {
        return false;
    }
    const int handleWidth = std::min(width, scaled(kDragHandleWidth, scale));
    return local.x >= 0 && local.x < handleWidth && local.y >= 0 && local.y < height;
}

Point clampToScreen(Point position, Point size, Rect available, int margin) noexcept {
    const int maxX = available.left + available.width - size.x - margin;
    const int maxY = available.top + available.height - size.y - margin;
    // A window larger than the screen still has to start at the margin, so the
    // lower bound is applied last.
    const int x = std::max(available.left + margin, std::min(position.x, maxX));
    const int y = std::max(available.top + margin, std::min(position.y, maxY));
    return Point{x, y};
}

} // namespace superior::ui
