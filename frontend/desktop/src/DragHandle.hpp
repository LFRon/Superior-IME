// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "LayoutRules.hpp"

namespace superior::ui {

/// Width of the draggable grip at the left edge of the status bar, in unscaled
/// units. It covers the coloured dot plus a little slack — the same idea as
/// Sogou's logo grip: the bar is movable only from that small area, so a click
/// on "中" or "全角标点" can never start a drag by accident.
constexpr int kDragHandleWidth = 22;

/// True when `local` (window coordinates) is inside the drag grip.
[[nodiscard]] bool isDragHandleHit(Point local, int width, int height, double scale) noexcept;

/// Keeps `position` inside `available`, allowing the window to be dragged to any
/// screen edge but not off it. `margin` is the same inset the automatic
/// placement uses.
[[nodiscard]] Point clampToScreen(Point position, Point size, Rect available,
                                  int margin) noexcept;

} // namespace superior::ui
