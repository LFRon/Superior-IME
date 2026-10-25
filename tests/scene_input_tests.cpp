// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>
#include <vector>

#include "support/TestSupport.hpp"
#include "superior/core/ImeSession.hpp"
#include "superior/core/SceneInput.hpp"

namespace {

using superior::api::SessionKey;
using superior::core::EngineCandidate;
using superior::core::EngineFacts;
using superior::core::HostFacts;
using superior::core::ImeSession;
using superior::core::updateSession;

EngineFacts composingEngine() {
    EngineFacts engine;
    engine.preedit = "nihao";
    engine.preeditCursor = 5;
    for (int i = 1; i <= 9; ++i) {
        EngineCandidate candidate;
        candidate.label = std::to_string(i);
        candidate.text = "候选" + std::to_string(i);
        candidate.comment = "c" + std::to_string(i);
        engine.candidates.push_back(std::move(candidate));
    }
    engine.pageIndex = 2;
    engine.pageCount = 9;
    engine.highlighted = 3;
    engine.vertical = false;
    engine.engineId = "wanxiang_pure";
    engine.composing = true;
    return engine;
}

HostFacts hostFacts() {
    HostFacts host;
    host.cursorRect = superior::api::Rect{100.0, 200.0, 0.0, 20.0};
    host.cursorValid = true;
    host.scale = 1.25;
    host.program = "kate";
    host.inputMethod = "super-ime";
    host.statusItems.push_back(superior::api::StatusItem{"ime", "拼", "", true, true});
    host.clientPreedit = true;
    host.surroundingText = false;
    return host;
}

void testComposingMapping() {
    ImeSession session(SessionKey{1, 1});
    updateSession(composingEngine(), hostFacts(), session);
    const auto frame = session.snapshot(1);

    CHECK(frame.panel.visible);
    CHECK(frame.panel.preedit.text == "nihao");
    CHECK(frame.panel.preedit.cursor == 5);
    CHECK(frame.panel.candidates.items.size() == 9);
    CHECK(frame.panel.candidates.pageCount == 9);
    CHECK(frame.panel.candidates.pageIndex == 1);
    CHECK(frame.panel.candidates.hasPrevious);
    CHECK(frame.panel.candidates.hasNext);
    CHECK(frame.panel.styleId == "h1");
    CHECK(frame.panel.mode == "wanxiang_pure");
    CHECK(frame.panel.skinId == "org.superior.default");
    CHECK(frame.panel.anchor.valid);
    CHECK(superior::test::near(frame.panel.anchor.cursor.x, 100.0));
    CHECK(superior::test::near(frame.panel.scale, 1.0));

    // The host's status items reach the toolbar unchanged.
    CHECK(frame.status.visible);
    CHECK(frame.status.items.size() == 1);
    CHECK(frame.status.items.front().id == "ime");

    // The highlighted engine row becomes the selected candidate.
    CHECK(session.candidates().selectedIndex() == 3);
}

/// A vertical candidate list flips the style id; the renderer decides what that
/// means, the decision layer only reports the fact.
void testVerticalStyle() {
    auto engine = composingEngine();
    engine.vertical = true;
    ImeSession session(SessionKey{1, 2});
    updateSession(engine, hostFacts(), session);
    CHECK(session.snapshot(1).panel.styleId == "v1");
}

/// L-4: composing but no candidates still shows the pre-edit string.
void testComposingWithoutCandidates() {
    EngineFacts engine;
    engine.preedit = "y";
    engine.preeditCursor = 1;
    engine.composing = true;

    ImeSession session(SessionKey{1, 3});
    updateSession(engine, hostFacts(), session);
    const auto frame = session.snapshot(1);
    CHECK(frame.panel.visible);
    CHECK(frame.panel.preedit.text == "y");
    CHECK(frame.panel.candidates.items.empty());
}

/// No composition: the panel hides and the candidate list is cleared, so a new
/// composition cannot inherit the previous generation's ids.
void testIdleHidesAndClears() {
    ImeSession session(SessionKey{1, 4});
    updateSession(composingEngine(), hostFacts(), session);
    CHECK(session.snapshot(1).panel.visible);
    const auto staleId = session.candidates().idAt(0);

    EngineFacts idle;
    idle.composing = false;
    updateSession(idle, hostFacts(), session);

    const auto frame = session.snapshot(2);
    CHECK(!frame.panel.visible);
    CHECK(frame.panel.candidates.items.empty());
    CHECK(session.candidates().positionOf(staleId) == std::nullopt);
}

/// Page numbers are passed through as 0-based; the adapter is the only place
/// that knows Fcitx5 counts pages from 1.
void testPageNumbersAreNormalised() {
    ImeSession session(SessionKey{1, 5});
    auto engine = composingEngine();
    engine.pageIndex = 1;
    engine.pageCount = 3;
    updateSession(engine, hostFacts(), session);
    const auto frame = session.snapshot(1);
    CHECK(frame.panel.candidates.pageIndex == 0);
    CHECK(frame.panel.candidates.pageCount == 3);
    CHECK(!frame.panel.candidates.hasPrevious);
    CHECK(frame.panel.candidates.hasNext);
}

/// Unknown page count (`0`) must fall back to the model's own paging instead of
/// reporting "one page of everything".
void testUnknownPageCountFallsBackToModel() {
    ImeSession session(SessionKey{1, 6});
    auto engine = composingEngine();
    engine.pageCount = 0;
    engine.pageIndex = 0;
    updateSession(engine, hostFacts(), session);
    const auto frame = session.snapshot(1);
    CHECK(frame.panel.candidates.pageCount == 1);
}

/// The skin id and generation come from the caller, never from the platform.
void testSkinIsPassedThrough() {
    ImeSession session(SessionKey{1, 7});
    updateSession(composingEngine(), hostFacts(), session, "org.test.skin", 42);
    const auto frame = session.snapshot(1);
    CHECK(frame.panel.skinId == "org.test.skin");
    CHECK(frame.panel.skinGeneration == 42);
    CHECK(frame.status.skinId == "org.test.skin");
}

} // namespace

int main() {
    testComposingMapping();
    testVerticalStyle();
    testComposingWithoutCandidates();
    testIdleHidesAndClears();
    testPageNumbersAreNormalised();
    testUnknownPageCountFallsBackToModel();
    testSkinIsPassedThrough();
    return superior::test::finish("scene input tests");
}
