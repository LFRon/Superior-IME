// SPDX-License-Identifier: GPL-3.0-or-later
//
// Overlay geometry is the one place where three independent windows could fight
// over stacking. The rule is "they never overlap", and it is arithmetic, so it is
// tested without a display.

#include "support/TestSupport.hpp"
#include "OverlayLayout.hpp"

namespace {

using superior::ui::Rect;
using superior::ui::Size;
using superior::ui::liftAbove;
using superior::ui::placeSpeechPanel;
using superior::ui::speechPanelSize;

bool intersects(const Rect &a, const Rect &b) {
    if (a.width <= 0 || a.height <= 0 || b.width <= 0 || b.height <= 0) {
        return false;
    }
    return a.left < b.left + b.width && b.left < a.left + a.width && a.top < b.top + b.height &&
           b.top < a.top + a.height;
}

void testSizes() {
    const Size ready = speechPanelSize(false, 1.0);
    const Size setup = speechPanelSize(true, 1.0);
    CHECK(ready.width == setup.width);
    CHECK(setup.height > ready.height);

    const Size scaled = speechPanelSize(false, 2.0);
    CHECK(scaled.width == ready.width * 2);
    CHECK(scaled.height == ready.height * 2);
}

/// The default case: a bottom-centre panel next to a bottom-right status bar.
void testDefaultPlacement() {
    const Rect screen{0, 0, 1920, 1040};
    const Rect status{1920 - 110 - 12, 1040 - 30 - 12, 110, 30};
    const Rect panel = placeSpeechPanel(screen, status, speechPanelSize(false, 1.0));

    CHECK(panel.width > 0);
    CHECK(!intersects(panel, status));
    // Centred horizontally, above the bottom edge.
    CHECK(panel.left + panel.width / 2 == screen.width / 2);
    CHECK(panel.top + panel.height <= screen.height);
    CHECK(panel.top > 0);
}

/// A dragged status bar can land exactly where the panel wants to be.
void testAvoidsDraggedStatusBar() {
    const Rect screen{0, 0, 1920, 1040};
    const Rect panelSize = placeSpeechPanel(screen, Rect{}, speechPanelSize(false, 1.0));
    const Rect status{panelSize.left, panelSize.top, panelSize.width, 30};

    const Rect panel = placeSpeechPanel(screen, status, speechPanelSize(false, 1.0));
    CHECK(!intersects(panel, status));
}

/// On a narrow screen the panel must shrink instead of covering the status bar.
void testNarrowScreen() {
    const Rect screen{0, 0, 360, 720};
    const Rect status{360 - 110 - 12, 720 - 30 - 12, 110, 30};
    const Rect panel = placeSpeechPanel(screen, status, speechPanelSize(false, 1.0));
    CHECK(panel.width <= screen.width);
    CHECK(panel.left >= screen.left);
    CHECK(panel.left + panel.width <= screen.left + screen.width);
    CHECK(!intersects(panel, status));
}

/// A candidate panel that would cover the speech panel is lifted above it.
void testCandidateLifted() {
    const Rect screen{0, 0, 1920, 1040};
    const Rect speech{800, 800, 320, 150};
    const Rect candidate{820, 820, 280, 120};
    CHECK(intersects(candidate, speech));

    const Rect lifted = liftAbove(candidate, speech, screen);
    CHECK(!intersects(lifted, speech));
    CHECK(lifted.top >= screen.top);
}

/// No overlap means no change: the common case must not move anything.
void testCandidateUntouched() {
    const Rect screen{0, 0, 1920, 1040};
    const Rect speech{800, 900, 320, 120};
    const Rect candidate{100, 100, 280, 80};
    const Rect result = liftAbove(candidate, speech, screen);
    CHECK(result.left == candidate.left);
    CHECK(result.top == candidate.top);
}

void testEmptyInputs() {
    CHECK(placeSpeechPanel({}, {}, speechPanelSize(false, 1.0)).width == 0);
    CHECK(placeSpeechPanel({0, 0, 100, 100}, {}, Size{}).width == 0);
    const Rect candidate{0, 0, 10, 10};
    CHECK(liftAbove(candidate, {}, {0, 0, 100, 100}).top == 0);
}

} // namespace

int main() {
    testSizes();
    testDefaultPlacement();
    testAvoidsDraggedStatusBar();
    testNarrowScreen();
    testCandidateLifted();
    testCandidateUntouched();
    testEmptyInputs();
    return superior::test::finish("overlay layout tests");
}
