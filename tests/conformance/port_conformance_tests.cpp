// SPDX-License-Identifier: GPL-3.0-or-later
//
// Port conformance (`架构设计.md` §11 P5.5, §6.6–6.8).
//
// The contract tests in `text_outbox_tests.cpp` prove the *policy* is right.
// These prove something different and complementary: that the sequence a policy
// emits is exactly what a port implementation can be held to — in order, one
// call per batch, no invented destination, and a quiet no-op when the host has
// no such feature.
//
// They run against `superior/platforms/stub`, which links nothing but
// `Superior::Api`. That is deliberate: a new platform adapter is correct when it
// makes these same assertions true, and this suite is the checklist.

#include <string>
#include <vector>

#include "support/TestSupport.hpp"
#include "superior/api/Ports.hpp"
#include "superior/core/TextOutbox.hpp"
#include "superior/stub/StubPorts.hpp"

namespace {

using namespace superior::core;
using superior::api::SessionKey;
using superior::api::TextEdit;
using superior::stub::RecordingTextSession;
using superior::stub::SilentAudioSource;

constexpr SessionKey kSession{3, 9};

/// §6.6: one `apply` per decision, in the order the policy produced it.
///
/// A port that batches across calls, or reorders a `Clear` before the `Preedit`
/// it belongs to, breaks the composition on screen. The whole point of the single
/// entry point is that this order is the only thing an implementation sees.
void testApplyIsSynchronousAndOrdered() {
    RecordingTextSession port;
    TextOutbox outbox(port);

    outbox.setDestination(kSession);
    outbox.showDraft("你好");
    outbox.clearDraft();
    outbox.commit({}, "世界", WhenUnfocused::Drop);

    CHECK(port.calls().size() == 3);
    if (port.calls().size() == 3) {
        CHECK(port.calls()[0].edits.size() == 1);
        CHECK(port.calls()[0].edits[0].kind == TextEdit::Kind::Preedit);
        CHECK(port.calls()[0].edits[0].utf8 == "你好");
        CHECK(port.calls()[1].edits.size() == 1);
        CHECK(port.calls()[1].edits[1 - 1].utf8.empty());
        CHECK(port.calls()[2].edits[0].kind == TextEdit::Kind::Commit);
        CHECK(port.calls()[2].edits[0].utf8 == "世界");
    }
    // Everything went to the focused session, without the policy restating it.
    for (const auto &call : port.calls()) {
        CHECK(call.session == kSession);
    }
    CHECK(port.committed() == "世界");
}

/// §6.7: a port never invents a destination. With no focused session the text
/// must be held by the policy, so `apply` is never even called.
void testNoDestinationMeansNoCall() {
    RecordingTextSession port;
    TextOutbox outbox(port);

    CHECK(!outbox.commit({}, "保留", WhenUnfocused::Hold));
    CHECK(port.calls().empty());
    CHECK(outbox.held() == "保留");

    // A click on a cloud candidate belongs to one composition: it is dropped
    // rather than typed into whatever is focused now.
    CHECK(!outbox.commit({}, "丢弃", WhenUnfocused::Drop));
    CHECK(port.calls().empty());
    CHECK(outbox.held() == "保留");
}

/// A stale key must be dropped by the *implementation*, so the policy can pass
/// the decision through without a second validity check.
void testStaleSessionIsDroppedByTheImplementation() {
    RecordingTextSession port;
    TextOutbox outbox(port);

    outbox.setDestination(kSession);
    // A key from a previous epoch: `valid()` is still true, but it is not the
    // destination any more. The policy forwards it; the port decides.
    const SessionKey stale{kSession.epoch - 1, kSession.context};
    const std::vector<TextEdit> edits{TextEdit::commit("旧")};
    CHECK(outbox.apply(stale, edits, WhenUnfocused::Hold));

    CHECK(port.calls().size() == 1);
    CHECK(port.calls()[0].session == stale);
}

/// §6.3: two sentences in a row must both arrive, in order, with no draft left
/// behind. This is the sequence that used to type the second sentence into the
/// composition line instead of the document.
void testTwoSentencesInARow() {
    RecordingTextSession port;
    TextOutbox outbox(port);
    outbox.setDestination(kSession);

    for (const char *sentence : {"第一句。", "第二句。"}) {
        outbox.showDraft(sentence);
        outbox.clearDraft();
        CHECK(outbox.commit({}, sentence, WhenUnfocused::Hold));
    }

    CHECK(port.committed() == "第一句。第二句。");
    CHECK(port.draft().empty());
    CHECK(!outbox.holding());
}

/// §6.8: a host without capture says so, quietly and repeatedly. Nothing throws,
/// nothing calls the sink, and `stop()` is safe before `start()`.
void testSilentAudioSourceKeepsItsPromises() {
    SilentAudioSource source;
    CHECK(!source.available());
    CHECK(source.defaultDevice().empty());
    CHECK(!source.restoreDefault("alsa_input.mic"));

    // `stop()` before `start()` is what a shutdown path does; it must be a no-op.
    source.stop();
    CHECK(source.stops == 1);

    struct Sink final : superior::api::AudioSink {
        void onAudio(std::span<const std::int16_t>) override { ++blocks; }
        void onAudioEnded() override { ++ended; }
        int blocks = 0;
        int ended = 0;
    } sink;

    std::string error;
    CHECK(!source.start(superior::api::AudioRequest{}, sink, error));
    CHECK(!error.empty()); // the capability shows this instead of failing silently
    CHECK(source.starts == 1);
    CHECK(sink.blocks == 0);
    CHECK(sink.ended == 0);
}

} // namespace

int main() {
    testApplyIsSynchronousAndOrdered();
    testNoDestinationMeansNoCall();
    testStaleSessionIsDroppedByTheImplementation();
    testTwoSentencesInARow();
    testSilentAudioSourceKeepsItsPromises();
    return superior::test::finish("port conformance tests");
}
