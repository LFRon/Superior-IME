// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "LayoutRules.hpp"

namespace superior::ui {

/// Geometry policy for the three overlay windows: candidate panel, status bar and
/// speech panel.
///
/// All three are override-redirect windows in one process, so X11 stacking is
/// whatever we ask for. Rather than raising windows against each other, the rule
/// here is **they never overlap**. That makes the z-order question moot, and it
/// is testable without a display.
struct OverlayGeometry {
    Rect screen;    // available area (minus panels/taskbars)
    Rect statusBar; // current status bar rect; may be empty when hidden
    Rect voicePanel; // current speech panel rect; may be empty when hidden
};

/// Width of the speech panel. The height is content-driven: it comes from the
/// Qt layout, because a translated label wraps to a different number of lines.
[[nodiscard]] int speechPanelWidth(double scale) noexcept;

/// Places the speech panel above the status bar, centred on it.
///
/// The panel belongs to the toolbar: it is opened from a button there, so it must
/// appear next to it rather than in the middle of the screen. With no status bar
/// visible it falls back to bottom-centre, and a toolbar at the very top of the
/// screen pushes the panel below itself instead of off-screen.
[[nodiscard]] Rect placeSpeechPanel(Rect screen, Rect statusBar, Size desired,
                                    int margin = kOverlayMargin) noexcept;

/// Keeps a dragged speech panel inside the screen.
///
/// The panel starts anchored above the toolbar, but it can be dragged anywhere.
/// A user position wins over the anchor; it is only nudged back when the screen
/// shrank or the panel grew, so dragging can never lose the window off-screen.
[[nodiscard]] Rect clampToScreen(Rect screen, Size desired, Point position,
                                 int margin = kOverlayMargin) noexcept;

/// Where the candidate panel should go so it does not cover the speech panel.
///
/// `candidate` is the position computed from the caret; when it intersects the
/// speech panel it is lifted above it (never pushed off-screen).
[[nodiscard]] Rect liftAbove(Rect candidate, Rect speechPanel, Rect screen,
                             int margin = kOverlayMargin) noexcept;

} // namespace superior::ui
