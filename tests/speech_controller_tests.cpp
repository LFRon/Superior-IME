// SPDX-License-Identifier: GPL-3.0-or-later
//
// The speech panel's controller (P5.4), driven with a fake capability and a fake
// clock: no microphone, no model, no child process, no Fcitx5.
//
// What is pinned here is the part that used to be tangled with the agent and
// could only be checked by talking into a microphone:
//
// 1. the panel's setup facts decide whether a process is spawned at all;
// 2. a password field refuses dictation before anything is recorded;
// 3. a transcript that arrives after a cancel is not typed;
// 4. a capability that dies — or a phase that never ends — resets the panel
//    instead of leaving "recording" on screen forever;
// 5. speech reaches the document exactly once, through the outbox, and is held
//    rather than dropped when nothing is focused.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "support/TestSupport.hpp"
#include "superior/api/Speech.hpp"
#include "superior/api/TextEdit.hpp"
#include "superior/core/TextOutbox.hpp"
#include "SpeechController.h"
#include "superior/stub/StubPorts.hpp"

namespace {

using superior::api::SessionKey;
using superior::api::SpeechPhase;
using superior::api::TextEdit;
using superior::core::TextOutbox;
using superior::fcitx5::SpeechCapability;
using superior::fcitx5::SpeechController;
using superior::stub::RecordingTextSession;

constexpr SessionKey kSession{3, 9};

/// A recogniser that never exists: records what would have been sent.
class FakeCapability final : public SpeechCapability {
public:
    [[nodiscard]] bool running() const override { return running_; }

    bool start(LineHandler handler, std::string &error) override {
        ++starts;
        if (!installed) {
            error = "the binary is not there";
            return false;
        }
        handler_ = std::move(handler);
        running_ = true;
        return true;
    }

    bool send(std::string_view line) override {
        lines.emplace_back(line);
        return running_;
    }

    void stop() override {
        ++stops;
        running_ = false;
    }

    /// Feeds one line back, as the child would.
    void deliver(std::string_view line) {
        if (handler_) {
            handler_(line);
        }
    }

    /// Simulates the child exiting on its own.
    void die() { running_ = false; }

    [[nodiscard]] bool sawCommand(std::string_view needle) const {
        for (const auto &line : lines) {
            if (line.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    bool installed = true;
    int starts = 0;
    int stops = 0;
    std::vector<std::string> lines;

private:
    bool running_ = false;
    LineHandler handler_;
};

/// Everything the controller needs, all of it fake.
struct Harness {
    RecordingTextSession port;
    TextOutbox outbox{port};
    std::uint64_t now = 1000;
    bool sensitive = false;
    bool engine = true;
    bool model = true;
    int repaints = 0;
    FakeCapability *fake = nullptr;
    std::unique_ptr<SpeechController> controller;

    Harness() {
        SpeechController::Layout layout;
        layout.enginePath = [] { return std::string("/fake/engine"); };
        layout.modelPath = [](std::string_view) { return std::string("/fake/model.gguf"); };
        layout.punctuationPath = [] { return std::string(); };
        layout.enginePresent = [this] { return engine; };
        layout.modelPresent = [this](std::string_view) { return model; };

        controller = std::make_unique<SpeechController>(SpeechController::Deps{
            [this]() -> TextOutbox & { return outbox; },
            [this] { return sensitive; },
            [this] { ++repaints; },
            [this] {
                auto created = std::make_unique<FakeCapability>();
                fake = created.get();
                return std::unique_ptr<SpeechCapability>(std::move(created));
            },
            [this] { return now; },
            layout});
    }
};

void testSetupFactsDecideWhetherAProcessIsSpawned() {
    Harness missing;
    missing.engine = false;
    missing.controller->togglePanel();
    CHECK(missing.controller->visible());
    CHECK(missing.controller->phase() == SpeechPhase::NeedsSetup);
    // Nothing installed, nothing spawned: a setup state is not a crash.
    CHECK(missing.fake == nullptr);
    CHECK(missing.repaints > 0);

    Harness ready;
    ready.controller->togglePanel();
    CHECK(ready.controller->phase() == SpeechPhase::Ready);
    CHECK(ready.fake != nullptr);
    CHECK(ready.fake->starts == 1);
    // The model is pushed to the child on start, never read by it.
    CHECK(ready.fake->sawCommand("\"configure\""));
    CHECK(ready.fake->sawCommand("/fake/model.gguf"));
}

void testPasswordFieldRefusesBeforeRecording() {
    Harness harness;
    harness.sensitive = true;
    harness.controller->togglePanel();
    CHECK(!harness.controller->start());
    CHECK(harness.controller->phase() == SpeechPhase::Failed);
    CHECK(harness.fake->sawCommand("\"start\"") == false);
}

void testFinalTranscriptIsCommittedOnce() {
    Harness harness;
    harness.outbox.setDestination(kSession);
    harness.controller->togglePanel();
    CHECK(harness.controller->start());
    CHECK(harness.controller->recording());

    harness.fake->deliver(R"({"ev":"phase","name":"recording"})");
    // The child ends the utterance on its own after a pause: transcribing
    // first, then the final. A final that arrives while still recording is a
    // protocol violation and is ignored.
    harness.fake->deliver(R"({"ev":"final","text":"提前到达"})");
    CHECK(harness.port.committed().empty());

    harness.fake->deliver(R"({"ev":"phase","name":"transcribing"})");
    harness.fake->deliver(R"({"ev":"final","text":"你好世界"})");
    CHECK(harness.port.committed() == "你好世界");
    CHECK(!harness.controller->recording());

    // A duplicate final is not a second sentence.
    harness.fake->deliver(R"({"ev":"final","text":"你好世界"})");
    CHECK(harness.port.committed() == "你好世界");
}

void testCancelRevokesATranscriptAlreadyOnItsWay() {
    Harness harness;
    harness.outbox.setDestination(kSession);
    harness.controller->togglePanel();
    harness.controller->start();

    harness.controller->cancel("test");
    harness.fake->deliver(R"({"ev":"final","text":"不该上屏"})");
    CHECK(harness.port.committed().empty());
    CHECK(!harness.outbox.holding());
    CHECK(harness.fake->sawCommand("\"cancel\""));
}

void testChunksCommitWhileRecording() {
    Harness harness;
    harness.outbox.setDestination(kSession);
    harness.controller->togglePanel();
    harness.controller->start();

    harness.fake->deliver(R"({"ev":"chunk","text":"第一段"})");
    CHECK(harness.port.committed() == "第一段");
    // Dictation keeps listening: a segment is not the end of the session.
    CHECK(harness.controller->recording());
}

void testDeadCapabilityEndsABusySessionOnly() {
    Harness idle;
    idle.controller->togglePanel();
    idle.fake->die();
    idle.controller->tick();
    // Nothing was in flight, so a dead child is simply the next start's problem.
    CHECK(idle.controller->phase() == SpeechPhase::Ready);

    Harness busy;
    busy.controller->togglePanel();
    busy.controller->start();
    busy.fake->die();
    busy.controller->tick();
    CHECK(busy.controller->phase() == SpeechPhase::Failed);
    CHECK(!busy.controller->state().message().empty());
}

void testStuckPhaseResetsThePanel() {
    Harness harness;
    harness.controller->togglePanel();
    harness.controller->start();
    harness.fake->deliver(R"({"ev":"phase","name":"recording"})");
    CHECK(harness.controller->recording());

    // Recording is capped at 60 s; a phase that outlives it lost an event.
    harness.now += 66000;
    harness.controller->tick();
    CHECK(harness.controller->phase() == SpeechPhase::Failed);
}

void testTranscriptIsHeldWithoutACaretAndFlushedLater() {
    Harness harness;
    harness.controller->togglePanel();
    harness.controller->start();

    // No destination: the user already spoke it, so it must not be dropped.
    harness.fake->deliver(R"({"ev":"phase","name":"transcribing"})");
    harness.fake->deliver(R"({"ev":"final","text":"稍后再打"})");
    CHECK(harness.port.committed().empty());
    CHECK(harness.outbox.holding());

    harness.outbox.setDestination(kSession);
    harness.controller->flushHeld();
    CHECK(harness.port.committed() == "稍后再打");
    CHECK(!harness.outbox.holding());
}

void testPartialsAreDraftsOnly() {
    Harness harness;
    harness.outbox.setDestination(kSession);
    harness.controller->togglePanel();
    harness.controller->start();

    harness.fake->deliver(R"({"ev":"partial","text":"你在"})");
    CHECK(harness.port.draft() == "你在");
    CHECK(harness.port.committed().empty());
    CHECK(harness.port.count(TextEdit::Kind::Commit) == 0);

    // A chunk replaces the draft instead of leaving it on screen.
    harness.fake->deliver(R"({"ev":"chunk","text":"你在说"})");
    CHECK(harness.port.draft().empty());
    CHECK(harness.port.committed() == "你在说");
}

void testTurningTheFeatureOffClosesThePanel() {
    Harness harness;
    harness.controller->togglePanel();
    CHECK(harness.controller->visible());

    harness.controller->setEnabled(false);
    CHECK(!harness.controller->enabled());
    // A hidden switch must not keep a microphone open.
    CHECK(!harness.controller->visible());
}

void testMalformedEventsAreIgnored() {
    Harness harness;
    harness.controller->togglePanel();
    harness.controller->start();

    harness.fake->deliver("not json at all");
    harness.fake->deliver(R"({"ev":"unknown-kind"})");
    CHECK(harness.controller->recording());
    CHECK(harness.port.committed().empty());
}

} // namespace

int main() {
    testSetupFactsDecideWhetherAProcessIsSpawned();
    testPasswordFieldRefusesBeforeRecording();
    testFinalTranscriptIsCommittedOnce();
    testCancelRevokesATranscriptAlreadyOnItsWay();
    testChunksCommitWhileRecording();
    testDeadCapabilityEndsABusySessionOnly();
    testStuckPhaseResetsThePanel();
    testTranscriptIsHeldWithoutACaretAndFlushedLater();
    testPartialsAreDraftsOnly();
    testTurningTheFeatureOffClosesThePanel();
    testMalformedEventsAreIgnored();
    return superior::test::finish("speech controller tests");
}
