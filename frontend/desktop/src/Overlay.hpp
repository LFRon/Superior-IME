// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "LayoutRules.hpp"

class QWidget;

namespace superior::ui {

/// Applies the desktop overlay window policy.
///
/// On X11/XWayland this becomes an override-redirect window: the compositor
/// cannot restack it behind normal windows and clicking it does not take focus
/// away from the text client. On Wayland the same flags degrade to a plain
/// frameless window; callers should prefer an XWayland session for the status
/// bar until the layer-shell backend lands.
void makeOverlay(QWidget &widget);

/// Anchors `widget` inside the primary screen's available area.
///
/// `extraBottom` lifts the window above another overlay (for example the
/// candidate panel above the status bar).
void placeOverlay(QWidget &widget, Placement placement, int extraBottom = 0);

/// Bottom-right placement with a 12 px margin. Convenience wrapper for panels
/// that have no user-configurable placement.
void placeBottomRight(QWidget &widget, int extraBottom = 0);

} // namespace superior::ui
