// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "LayoutRules.hpp"
#include "superior/api/Scene.hpp"

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

/// Places `widget` just below the caret in `anchor`, clamped to the screen that
/// contains it. `gap` is the distance between the caret and the widget.
///
/// `Anchor::cursor` is in session device pixels; Qt moves windows in logical
/// pixels, and the mapping is per screen (see `caretToLogical`). When the caret
/// is unknown the widget falls back to the bottom-right corner, which is where a
/// lone overlay is still visible.
void placeAtCaret(QWidget &widget, const api::Anchor &anchor, int gap = 6);

/// Window policy for a panel the **compositor** is expected to place.
///
/// Deliberately *not* override-redirect: a compositor only manages windows it
/// knows about, so an override-redirect window cannot be adopted as an
/// input-method candidate panel (Treeland looks the surface up in its own
/// container and finds nothing). In exchange for giving up override-redirect,
/// the adopting compositor supplies what that flag was buying: no decoration, no
/// keyboard focus, a popup-level stacking, and — the reason this exists at all —
/// a position derived from the application's own cursor rectangle.
void makeCompositorPlacedPanel(QWidget &widget);

/// Anchors `widget` inside the primary screen's available area.
///
/// `extraBottom` lifts the window above another overlay (for example the
/// candidate panel above the status bar).
void placeOverlay(QWidget &widget, Placement placement, int extraBottom = 0);

/// Bottom-right placement with a 12 px margin. Convenience wrapper for panels
/// that have no user-configurable placement.
void placeBottomRight(QWidget &widget, int extraBottom = 0);

} // namespace superior::ui
