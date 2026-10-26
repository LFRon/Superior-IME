// SPDX-License-Identifier: GPL-3.0-or-later
//
// `TextOutbox` is the single exit for text, so its rules are pinned here with a
// fake port instead of by typing into a real application.
//
// The four rules that have caused bugs before:
//
// 1. a transcript with no caret is **held**, not dropped — the user already
//    spoke it;
// 2. a clicked candidate with a stale session is **dropped**, because typing it
//    into whatever window now has focus would be wrong;
// 3. a commit ends the draft, and a cleared draft does **not** end the
//    composition (a cancelled transcript must not reset a pinyin composition);
// 4. `flush` is idempotent and ordered.

#include "support/TestSupport.hpp"
#include "superior/core/TextOutbox.hpp"
#include "superior/stub/StubPorts.hpp"

#include <string>
#include <vector>

namespace {

using superior::api::SessionKey;
using superior::api::TextEdit;
using superior::core::TextOutbox;
using superior::core::WhenUnfocused;
/// The recording port is the shipped stub, not a local copy: one fake, used by
/// these tests and by `tests/conformance/`, so the two cannot disagree.
using RecordingPort = superior::stub::RecordingTextSession;

constexpr SessionKey kSession{7, 42};

void testTranscriptIsHeldWithoutDestination() {
    RecordingPort port;
    TextOutbox outbox(port);

    CHECK(!outbox.commit({}, "你好", WhenUnfocused::Hold));
    CHECK(outbox.holding());
    CHECK(outbox.held() == "你好");
    CHECK(port.calls().empty());

    // Still nothing focused: flushing must not lose them.
    outbox.flush();
    CHECK(outbox.holding());
    CHECK(port.calls().empty());
}

void testHeldTextIsWrittenInOrder() {
    RecordingPort port;
    TextOutbox outbox(port);
    CHECK(!outbox.commit({}, "你", WhenUnfocused::Hold));
    CHECK(!outbox.commit({}, "好", WhenUnfocused::Hold));
    CHECK(outbox.held() == "你好");

    outbox.setDestination(kSession);
    outbox.flush();
    CHECK(!outbox.holding());
    CHECK(port.committed() == "你好");
    CHECK(port.calls().size() == 1);
    CHECK(port.calls().front().session == kSession);

    // Flushing again does nothing: a second copy would be text arriving twice.
    outbox.flush();
    CHECK(port.committed() == "你好");
}

void testCandidateIsDroppedWhenTheSessionIsStale() {
    RecordingPort port;
    TextOutbox outbox(port);
    outbox.setDestination(kSession);
    port.clear();

    // The outbox itself cannot tell "stale" from "valid" — that is the port's
    // job (§6.7). What it must guarantee is that a targeted commit is never
    // silently redirected to the *current* destination.
    CHECK(outbox.commit(SessionKey{7, 99}, "词", WhenUnfocused::Drop));
    CHECK(port.calls().size() == 1);
    const SessionKey expected{7, 99};
    CHECK(port.calls().front().session == expected);
    CHECK(!outbox.holding());
}

void testCommitEndsTheDraft() {
    RecordingPort port;
    TextOutbox outbox(port);
    outbox.setDestination(kSession);

    CHECK(outbox.showDraft("你好世"));
    CHECK(outbox.draftShowing());
    CHECK(port.count(TextEdit::Kind::Preedit) == 1);

    // A commit supersedes the draft; the port drops it as part of committing.
    CHECK(outbox.commit({}, "界", WhenUnfocused::Hold));
    CHECK(!outbox.draftShowing());

    // Clearing afterwards must not send anything: there is nothing showing.
    const std::size_t before = port.calls().size();
    outbox.clearDraft();
    CHECK(port.calls().size() == before);
}

/// A cancelled transcript must not reset the engine's composition: the user may
/// be half-way through typing pinyin.
void testClearingADraftDoesNotAbandonTheComposition() {
    RecordingPort port;
    TextOutbox outbox(port);
    outbox.setDestination(kSession);
    CHECK(outbox.showDraft("你好"));
    port.clear();

    outbox.clearDraft();
    CHECK(port.calls().size() == 1);
    CHECK(port.count(TextEdit::Kind::Preedit) == 1);
    CHECK(port.count(TextEdit::Kind::Clear) == 0);
    CHECK(port.calls().front().edits.front().utf8.empty());
}

void testAbandoningTheCompositionSendsClear() {
    RecordingPort port;
    TextOutbox outbox(port);
    outbox.setDestination(kSession);

    outbox.abandonComposition();
    CHECK(port.count(TextEdit::Kind::Clear) == 1);
    CHECK(!outbox.draftShowing());
}

void testDiscardHeldForgetsTheWords() {
    RecordingPort port;
    TextOutbox outbox(port);
    CHECK(!outbox.commit({}, "你好", WhenUnfocused::Hold));
    CHECK(outbox.holding());

    outbox.discardHeld();
    CHECK(!outbox.holding());

    outbox.setDestination(kSession);
    outbox.flush();
    CHECK(port.calls().empty());
}

/// A draft has nowhere to be drawn, so it is never held.
void testDraftWithoutDestinationIsDropped() {
    RecordingPort port;
    TextOutbox outbox(port);
    CHECK(!outbox.showDraft("你好"));
    CHECK(!outbox.draftShowing());
    CHECK(port.calls().empty());
}

} // namespace

int main() {
    testTranscriptIsHeldWithoutDestination();
    testHeldTextIsWrittenInOrder();
    testCandidateIsDroppedWhenTheSessionIsStale();
    testCommitEndsTheDraft();
    testClearingADraftDoesNotAbandonTheComposition();
    testAbandoningTheCompositionSendsClear();
    testDiscardHeldForgetsTheWords();
    testDraftWithoutDestinationIsDropped();
    return superior::test::finish("text outbox");
}
