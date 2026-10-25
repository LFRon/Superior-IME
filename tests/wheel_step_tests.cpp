// SPDX-License-Identifier: GPL-3.0-or-later
#include "WheelStep.hpp"
#include "support/TestSupport.hpp"

namespace {

using superior::ui::pageStepForWheel;

void testNotch() {
    CHECK(pageStepForWheel(-120) == 1); // down = next page
    CHECK(pageStepForWheel(-240) == 1);
    CHECK(pageStepForWheel(120) == -1); // up = previous page
    CHECK(pageStepForWheel(360) == -1);
}

void testSmallDeltasIgnored() {
    // Trackpads emit many small deltas; paging on those would flicker.
    CHECK(pageStepForWheel(0) == 0);
    CHECK(pageStepForWheel(15) == 0);
    CHECK(pageStepForWheel(-15) == 0);
    CHECK(pageStepForWheel(119) == 0);
    CHECK(pageStepForWheel(-119) == 0);
}

} // namespace

int main() {
    testNotch();
    testSmallDeltasIgnored();
    return superior::test::finish("wheel step tests");
}
