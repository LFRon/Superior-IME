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

/// The anchor arrives in device pixels; Qt moves windows in logical pixels.
void testDeviceToLogical() {
    // 100 %: the identity.
    const auto one = superior::ui::deviceToLogical(1000, 500, 2, 20, 1.0);
    CHECK(one.left == 1000);
    CHECK(one.top == 500);
    CHECK(one.height == 20);

    // 125 %: XWayland device pixels are 1.25x the logical grid Qt uses.
    const auto x125 = superior::ui::deviceToLogical(2000, 1000, 125, 25, 1.25);
    CHECK(x125.left == 1600);
    CHECK(x125.top == 800);
    CHECK(x125.width == 100);
    CHECK(x125.height == 20);

    // A nonsense ratio must not divide by zero or by 0.01.
    const auto broken = superior::ui::deviceToLogical(800, 400, 10, 10, 0.0);
    CHECK(broken.left == 800);
    const auto absurd = superior::ui::deviceToLogical(800, 400, 10, 10, 1000.0);
    CHECK(absurd.left == 800);
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
    testDeviceToLogical();
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
