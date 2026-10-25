// SPDX-License-Identifier: GPL-3.0-or-later
//
// The speech panel's decision layer, tested with no microphone, no model and no
// window. Every rule here is a promise the UI and the agent rely on.

#include <string>

#include "support/TestSupport.hpp"
#include "superior/core/SpeechState.hpp"

namespace {

using superior::api::SpeechPhase;
using superior::core::SpeechState;

void testSetupIsAStateNotAnError() {
    SpeechState state;
    CHECK(!state.visible());
    CHECK(!state.canStart());

    state.show(false, false);
    CHECK(state.visible());
    CHECK(state.phase() == SpeechPhase::NeedsSetup);
    CHECK(!state.canStart());
    CHECK(!state.message().empty());

    // Only the engine present is still setup: the model is the bigger gate.
    state.refreshSetup(true, false);
    CHECK(state.phase() == SpeechPhase::NeedsSetup);

    // Both present: ready to record.
    state.refreshSetup(true, true);
    CHECK(state.phase() == SpeechPhase::Ready);
    CHECK(state.canStart());
    CHECK(state.message().empty());
}

void testRecordingFlow() {
    SpeechState state;
    state.show(true, true);
    CHECK(state.beginRecording());
    CHECK(state.recording());
    CHECK(state.busy());

    state.setPartial("今天天");
    CHECK(state.partial() == "今天天");
    // A streaming recogniser may shorten its draft; the state must accept it.
    state.setPartial("今天");
    CHECK(state.partial() == "今天");

    state.setLevel(2.5); // clamped
    CHECK(superior::test::near(state.level(), 1.0));

    state.beginTranscribing();
    CHECK(state.phase() == SpeechPhase::Transcribing);
    CHECK(state.acceptFinal("今天天气不错。"));
    CHECK(state.lastText() == "今天天气不错。");
    CHECK(state.phase() == SpeechPhase::Ready);
}

void testEmptyResultDoesNotCommit() {
    SpeechState state;
    state.show(true, true);
    state.beginRecording();
    state.beginTranscribing();
    CHECK(!state.acceptFinal(""));
    CHECK(state.phase() == SpeechPhase::Ready);
    CHECK(!state.message().empty());
}

/// The reason the panel has a cancel at all: a result that arrives after the
/// user gave up must never be typed into the document.
void testCancelRevokesLateResult() {
    SpeechState state;
    state.show(true, true);
    state.beginRecording();
    state.beginTranscribing();

    state.cancel();
    CHECK(state.phase() == SpeechPhase::Cancelling);
    CHECK(!state.acceptFinal("迟到的结果"));

    state.idle();
    CHECK(state.phase() == SpeechPhase::Ready);
}

void testHideRevokesAndCloses() {
    SpeechState state;
    state.show(true, true);
    state.beginRecording();

    state.hide();
    CHECK(state.phase() == SpeechPhase::Hidden);
    CHECK(!state.visible());
    CHECK(!state.canStart());

    // Reopening must not resurrect the old permission either.
    state.show(true, true);
    CHECK(state.phase() == SpeechPhase::Ready);
}

void testLevellessStatesIgnoreTelemetry() {
    SpeechState state;
    state.show(true, true);
    state.setLevel(0.8);
    state.setPartial("不该出现");
    CHECK(superior::test::near(state.level(), 0.0));
    CHECK(state.partial().empty());

    state.beginRecording();
    state.beginTranscribing();
    state.setLevel(0.9);
    CHECK(superior::test::near(state.level(), 0.0));
}

void testFailureCanRetry() {
    SpeechState state;
    state.show(true, true);
    state.beginRecording();
    state.fail("没有检测到麦克风输入");
    CHECK(state.phase() == SpeechPhase::Failed);
    CHECK(state.canStart());
    CHECK(state.beginRecording());
}

void testSceneCarriesSetupFacts() {
    SpeechState state;
    state.setModel("qwen3-asr-0.6b", "Qwen3-ASR-0.6B", 631000000);
    state.show(true, false);
    const auto scene = state.scene("org.superior.default", 7);
    CHECK(scene.visible);
    CHECK(scene.phase == SpeechPhase::NeedsSetup);
    CHECK(!scene.modelPresent);
    CHECK(scene.enginePresent);
    CHECK(scene.modelId == "qwen3-asr-0.6b");
    CHECK(scene.modelBytes == 631000000);
    CHECK(scene.skinId == "org.superior.default");
    CHECK(scene.skinGeneration == 7);
    CHECK(!scene.message.empty());

    state.hide();
    const auto hidden = state.scene("org.superior.default", 7);
    CHECK(!hidden.visible);
    CHECK(hidden.phase == SpeechPhase::Hidden);
}

void testPhaseNamesAreStable() {
    CHECK(std::string(superior::api::speechPhaseName(SpeechPhase::NeedsSetup)) == "needs-setup");
    CHECK(std::string(superior::api::speechPhaseName(SpeechPhase::Recording)) == "recording");
}

} // namespace

int main() {
    testSetupIsAStateNotAnError();
    testRecordingFlow();
    testEmptyResultDoesNotCommit();
    testCancelRevokesLateResult();
    testHideRevokesAndCloses();
    testLevellessStatesIgnoreTelemetry();
    testFailureCanRetry();
    testSceneCarriesSetupFacts();
    testPhaseNamesAreStable();
    return superior::test::finish("speech state tests");
}
