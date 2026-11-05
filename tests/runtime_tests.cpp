// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <map>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "support/TestSupport.hpp"
#include "superior/api/Composer.hpp"
#include "superior/api/Host.hpp"
#include "superior/api/Key.hpp"
#include "superior/api/Ports.hpp"
#include "superior/api/Scene.hpp"
#include "superior/runtime/Runtime.hpp"
#include "superior/stub/StubPorts.hpp"

namespace {

using superior::api::CompositionFact;
using superior::api::Composer;
using superior::api::ComposerFactory;
using superior::api::HostFacts;
using superior::api::KeyCode;
using superior::api::KeyOutcome;
using superior::api::KeyPress;
using superior::api::PipelineOptions;
using superior::api::SessionKey;
using superior::runtime::Runtime;
using superior::runtime::SpeechPort;

class FakeComposer final : public Composer {
public:
    [[nodiscard]] bool available() const override { return true; }

    KeyOutcome processKey(const KeyPress &key, std::uint64_t) override {
        if (key.text >= U'a' && key.text <= U'z') {
            preedit_.push_back(static_cast<char>(key.text));
            candidates_ = {"A" + preedit_, "B" + preedit_};
            highlighted_ = 0;
            return KeyOutcome{true, {}, true};
        }
        return KeyOutcome{false, {}, false};
    }

    KeyOutcome select(std::size_t row, std::uint64_t) override {
        KeyOutcome outcome;
        if (row < candidates_.size()) {
            outcome.consumed = true;
            outcome.commit = candidates_[row];
            preedit_.clear();
            candidates_.clear();
            highlighted_ = -1;
        }
        return outcome;
    }

    bool moveSelection(int delta) override {
        if (candidates_.empty()) {
            return false;
        }
        const int next = std::clamp(highlighted_ + delta, 0,
                                    static_cast<int>(candidates_.size()) - 1);
        if (next == highlighted_) {
            return false;
        }
        highlighted_ = next;
        return true;
    }

    bool page(int) override { return false; }
    bool commitLetters(std::string &out) override {
        out = preedit_;
        reset();
        return !out.empty();
    }
    void reset() override {
        preedit_.clear();
        candidates_.clear();
        highlighted_ = -1;
    }
    void setOption(std::string_view name, bool value) override {
        options[std::string(name)] = value;
    }
    bool getOption(std::string_view name) override {
        const auto found = options.find(std::string(name));
        return found != options.end() && found->second;
    }

    [[nodiscard]] CompositionFact frame() const override {
        CompositionFact fact;
        fact.visible = !preedit_.empty();
        fact.composing = !preedit_.empty();
        fact.preedit = preedit_;
        fact.preeditCursor = preedit_.size();
        fact.highlighted = highlighted_;
        for (std::size_t index = 0; index < candidates_.size(); ++index) {
            superior::api::CandidateFact candidate;
            candidate.text = candidates_[index];
            candidate.engineRow = static_cast<std::uint32_t>(index);
            fact.candidates.push_back(std::move(candidate));
        }
        return fact;
    }

    void tick(std::uint64_t) override {}

    void setOptions(const PipelineOptions &value) override { pipeline = value; }

    /// The state librime leaves after a partial commit: text in the composition
    /// and no candidates ("weibrds" -> 为brds). Driving it directly keeps the
    /// test about the runtime's space rule, not about the fake's key handling.
    void leaveCompositionOnly(std::string text) {
        preedit_ = std::move(text);
        candidates_.clear();
        highlighted_ = -1;
    }

    std::map<std::string, bool> options;
    PipelineOptions pipeline;

private:
    std::string preedit_;
    std::vector<std::string> candidates_;
    int highlighted_ = -1;
};

class FakeFactory final : public ComposerFactory {
public:
    [[nodiscard]] bool available() const override { return true; }
    [[nodiscard]] std::string unavailableReason() const override { return {}; }
    std::unique_ptr<Composer> create(std::string &) override {
        auto composer = std::make_unique<FakeComposer>();
        last = composer.get();
        return composer;
    }

    FakeComposer *last = nullptr;
};

class FakeScene final : public superior::api::ScenePresenter {
public:
    void present(const superior::api::UiSceneFrame &frame) override { frames.push_back(frame); }
    void setHitMap(const SessionKey &, const superior::api::HitMap &) override {}
    void setVisible(const SessionKey &, bool) override {}

    std::vector<superior::api::UiSceneFrame> frames;
};

class FakeScheduler final : public superior::api::Scheduler {
public:
    [[nodiscard]] bool onOwnerThread() const noexcept override { return true; }
    void post(std::function<void()> work) override { work(); }
    [[nodiscard]] std::uint64_t nowMs() const override { return clock; }
    std::uint64_t after(std::uint64_t, std::function<void()>) override { return 0; }
    void cancel(std::uint64_t) override {}
    std::uint64_t nextFrame(std::function<void(std::uint64_t)>) override { return 0; }

    std::uint64_t clock = 1000;
};

class FakeLifecycle final : public superior::api::LifecyclePort {
public:
    void degraded(superior::api::DegradedReason, const std::string &) override { ++degraded_; }
    void recovered(superior::api::DegradedReason) override { ++recovered_; }
    void sessionFailed(const SessionKey &, const std::string &) override { ++failed_; }

    int degraded_ = 0;
    int recovered_ = 0;
    int failed_ = 0;
};

class FakeNativePanel final : public superior::api::NativePanelSink {
public:
    void show(const SessionKey &, const superior::api::NativePanel &panel) override {
        last = panel;
        ++shows;
    }
    void hide(const SessionKey &) override { ++hides; }

    superior::api::NativePanel last;
    int shows = 0;
    int hides = 0;
};

class FakeStatus final : public superior::api::StatusSink {
public:
    void setStatusItems(std::span<const superior::api::StatusItem> value) override {
        items.assign(value.begin(), value.end());
    }
    bool activate(std::string_view) override { return false; }

    std::vector<superior::api::StatusItem> items;
};

class FakeSpeech final : public SpeechPort {
public:
    [[nodiscard]] bool enabled() const override { return enabled_; }
    [[nodiscard]] bool hotkeyEnabled() const override { return hotkey_; }
    [[nodiscard]] bool busy() const override { return busy_; }
    [[nodiscard]] bool recording() const override { return false; }
    [[nodiscard]] bool canStart() const override { return enabled_; }
    void setEnabled(bool value) override { enabled_ = value; }
    void setHotkeyEnabled(bool value) override { hotkey_ = value; }
    void setModelId(std::string) override {}
    void toggle() override { ++toggles; }
    void toggleByHotkey() override { ++hotkeyToggles; }
    void start() override { ++starts; }
    void stop() override { ++stops; }
    void cancel(std::string_view) override { ++cancels; }
    void reload() override { ++reloads; }
    void tick() override {}
    void holdCommits() override { ++holds; }
    void flushHeld() override { ++flushes; }
    [[nodiscard]] superior::api::SpeechScene scene(std::string_view,
                                                   superior::api::SkinGeneration) const override {
        return {};
    }
    void shutdown() override {}

    bool enabled_ = true;
    bool hotkey_ = true;
    bool busy_ = false;
    int toggles = 0;
    int hotkeyToggles = 0;
    int starts = 0;
    int stops = 0;
    int cancels = 0;
    int reloads = 0;
    int holds = 0;
    int flushes = 0;
};

KeyPress letter(char value) {
    KeyPress key;
    key.text = static_cast<char32_t>(static_cast<unsigned char>(value));
    return key;
}

HostFacts hostFacts() {
    HostFacts host;
    host.cursorValid = true;
    host.cursorRect = superior::api::Rect{10.0, 20.0, 1.0, 20.0};
    host.focused = true;
    host.program = "test";
    return host;
}

std::filesystem::path settingsPath() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "superior-runtime-tests";
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir / "settings.json";
}

void testSpaceCommitsAnUnparseableRemainder() {
    superior::stub::RecordingTextSession text;
    FakeScene scene;
    FakeScheduler scheduler;
    FakeLifecycle lifecycle;
    FakeNativePanel native;
    FakeStatus status;
    FakeFactory factory;
    FakeSpeech speech;

    Runtime runtime(superior::runtime::Ports{text, scene, scheduler, lifecycle, &native, &status},
                    factory, speech,
                    superior::runtime::Options{settingsPath(), "org.superior.default"});
    const SessionKey key{3, 3};
    runtime.focusIn(key, hostFacts());
    CHECK(factory.last != nullptr);

    // "weibrds": the engine turns "wei" into a word and cannot segment "brds",
    // so after the pick the composition carries text but no candidates. Space
    // then types that text, exactly as librime's own space key would.
    CHECK(runtime.key(key, letter('a'), hostFacts()));
    factory.last->leaveCompositionOnly("为brds");
    KeyPress space;
    space.code = KeyCode::Space;
    CHECK(runtime.key(key, space, hostFacts()));
    CHECK(text.committed() == "为brds");
    CHECK(scene.frames.back().panel.preedit.text.empty());
    CHECK(!scene.frames.back().panel.visible);

    // The same rule covers a bare unparseable syllable: nothing selectable, so
    // space is a commit of the letters rather than a key that does nothing.
    factory.last->leaveCompositionOnly("brds");
    CHECK(runtime.key(key, space, hostFacts()));
    CHECK(text.committed() == "为brdsbrds");
    CHECK(!scene.frames.back().panel.visible);
}

void testKeySelectionAndMode() {
    superior::stub::RecordingTextSession text;
    FakeScene scene;
    FakeScheduler scheduler;
    FakeLifecycle lifecycle;
    FakeNativePanel native;
    FakeStatus status;
    FakeFactory factory;
    FakeSpeech speech;

    Runtime runtime(superior::runtime::Ports{text, scene, scheduler, lifecycle, &native, &status},
                    factory, speech,
                    superior::runtime::Options{settingsPath(), "org.superior.default"});

    const SessionKey key{1, 1};
    runtime.focusIn(key, hostFacts());
    CHECK(factory.last != nullptr);
    CHECK(!scene.frames.empty());
    CHECK(factory.last->options["emoji"]);
    CHECK(!factory.last->options["ascii_mode"]);

    CHECK(runtime.key(key, letter('a'), hostFacts()));
    CHECK(factory.last->frame().preedit == "a");
    CHECK(scene.frames.back().panel.visible);
    CHECK(scene.frames.back().panel.preedit.text == "a");
    CHECK(native.shows > 0);

    KeyPress space;
    space.code = KeyCode::Space;
    CHECK(runtime.key(key, space, hostFacts()));
    CHECK(text.committed() == "Aa");
    CHECK(!scene.frames.back().panel.visible);

    // A Shift tap switches to English; letters commit directly.
    CHECK(runtime.gesture(key, superior::api::Gesture::ShiftTap, hostFacts()));
    CHECK(runtime.english(key));
    CHECK(runtime.key(key, letter('x'), hostFacts()));
    CHECK(text.committed() == "Aax");

    bool foundEnglish = false;
    for (const auto &item : status.items) {
        if (item.id == "inline-mode" && item.label == "英") {
            foundEnglish = true;
        }
    }
    CHECK(foundEnglish);
}

void testSettingsReachTheCapability() {
    const auto path = settingsPath();
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << R"({"schema":"superior.settings/1","emojiCandidates":false,)"
                  R"("cloudPinyinEnabled":true,"cloudPinyinMinLength":3,)"
                  R"("cloudPinyinIndex":4,"cloudPinyinBackend":"baidu"})";
    }

    superior::stub::RecordingTextSession text;
    FakeScene scene;
    FakeScheduler scheduler;
    FakeLifecycle lifecycle;
    FakeNativePanel native;
    FakeStatus status;
    FakeFactory factory;
    FakeSpeech speech;

    Runtime runtime(superior::runtime::Ports{text, scene, scheduler, lifecycle, &native, &status},
                    factory, speech,
                    superior::runtime::Options{path, "org.superior.default"});
    runtime.focusIn(SessionKey{1, 3}, hostFacts());
    CHECK(factory.last != nullptr);
    CHECK(factory.last->pipeline.cloudEnabled);
    CHECK(factory.last->pipeline.cloudMinLength == 3);
    CHECK(factory.last->pipeline.cloudIndex == 4);
    CHECK(factory.last->pipeline.cloudBackend == "baidu");
    CHECK(!factory.last->options["emoji"]);
}

void testCandidateCommandAndFocusLifecycle() {
    superior::stub::RecordingTextSession text;
    FakeScene scene;
    FakeScheduler scheduler;
    FakeLifecycle lifecycle;
    FakeNativePanel native;
    FakeStatus status;
    FakeFactory factory;
    FakeSpeech speech;

    Runtime runtime(superior::runtime::Ports{text, scene, scheduler, lifecycle, &native, &status},
                    factory, speech,
                    superior::runtime::Options{settingsPath(), "org.superior.default"});
    const SessionKey key{1, 2};
    runtime.focusIn(key, hostFacts());

    CHECK(runtime.key(key, letter('b'), hostFacts()));
    const auto frame = scene.frames.back();
    CHECK(frame.panel.candidates.items.size() == 2);
    const auto second = frame.panel.candidates.items[1];

    runtime.command(superior::api::ActivateCandidate{key, second.id});
    CHECK(text.committed() == "Bb");

    runtime.focusOut(key);
    CHECK(speech.holds > 0);
    CHECK(!scene.frames.back().panel.visible);

    runtime.focusIn(key, hostFacts());
    CHECK(speech.flushes > 0);
}

void testLanguageIndicator() {
    const auto path = settingsPath();
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << R"({"schema":"superior.settings/1","showStatusBar":false})";
    }

    superior::stub::RecordingTextSession text;
    FakeScene scene;
    FakeScheduler scheduler;
    FakeLifecycle lifecycle;
    FakeNativePanel native;
    FakeStatus status;
    FakeFactory factory;
    FakeSpeech speech;

    Runtime runtime(superior::runtime::Ports{text, scene, scheduler, lifecycle, &native, &status},
                    factory, speech,
                    superior::runtime::Options{path, "org.superior.default"});
    const SessionKey key{2, 1};
    runtime.focusIn(key, hostFacts());

    // Shift flips to English: with the toolbar hidden the badge is the only
    // feedback, so it carries both the state and the caret it belongs to.
    CHECK(runtime.gesture(key, superior::api::Gesture::ShiftTap, hostFacts()));
    CHECK(runtime.english(key));
    CHECK(scene.frames.back().indicator.visible);
    CHECK(scene.frames.back().indicator.text == "A");
    CHECK(scene.frames.back().indicator.anchor.valid);

    // The runtime owns the timeout; a tick before the deadline must not unmap it.
    scheduler.clock += 500;
    runtime.tick();
    CHECK(scene.frames.back().indicator.visible);

    scheduler.clock += 500;
    runtime.tick();
    CHECK(!scene.frames.back().indicator.visible);

    // The next tap flips back and re-arms the badge with the Chinese glyph.
    CHECK(runtime.gesture(key, superior::api::Gesture::ShiftTap, hostFacts()));
    CHECK(!runtime.english(key));
    CHECK(scene.frames.back().indicator.visible);
    CHECK(scene.frames.back().indicator.text == "拼");

    // A composition takes the popup: the badge and the candidate list are never
    // on screen together, which is what lets the UI paint both in one window
    // (Treeland places every adopted panel at the same anchor).
    CHECK(runtime.key(key, letter('a'), hostFacts()));
    CHECK(scene.frames.back().panel.visible);
    CHECK(!scene.frames.back().indicator.visible);
    scheduler.clock += 500;
    runtime.tick();
    CHECK(!scene.frames.back().indicator.visible);

    // Losing focus must not leave the badge waiting for a caret that is gone.
    runtime.focusOut(key);
    CHECK(!scene.frames.back().indicator.visible);
    scheduler.clock += 500;
    runtime.tick();
    CHECK(!scene.frames.back().indicator.visible);
}

void testLanguageIndicatorHiddenWithToolbar() {
    const auto path = settingsPath();
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << R"({"schema":"superior.settings/1","showStatusBar":true})";
    }

    superior::stub::RecordingTextSession text;
    FakeScene scene;
    FakeScheduler scheduler;
    FakeLifecycle lifecycle;
    FakeNativePanel native;
    FakeStatus status;
    FakeFactory factory;
    FakeSpeech speech;

    Runtime runtime(superior::runtime::Ports{text, scene, scheduler, lifecycle, &native, &status},
                    factory, speech,
                    superior::runtime::Options{path, "org.superior.default"});
    const SessionKey key{2, 2};
    runtime.focusIn(key, hostFacts());
    CHECK(runtime.gesture(key, superior::api::Gesture::ShiftTap, hostFacts()));
    CHECK(runtime.english(key));
    // The toolbar's 中/英 chip already shows the state; a second badge next to
    // the caret would be noise.
    CHECK(!scene.frames.back().indicator.visible);
}

} // namespace

int main() {
    testKeySelectionAndMode();
    testSpaceCommitsAnUnparseableRemainder();
    testSettingsReachTheCapability();
    testCandidateCommandAndFocusLifecycle();
    testLanguageIndicator();
    testLanguageIndicatorHiddenWithToolbar();
    return superior::test::finish("runtime");
}
