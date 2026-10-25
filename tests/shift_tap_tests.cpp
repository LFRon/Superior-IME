// SPDX-License-Identifier: GPL-3.0-or-later
#include "support/TestSupport.hpp"
#include "superior/fcitx5/ShiftTap.hpp"

#include <chrono>

namespace {

using superior::fcitx5::ShiftTapDetector;
using Key = ShiftTapDetector::ShiftKey;
using Result = ShiftTapDetector::Result;
using Clock = ShiftTapDetector::TimePoint;
using std::chrono::milliseconds;

Clock at(int ms) {
    return Clock{} + milliseconds(ms);
}

void testQuickTapToggles() {
    ShiftTapDetector detector;
    CHECK(detector.shiftPressed(Key::Left, at(0)) == Result::None);
    CHECK(detector.shiftDown());
    CHECK(detector.shiftReleased(Key::Left, at(80)) == Result::Toggle);
    CHECK(!detector.shiftDown());
}

void testTapWindowBoundary() {
    ShiftTapDetector atLimit;
    atLimit.shiftPressed(Key::Left, at(0));
    CHECK(atLimit.shiftReleased(Key::Left, at(400)) == Result::None);

    ShiftTapDetector justInside;
    justInside.shiftPressed(Key::Left, at(0));
    CHECK(justInside.shiftReleased(Key::Left, at(399)) == Result::Toggle);
}

void testLongHoldIsNotATap() {
    ShiftTapDetector detector;
    detector.shiftPressed(Key::Left, at(0));
    CHECK(detector.shiftReleased(Key::Left, at(1200)) == Result::None);
}

void testTypingWhileShiftHeldCancels() {
    ShiftTapDetector detector;
    detector.shiftPressed(Key::Left, at(0));
    detector.otherKeyPressed(); // the 'A' of a capital letter
    CHECK(detector.shiftReleased(Key::Left, at(90)) == Result::None);
}

void testKeyReleasedBeforeShiftStillCancels() {
    ShiftTapDetector detector;
    detector.shiftPressed(Key::Left, at(0));
    detector.otherKeyPressed();
    CHECK(detector.shiftReleased(Key::Left, at(120)) == Result::None);
}

void testOtherKeyWithoutShiftDoesNotLeak() {
    ShiftTapDetector detector;
    detector.otherKeyPressed(); // typing in Chinese mode
    detector.shiftPressed(Key::Left, at(0));
    CHECK(detector.shiftReleased(Key::Left, at(50)) == Result::Toggle);
}

void testBothShiftKeysCountAsOneTap() {
    ShiftTapDetector first;
    first.shiftPressed(Key::Left, at(0));
    first.shiftPressed(Key::Right, at(30));
    CHECK(first.shiftReleased(Key::Left, at(60)) == Result::None);
    CHECK(first.shiftDown());
    CHECK(first.shiftReleased(Key::Right, at(70)) == Result::Toggle);
    CHECK(!first.shiftDown());

    ShiftTapDetector reversed;
    reversed.shiftPressed(Key::Left, at(0));
    reversed.shiftPressed(Key::Right, at(30));
    CHECK(reversed.shiftReleased(Key::Right, at(60)) == Result::None);
    CHECK(reversed.shiftReleased(Key::Left, at(70)) == Result::Toggle);
}

void testAutoRepeatDoesNotRestartWindow() {
    ShiftTapDetector detector;
    detector.shiftPressed(Key::Left, at(0));
    detector.shiftPressed(Key::Left, at(300)); // X11 auto-repeat
    // If the repeat restarted the window this would be a 150 ms tap.
    CHECK(detector.shiftReleased(Key::Left, at(450)) == Result::None);
}

void testStrayReleaseIsIgnored() {
    ShiftTapDetector detector;
    CHECK(detector.shiftReleased(Key::Left, at(10)) == Result::None);
    detector.shiftPressed(Key::Left, at(20));
    CHECK(detector.shiftReleased(Key::Left, at(80)) == Result::Toggle);
}

void testConsecutiveTapsKeepWorking() {
    ShiftTapDetector detector;
    for (int index = 0; index < 3; ++index) {
        const int base = index * 1000;
        detector.shiftPressed(Key::Left, at(base));
        CHECK(detector.shiftReleased(Key::Left, at(base + 60)) == Result::Toggle);
    }
}

void testAlternatingShiftKeys() {
    ShiftTapDetector detector;
    detector.shiftPressed(Key::Left, at(0));
    CHECK(detector.shiftReleased(Key::Left, at(50)) == Result::Toggle);
    detector.shiftPressed(Key::Right, at(300));
    CHECK(detector.shiftReleased(Key::Right, at(350)) == Result::Toggle);
}

void testCancelAfterTypingThenTapAgain() {
    ShiftTapDetector detector;
    detector.shiftPressed(Key::Left, at(0));
    detector.otherKeyPressed();
    CHECK(detector.shiftReleased(Key::Left, at(100)) == Result::None);

    detector.shiftPressed(Key::Left, at(200));
    CHECK(detector.shiftReleased(Key::Left, at(250)) == Result::Toggle);
}

void testResetClearsHeldState() {
    ShiftTapDetector detector;
    detector.shiftPressed(Key::Left, at(0));
    detector.otherKeyPressed();
    detector.reset();
    CHECK(!detector.shiftDown());
    detector.shiftPressed(Key::Left, at(10));
    CHECK(detector.shiftReleased(Key::Left, at(60)) == Result::Toggle);
}

} // namespace

int main() {
    testQuickTapToggles();
    testTapWindowBoundary();
    testLongHoldIsNotATap();
    testTypingWhileShiftHeldCancels();
    testKeyReleasedBeforeShiftStillCancels();
    testOtherKeyWithoutShiftDoesNotLeak();
    testBothShiftKeysCountAsOneTap();
    testAutoRepeatDoesNotRestartWindow();
    testStrayReleaseIsIgnored();
    testConsecutiveTapsKeepWorking();
    testAlternatingShiftKeys();
    testCancelAfterTypingThenTapAgain();
    testResetClearsHeldState();
    return superior::test::finish("shift tap tests");
}
