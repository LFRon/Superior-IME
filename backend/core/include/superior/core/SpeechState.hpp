// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

#include "superior/api/Scene.hpp"

namespace superior::core {

/// The speech-recognition panel's state machine.
///
/// This is pure decision logic: it never records audio, never runs a model and
/// never touches a window. It answers three questions:
///
/// - may the microphone start a recording right now;
/// - what must the panel show;
/// - is a finished transcript still allowed to be committed (a cancel revokes
///   that permission, including for a result that arrives later).
class SpeechState {
public:
    /// Opens the panel. Selects `NeedsSetup` when something is missing.
    void show(bool enginePresent, bool modelPresent, const std::string &setupMessage = {});

    /// Closes the panel and revokes any pending commit.
    void hide();

    /// Re-evaluates the setup facts while the panel is open (after a download).
    void refreshSetup(bool enginePresent, bool modelPresent, const std::string &setupMessage = {});

    /// Applies the model description used by the panel and the download hint.
    void setModel(std::string id, std::string name, std::uint64_t bytes);

    /// False when the panel is hidden, in setup, or already busy.
    [[nodiscard]] bool canStart() const noexcept;

    /// Moves to `Recording` and returns true, or does nothing and returns false.
    bool beginRecording();

    /// The service confirmed that capture started.
    void recordingStarted();

    /// Live level in 0..1 and the live transcript. Ignored unless recording.
    void setLevel(double level);
    /// Remaining time until the session cap, or `-1` for unlimited.
    void setRemaining(std::int64_t remainingMs);
    void setPartial(std::string text);

    /// Recording stopped; the final transcript is being produced.
    void beginTranscribing();

    /// A *segment* finalized while the recording continues (the user paused, so
    /// what was said so far becomes real text). Returns true when it may be
    /// committed; the phase stays `Recording`.
    ///
    /// This is what makes dictation feel like typing: text appears while the
    /// user is still speaking, and only the volatile tail stays a draft.
    [[nodiscard]] bool acceptChunk(std::string text);

    /// A final transcript arrived. Returns true when it may be committed.
    ///
    /// Cancelling or hiding revokes permission, so a late result from a
    /// cancelled session can never type into the user's document.
    [[nodiscard]] bool acceptFinal(std::string text);

    /// The user asked to cancel; the panel stays visible and shows `Cancelling`.
    void cancel();

    /// The capability came back to idle after a cancel.
    void idle();

    /// Records a failure. Ignored while cancelling: the user asked to discard the
    /// sentence, so "the engine returned nothing" would be an error about the
    /// action they just took.
    void fail(std::string message);

    [[nodiscard]] api::SpeechPhase phase() const noexcept { return phase_; }
    [[nodiscard]] bool visible() const noexcept { return phase_ != api::SpeechPhase::Hidden; }
    [[nodiscard]] bool recording() const noexcept { return phase_ == api::SpeechPhase::Recording; }
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] const std::string &lastText() const noexcept { return lastText_; }
    [[nodiscard]] const std::string &message() const noexcept { return message_; }
    [[nodiscard]] double level() const noexcept { return level_; }
    [[nodiscard]] const std::string &partial() const noexcept { return partial_; }

    /// Builds the scene section. `skinId`/`generation` come from the caller,
    /// which is the only layer that knows which skin is current.
    [[nodiscard]] api::SpeechScene scene(std::string skinId,
                                         api::SkinGeneration generation) const;

private:
    void enter(api::SpeechPhase phase);

    api::SpeechPhase phase_ = api::SpeechPhase::Hidden;
    bool enginePresent_ = false;
    bool modelPresent_ = false;
    std::string message_;
    std::string modelId_;
    std::string modelName_;
    std::uint64_t modelBytes_ = 0;
    double level_ = 0.0;
    std::int64_t remainingMs_ = -1;
    std::string partial_;
    std::string lastText_;
    /// Set by cancel()/hide(); cleared when a new recording starts.
    bool commitRevoked_ = false;
};

} // namespace superior::core
