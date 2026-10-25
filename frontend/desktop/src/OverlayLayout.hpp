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

/// Desired size for the speech panel in the given phase; the panel itself has
/// two layouts (setup is taller because it holds the download row).
[[nodiscard]] Size speechPanelSize(bool setup, double scale) noexcept;

/// Bottom-centre placement for the speech panel, narrowed and lifted so it never
/// intersects the status bar.
[[nodiscard]] Rect placeSpeechPanel(Rect screen, Rect statusBar, Size desired,
                                    int margin = kOverlayMargin) noexcept;

/// Where the candidate panel should go so it does not cover the speech panel.
///
/// `candidate` is the position computed from the caret; when it intersects the
/// speech panel it is lifted above it (never pushed off-screen).
[[nodiscard]] Rect liftAbove(Rect candidate, Rect speechPanel, Rect screen,
                             int margin = kOverlayMargin) noexcept;

} // namespace superior::ui
