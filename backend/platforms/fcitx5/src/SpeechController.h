// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "superior/core/SpeechState.hpp"
#include "superior/speech_recognition/Layout.hpp"

namespace superior::core {
class TextOutbox;
}

namespace superior::fcitx5 {

/// The recogniser child, as the controller sees it.
///
/// Only four things ever happen to it: start it, ask whether it is alive, write
/// one command line, stop it. `SpeechProcess` is the real implementation; the
/// tests use a fake, which is the whole point of the interface — the panel's
/// state machine is not allowed to depend on a spawned model.
class SpeechCapability {
public:
    using LineHandler = std::function<void(std::string_view)>;

    virtual ~SpeechCapability() = default;

    [[nodiscard]] virtual bool running() const = 0;
    virtual bool start(LineHandler handler, std::string &error) = 0;
    virtual bool send(std::string_view line) = 0;
    virtual void stop() = 0;
};

/// The speech panel's decision layer, plus the capability it needs.
///
/// Everything that is a *judgement* lives here: may the microphone start, is
/// this transcript still allowed to be typed, has a phase been stuck so long
/// that an event must have been lost, does a dead child end the session. What it
/// does not own is where the text goes (`core::TextOutbox`), how the panel is
/// repainted, or where the child process comes from — those arrive as the three
/// effects below, so this class can be tested with a fake capability and no
/// Fcitx5, no Qt and no audio device.
class SpeechController {
public:
    /// Where the platform-specific pieces come from. Defaults are the real ones;
    /// a test overrides them to stay off the filesystem.
    struct Layout {
        std::function<std::string()> enginePath = [] {
            return speech_recognition::resolveEngine();
        };
        std::function<std::string(std::string_view)> modelPath = [](std::string_view id) {
            return speech_recognition::modelPath(id).string();
        };
        std::function<std::string()> punctuationPath = [] {
            return speech_recognition::punctuationModelPath().string();
        };
        std::function<bool()> enginePresent = [] { return speech_recognition::enginePresent(); };
        std::function<bool(std::string_view)> modelPresent = [](std::string_view id) {
            return speech_recognition::modelPresent(id);
        };
    };

    struct Deps {
        /// Where a committed transcript goes (§6.6: the outbox is the only exit).
        std::function<core::TextOutbox &()> outbox;
        /// True while the focused client refuses dictation (a password field).
        std::function<bool()> caretRefusesDictation;
        /// Repaints whatever shows speech state; called after every transition.
        std::function<void()> repaint;
        /// Creates the recogniser child on first use.
        std::function<std::unique_ptr<SpeechCapability>()> createCapability;
        /// Monotonic milliseconds, for "how long has this been in this state".
        std::function<std::uint64_t()> nowMs;
        Layout layout;
    };

    explicit SpeechController(Deps deps);
    ~SpeechController();

    SpeechController(const SpeechController &) = delete;
    SpeechController &operator=(const SpeechController &) = delete;

    /// The toolbar switch (`settings.json: speechRecognitionEnabled`). Turning it
    /// off closes the panel: a hidden feature must not keep a microphone open.
    void setEnabled(bool enabled);
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    /// The model selected in settings, as a catalogue id. Does not reload
    /// anything by itself; call `refreshSetup()` afterwards.
    void setModelId(std::string id);
    [[nodiscard]] const std::string &modelId() const noexcept { return modelId_; }

    /// The hotkey switch (`settings.json: speechRecognitionHotkey`). The decision
    /// to act on the gesture is `core::decideGesture`'s, which the adapter asks
    /// through this accessor.
    void setHotkeyEnabled(bool enabled) noexcept { hotkeyEnabled_ = enabled; }
    [[nodiscard]] bool hotkeyEnabled() const noexcept { return hotkeyEnabled_; }

    [[nodiscard]] const core::SpeechState &state() const noexcept { return state_; }
    [[nodiscard]] bool recording() const noexcept { return state_.recording(); }
    [[nodiscard]] bool busy() const noexcept { return state_.busy(); }
    [[nodiscard]] bool visible() const noexcept { return state_.visible(); }
    [[nodiscard]] bool canStart() const noexcept { return state_.canStart(); }
    [[nodiscard]] api::SpeechPhase phase() const noexcept { return state_.phase(); }
    /// The panel's section of the scene, folded from the state machine.
    [[nodiscard]] api::SpeechScene scene(std::string skinId,
                                         api::SkinGeneration generation) const {
        return state_.scene(std::move(skinId), generation);
    }

    /// Re-reads "is the engine installed / is a model downloaded".
    void refreshSetup();

    /// Opens or closes the panel, cancelling a live session on close.
    void togglePanel();

    /// Right-Ctrl tap: stop while recording, otherwise open and start.
    ///
    /// The `settings.json` gate is `core::decideGesture`'s job, not re-checked
    /// here.
    void toggleByHotkey();

    /// Starts dictation when the capability is ready. Shared by the panel button
    /// and the hotkey so the two paths cannot drift apart.
    bool start();

    /// Ends the utterance and lets the recogniser produce a transcript.
    void stop();

    /// Drops the current utterance without committing it. Used by close, Escape
    /// and focus loss; the reason is only for the log.
    void cancel(std::string_view reason);

    /// Sends the model/engine configuration to the child. Called on start and
    /// whenever the selected model changes, so a running child cannot keep
    /// decoding with the model the user just switched away from.
    void reconfigure();

    /// Polls for a capability that died or a phase that will never end.
    ///
    /// Without it the panel kept showing "recording" forever: the child is the
    /// only thing that sends phase events, so its death left the state machine
    /// waiting for an event that could never arrive, and every later click was
    /// refused because the phase was still busy.
    void tick();

    /// Applies one JSON line from the recogniser.
    void handleEvent(std::string_view line);

    /// Focus lost while dictating: keep recording, stop typing.
    void holdCommits();
    /// Focus gained: type whatever was said while nothing was focused.
    void flushHeld();

    /// Stops the child. Idempotent; called from the destructor.
    void shutdown();

private:
    bool ensureCapability();
    /// Shows the in-flight transcript as the composition draft, so words appear
    /// while speaking and nothing is real until a chunk is accepted.
    void setDraft(std::string_view text);
    void commit(std::string_view text);
    [[nodiscard]] bool engineReady() const;
    [[nodiscard]] bool modelReady() const;
    /// Applies the catalogue entry for `modelId_` to the panel.
    void applyModel();
    void repaint() const;

    Deps deps_;
    core::SpeechState state_;
    std::unique_ptr<SpeechCapability> capability_;
    /// When the speech state last changed. A session cannot outlive
    /// `stuckMs` by much, so a phase that stays busy far longer means an event
    /// was lost — and a lost event used to mean the panel refused every later
    /// click because it still believed a session was running.
    std::uint64_t phaseSinceMs_ = 0;
    std::string modelId_ = "paraformer-zh";
    bool enabled_ = true;
    bool hotkeyEnabled_ = true;
};

} // namespace superior::fcitx5
