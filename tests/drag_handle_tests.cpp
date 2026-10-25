// SPDX-License-Identifier: GPL-3.0-or-later
#include "DragHandle.hpp"
#include "support/TestSupport.hpp"

namespace {

using superior::ui::clampToScreen;
using superior::ui::isDragHandleHit;
using superior::ui::Point;
using superior::ui::Rect;

constexpr int kWidth = 110;
constexpr int kHeight = 30;

void testDotAreaIsDraggable() {
    // The dot sits at x≈8..18; the whole grip is the leading 22 px.
    for (int x : {0, 1, 8, 12, 18, 21}) {
        CHECK(isDragHandleHit(Point{x, 15}, kWidth, kHeight, 1.0));
    }
}

void testLabelsAreNotDraggable() {
    // Everything from the first label on must stay a click target.
    for (int x : {22, 30, 60, 109}) {
        CHECK(!isDragHandleHit(Point{x, 15}, kWidth, kHeight, 1.0));
    }
}

void testOutsideWindowIsNotADrag() {
    CHECK(!isDragHandleHit(Point{-1, 15}, kWidth, kHeight, 1.0));
    CHECK(!isDragHandleHit(Point{5, -1}, kWidth, kHeight, 1.0));
    CHECK(!isDragHandleHit(Point{5, kHeight}, kWidth, kHeight, 1.0));
    CHECK(!isDragHandleHit(Point{5, 15}, 0, kHeight, 1.0));
    CHECK(!isDragHandleHit(Point{5, 15}, kWidth, 0, 1.0));
}

void testGripScalesWithUiScale() {
    // At 150% the grip covers 33 px, so 30 is still a handle but 40 is not.
    CHECK(isDragHandleHit(Point{30, 20}, 165, 45, 1.5));
    CHECK(!isDragHandleHit(Point{40, 20}, 165, 45, 1.5));
    // At 75% it shrinks to 17 px.
    CHECK(isDragHandleHit(Point{15, 10}, 82, 22, 0.75));
    CHECK(!isDragHandleHit(Point{20, 10}, 82, 22, 0.75));
}

void testNarrowWindowClampsGripToWidth() {
    // A window narrower than the grip must not report hits outside itself.
    CHECK(isDragHandleHit(Point{9, 5}, 10, 20, 1.0));
    CHECK(!isDragHandleHit(Point{12, 5}, 10, 20, 1.0));
}

void testClampKeepsWindowOnScreen() {
    const Rect screen{0, 0, 1920, 1080};
    const Point size{110, 30};
    const auto inside = clampToScreen(Point{500, 400}, size, screen, 12);
    CHECK(inside.x == 500);
    CHECK(inside.y == 400);

    const auto draggedLeft = clampToScreen(Point{-300, -50}, size, screen, 12);
    CHECK(draggedLeft.x == 12);
    CHECK(draggedLeft.y == 12);

    const auto draggedRight = clampToScreen(Point{5000, 5000}, size, screen, 12);
    CHECK(draggedRight.x == 1920 - 110 - 12);
    CHECK(draggedRight.y == 1080 - 30 - 12);
}

void testClampOnSecondaryScreen() {
    const Rect secondary{1920, 0, 1280, 1024};
    // Already inside: untouched, even though the origin is not (0, 0).
    const auto inside = clampToScreen(Point{2000, 900}, Point{110, 30}, secondary, 12);
    CHECK(inside.x == 2000);
    CHECK(inside.y == 900);

    // Past the bottom-right corner of that screen.
    const auto outside = clampToScreen(Point{4000, 2000}, Point{110, 30}, secondary, 12);
    CHECK(outside.x == 1920 + 1280 - 110 - 12);
    CHECK(outside.y == 1024 - 30 - 12);
}

} // namespace

int main() {
    testDotAreaIsDraggable();
    testLabelsAreNotDraggable();
    testOutsideWindowIsNotADrag();
    testGripScalesWithUiScale();
    testNarrowWindowClampsGripToWidth();
    testClampKeepsWindowOnScreen();
    testClampOnSecondaryScreen();
    return superior::test::finish("drag handle tests");
}
