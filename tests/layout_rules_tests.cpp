// SPDX-License-Identifier: GPL-3.0-or-later
#include <string_view>

#include "LayoutRules.hpp"
#include "support/TestSupport.hpp"

namespace {

using superior::ui::clampScale;
using superior::ui::overlayPosition;
using superior::ui::Placement;
using superior::ui::Point;
using superior::ui::Rect;

constexpr Rect kScreen{0, 0, 1920, 1080};
constexpr int kMargin = 12;

void testPlacementParsing() {
    CHECK(superior::ui::placementFromString("bottomRight") == Placement::BottomRight);
    CHECK(superior::ui::placementFromString("bottomLeft") == Placement::BottomLeft);
    CHECK(superior::ui::placementFromString("topRight") == Placement::TopRight);
    CHECK(superior::ui::placementFromString("topLeft") == Placement::TopLeft);
}

void testUnknownPlacementFallsBack() {
    CHECK(superior::ui::placementFromString("") == Placement::BottomRight);
    CHECK(superior::ui::placementFromString("bottom-center") == Placement::BottomRight);
    CHECK(superior::ui::placementFromString("BOTTOMLEFT") == Placement::BottomRight);
}

void testPlacementRoundTrip() {
    for (auto placement : {Placement::BottomRight, Placement::BottomLeft, Placement::TopRight,
                           Placement::TopLeft}) {
        CHECK(superior::ui::placementFromString(superior::ui::placementName(placement)) ==
              placement);
    }
}

void testBottomRight() {
    const Point position =
        overlayPosition(Point{200, 34}, Placement::BottomRight, kScreen, kMargin);
    CHECK(position.x == 1920 - 200 - 12);
    CHECK(position.y == 1080 - 34 - 12);
}

void testBottomLeft() {
    const Point position =
        overlayPosition(Point{200, 34}, Placement::BottomLeft, kScreen, kMargin);
    CHECK(position.x == 12);
    CHECK(position.y == 1080 - 34 - 12);
}

void testTopRight() {
    const Point position = overlayPosition(Point{200, 34}, Placement::TopRight, kScreen, kMargin);
    CHECK(position.x == 1920 - 200 - 12);
    CHECK(position.y == 12);
}

void testTopLeft() {
    const Point position = overlayPosition(Point{200, 34}, Placement::TopLeft, kScreen, kMargin);
    CHECK(position.x == 12);
    CHECK(position.y == 12);
}

void testExtraBottomLiftsWindow() {
    const Point plain =
        overlayPosition(Point{200, 34}, Placement::BottomRight, kScreen, kMargin);
    const Point lifted =
        overlayPosition(Point{200, 34}, Placement::BottomRight, kScreen, kMargin, 120);
    CHECK(lifted.y == plain.y - 120);
}

void testNonZeroOriginScreen() {
    // A secondary monitor to the right of the primary one.
    const Rect secondary{1920, 0, 1280, 1024};
    const Point position =
        overlayPosition(Point{200, 34}, Placement::BottomRight, secondary, kMargin);
    CHECK(position.x == 1920 + 1280 - 200 - 12);
    CHECK(position.y == 1024 - 34 - 12);
}

void testOversizedWindowStaysOnScreen() {
    const Point position = overlayPosition(Point{4000, 4000}, Placement::BottomRight, kScreen,
                                           kMargin);
    CHECK(position.x == 12);
    CHECK(position.y == 12);
}

void testScaleClamp() {
    CHECK(clampScale(1.0) == 1.0);
    CHECK(clampScale(0.5) == 0.75);
    CHECK(clampScale(0.75) == 0.75);
    CHECK(clampScale(3.0) == 3.0);
    CHECK(clampScale(9.0) == 3.0);
    CHECK(clampScale(0.0) == 1.0);
    CHECK(clampScale(-2.0) == 1.0);
}

} // namespace

int main() {
    testPlacementParsing();
    testUnknownPlacementFallsBack();
    testPlacementRoundTrip();
    testBottomRight();
    testBottomLeft();
    testTopRight();
    testTopLeft();
    testExtraBottomLiftsWindow();
    testNonZeroOriginScreen();
    testOversizedWindowStaysOnScreen();
    testScaleClamp();
    return superior::test::finish("layout rules tests");
}
