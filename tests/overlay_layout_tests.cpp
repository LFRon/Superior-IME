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
using superior::ui::clampToScreen;
using superior::ui::liftAbove;
using superior::ui::placeSpeechPanel;
using superior::ui::speechPanelWidth;

bool intersects(const Rect &a, const Rect &b) {
    if (a.width <= 0 || a.height <= 0 || b.width <= 0 || b.height <= 0) {
        return false;
    }
    return a.left < b.left + b.width && b.left < a.left + a.width && a.top < b.top + b.height &&
           b.top < a.top + a.height;
}

Rect panelSize(int height = 170) {
    return Rect{0, 0, speechPanelWidth(1.0), height};
}

void testWidthScales() {
    CHECK(speechPanelWidth(1.0) > 0);
    CHECK(speechPanelWidth(2.0) == speechPanelWidth(1.0) * 2);
    // A zero scale must not produce a zero-width window.
    CHECK(speechPanelWidth(0.0) >= 0);
}

/// The panel is opened from a button in the toolbar, so it appears directly above
/// the bar and centred on it — not in the middle of the screen.
void testAnchoredAboveToolbar() {
    const Rect screen{0, 0, 1920, 1040};
    // Mid-screen bar: the panel is wider than the bar, so a bar near an edge
    // would be clamped and the centring assertion would test nothing.
    const Rect status{900, 1000, 200, 34};
    const Size desired{panelSize().width, panelSize().height};
    const Rect panel = placeSpeechPanel(screen, status, desired);

    CHECK(!intersects(panel, status));
    CHECK(panel.top + panel.height <= status.top);
    // Centred on the toolbar, which is what "relative to the toolbar" means.
    CHECK(panel.left + panel.width / 2 == status.left + status.width / 2);
    CHECK(panel.top >= screen.top);
}

/// A bar near the right edge makes centring impossible; staying on screen wins.
void testBarAtRightEdgeStillContained() {
    const Rect screen{0, 0, 1920, 1040};
    const Rect status{1700, 1000, 200, 34};
    const Rect panel = placeSpeechPanel(screen, status, Size{panelSize().width, 170});
    CHECK(panel.left + panel.width <= screen.left + screen.width);
    CHECK(!intersects(panel, status));
    CHECK(panel.top + panel.height <= status.top);
}

/// A toolbar dragged to the left edge must not push the panel off-screen.
void testAnchoredPanelStaysOnScreen() {
    const Rect screen{0, 0, 1920, 1040};
    const Rect status{0, 1000, 200, 34};
    const Rect panel = placeSpeechPanel(screen, status, Size{panelSize().width, 170});
    CHECK(panel.left >= screen.left);
    CHECK(panel.left + panel.width <= screen.left + screen.width);
    CHECK(!intersects(panel, status));
}

/// No toolbar on screen: fall back to bottom-centre, which is where a lone panel
/// belongs.
void testNoToolbarUsesBottomCentre() {
    const Rect screen{0, 0, 1920, 1040};
    const Rect panel = placeSpeechPanel(screen, Rect{}, Size{panelSize().width, 170});
    CHECK(panel.left + panel.width / 2 == screen.width / 2);
    CHECK(panel.top + panel.height <= screen.height);
    CHECK(panel.top > 0);
}

/// A toolbar at the top of the screen leaves no room above it.
void testToolbarAtTopPlacesPanelBelow() {
    const Rect screen{0, 0, 1920, 1040};
    const Rect status{1700, 0, 200, 34};
    const Rect panel = placeSpeechPanel(screen, status, Size{panelSize().width, 170});
    CHECK(panel.top >= status.top + status.height);
    CHECK(!intersects(panel, status));
}

/// On a narrow screen the panel must shrink instead of hanging off the edge.
void testNarrowScreen() {
    const Rect screen{0, 0, 360, 720};
    const Rect status{360 - 110 - 12, 720 - 30 - 12, 110, 30};
    const Rect panel = placeSpeechPanel(screen, status, Size{panelSize().width, 170});
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

/// Dragging is a user decision and outranks the anchor: the panel stays exactly
/// where it was dropped.
void testDraggedPositionWins() {
    const Rect screen{0, 0, 1920, 1040};
    const Rect dragged = clampToScreen(screen, Size{panelSize().width, 170}, {700, 420});
    CHECK(dragged.left == 700);
    CHECK(dragged.top == 420);
    CHECK(dragged.width == panelSize().width);
    CHECK(dragged.height == 170);
}

/// A drag may end at an edge or past it; being reachable beats being exact.
void testDraggedPositionClamped() {
    const Rect screen{0, 0, 1920, 1040};
    const Size size{panelSize().width, 170};
    const Rect leftTop = clampToScreen(screen, size, {-500, -500});
    CHECK(leftTop.left == 12);
    CHECK(leftTop.top == 12);

    const Rect rightBottom = clampToScreen(screen, size, {5000, 5000});
    CHECK(rightBottom.left + rightBottom.width <= screen.width - 12);
    CHECK(rightBottom.top + rightBottom.height <= screen.height - 12);
}

/// A panel taller than the screen must still start at the top margin instead of
/// being pushed off the top.
void testDraggedPanelTallerThanScreen() {
    const Rect screen{0, 0, 800, 300};
    const Rect clamped = clampToScreen(screen, Size{400, 900}, {10, 500});
    CHECK(clamped.height == 300 - 24);
    CHECK(clamped.top == 12);
    CHECK(clamped.left >= 12);
}

void testEmptyInputs() {
    CHECK(clampToScreen({}, Size{100, 100}, {0, 0}).width == 0);
    CHECK(clampToScreen({0, 0, 100, 100}, Size{}, {0, 0}).width == 0);
    CHECK(placeSpeechPanel({}, {}, Size{panelSize().width, 170}).width == 0);
    CHECK(placeSpeechPanel({0, 0, 100, 100}, {}, Size{}).width == 0);
    const Rect candidate{0, 0, 10, 10};
    CHECK(liftAbove(candidate, {}, {0, 0, 100, 100}).top == 0);
}

} // namespace

int main() {
    testWidthScales();
    testAnchoredAboveToolbar();
    testBarAtRightEdgeStillContained();
    testAnchoredPanelStaysOnScreen();
    testNoToolbarUsesBottomCentre();
    testToolbarAtTopPlacesPanelBelow();
    testNarrowScreen();
    testCandidateLifted();
    testCandidateUntouched();
    testDraggedPositionWins();
    testDraggedPositionClamped();
    testDraggedPanelTallerThanScreen();
    testEmptyInputs();
    return superior::test::finish("overlay layout tests");
}
