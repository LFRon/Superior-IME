// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>

#include "PlatformChoice.hpp"
#include "support/TestSupport.hpp"

namespace {

using superior::ui::chooseQtPlatform;

void testOurKnobWins() {
    // The dedicated variable exists for "I really want the native plugin".
    const auto decision = chooseQtPlatform("wayland", "dxcb;xcb", "wayland-0", ":1", "wayland");
    CHECK(decision.value == "wayland");
    CHECK(decision.reason == "override");
}

void testWaylandSessionForcesX11() {
    const auto decision = chooseQtPlatform("", "", "wayland-0", ":1", "wayland");
    CHECK(decision.value == "xcb");
    CHECK(decision.reason == "xwayland");
}

void testWaylandSessionKeepsSessionX11Plugin() {
    // Deepin exports QT_QPA_PLATFORM=dxcb;xcb; keep its plugin choice.
    const auto decision = chooseQtPlatform("", "dxcb;xcb", "wayland-0", ":1", "wayland");
    CHECK(decision.value == "dxcb;xcb");
    CHECK(decision.reason == "session-x11-plugin");
}

void testWaylandSessionOverridesNativeWaylandRequest() {
    // A globally exported QT_QPA_PLATFORM=wayland is not a choice made for this
    // overlay; taking it would silently lose always-on-top.
    const auto decision = chooseQtPlatform("", "wayland", "wayland-0", ":1", "wayland");
    CHECK(decision.value == "xcb");
    CHECK(decision.reason == "xwayland");
}

void testWaylandSessionWithoutSessionType() {
    // Some compositors/launchers do not export XDG_SESSION_TYPE.
    const auto decision = chooseQtPlatform("", "", "wayland-1", ":0", "");
    CHECK(decision.value == "xcb");
}

void testWaylandSessionWithoutXwaylandAvailable() {
    const auto decision = chooseQtPlatform("", "", "wayland-0", "", "wayland");
    CHECK(decision.value == "wayland");
    CHECK(decision.reason == "wayland-only");
}

void testWaylandWithoutXwaylandKeepsSessionDefault() {
    const auto decision = chooseQtPlatform("", "wayland-egl", "wayland-0", "", "wayland");
    CHECK(decision.value == "wayland-egl");
    CHECK(decision.reason == "session-default");
}

void testX11SessionKeepsSessionDefault() {
    const auto decision = chooseQtPlatform("", "dxcb;xcb", "", ":0", "x11");
    CHECK(decision.value == "dxcb;xcb");
    CHECK(decision.reason == "session-default");
}

void testX11Session() {
    const auto decision = chooseQtPlatform("", "", "", ":0", "x11");
    CHECK(decision.value == "xcb");
    CHECK(decision.reason == "x11");
}

void testX11SessionReportedOnlyByDisplay() {
    const auto decision = chooseQtPlatform("", "", "", ":10", "");
    CHECK(decision.value == "xcb");
}

void testHeadlessLeavesQtAlone() {
    const auto decision = chooseQtPlatform("", "", "", "", "");
    CHECK(decision.value.empty());
    CHECK(decision.reason == "default");
}

void testEmptyKnobsAreNotChoices() {
    // An exported-but-blank value must not be treated as a deliberate choice.
    const auto decision = chooseQtPlatform("", "", "wayland-0", ":1", "wayland");
    CHECK(decision.reason == "xwayland");
}

} // namespace

int main() {
    testOurKnobWins();
    testWaylandSessionForcesX11();
    testWaylandSessionKeepsSessionX11Plugin();
    testWaylandSessionOverridesNativeWaylandRequest();
    testWaylandSessionWithoutSessionType();
    testWaylandSessionWithoutXwaylandAvailable();
    testWaylandWithoutXwaylandKeepsSessionDefault();
    testX11SessionKeepsSessionDefault();
    testX11Session();
    testX11SessionReportedOnlyByDisplay();
    testHeadlessLeavesQtAlone();
    testEmptyKnobsAreNotChoices();
    return superior::test::finish("qt platform choice tests");
}
