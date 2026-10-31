// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cstddef>
#include <span>
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
void testCaretToLogical() {
    using superior::ui::caretToLogical;
    using superior::ui::ScreenSpace;

    const ScreenSpace oneHundred{Rect{0, 0, 1920, 1080}, 1.0};
    const ScreenSpace oneTwentyFive{Rect{0, 0, 1536, 864}, 1.25};

    // 100 %: the identity.
    const auto one = caretToLogical(1000, 500, 2, 20, std::span{&oneHundred, 1});
    CHECK(one.left == 1000);
    CHECK(one.top == 500);
    CHECK(one.height == 20);

    // 125 %: XWayland device pixels are 1.25x the logical grid Qt uses.
    const auto x125 = caretToLogical(2000, 1000, 125, 25, std::span{&oneTwentyFive, 1});
    CHECK(x125.left == 1600);
    CHECK(x125.top == 800);
    CHECK(x125.width == 100);
    CHECK(x125.height == 20);

    // A nonsense ratio must not divide by zero or by 0.01.
    const ScreenSpace brokenDpr{Rect{0, 0, 1920, 1080}, 0.0};
    CHECK(caretToLogical(800, 400, 10, 10, std::span{&brokenDpr, 1}).left == 800);
    const ScreenSpace absurdDpr{Rect{0, 0, 1920, 1080}, 1000.0};
    CHECK(caretToLogical(800, 400, 10, 10, std::span{&absurdDpr, 1}).left == 800);
}

/// Two 1.5x screens side by side: the ratio is the same everywhere, but the
/// origin is not, and a caret on the second screen must not be divided against
/// the desktop origin.
void testCaretToLogicalSecondScreen() {
    using superior::ui::caretToLogical;
    using superior::ui::ScreenSpace;

    const ScreenSpace left{Rect{0, 0, 1280, 720}, 1.5};
    const ScreenSpace right{Rect{1280, 0, 1280, 720}, 1.5};
    const std::array screens{left, right};

    std::size_t index = 0;
    const auto onLeft = caretToLogical(600, 300, 0, 36, screens, &index);
    CHECK(index == 0);
    CHECK(onLeft.left == 400);
    CHECK(onLeft.top == 200);
    CHECK(onLeft.height == 24);

    // Device (2000, 300) is 80 device pixels into the right screen, which is 53
    // logical pixels from its origin: 1280 + 53. Dividing the absolute 2000 by
    // 1.5 would have answered 1333, i.e. one device pixel off here and much more
    // further along.
    const auto onRight = caretToLogical(2000, 300, 0, 36, screens, &index);
    CHECK(index == 1);
    CHECK(onRight.left == 1333);
    CHECK(onRight.top == 200);
}

/// A second screen with a different factor: the ratio belongs to the screen, not
/// to the desktop, so one global division cannot answer for both. (X11 itself
/// only has one factor per session; this models the mapping, which is what the
/// app has to get right if that ever changes.)
void testCaretToLogicalMixedScaling() {
    using superior::ui::caretToLogical;
    using superior::ui::ScreenSpace;

    const ScreenSpace left{Rect{0, 0, 1920, 1080}, 1.0};
    const ScreenSpace right{Rect{1920, 0, 1280, 720}, 2.0};
    const std::array screens{left, right};

    std::size_t index = 0;
    const auto onRight = caretToLogical(4000, 400, 0, 48, screens, &index);
    CHECK(index == 1);
    CHECK(onRight.left == 2000);
    CHECK(onRight.top == 200);
    CHECK(onRight.height == 24);

    // A caret outside every screen still has to answer with a screen: the
    // nearest one beats an arbitrary ratio.
    const auto outside = caretToLogical(6000, -200, 0, 48, screens, &index);
    CHECK(index == 1);
    CHECK(outside.left > 1920);
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
    testCaretToLogical();
    testCaretToLogicalSecondScreen();
    testCaretToLogicalMixedScaling();
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
