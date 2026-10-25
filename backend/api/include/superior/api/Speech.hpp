// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace superior::api {

/// Where the speech-recognition panel is in its lifecycle.
///
/// `NeedsSetup` is a first-class state, not an error: a fresh install has no
/// model, and the panel must say so and offer the download instead of failing.
enum class SpeechPhase {
    Hidden,
    NeedsSetup,
    Ready,
    Recording,
    Transcribing,
    Cancelling,
    Failed,
};

[[nodiscard]] constexpr const char *speechPhaseName(SpeechPhase phase) noexcept {
    switch (phase) {
    case SpeechPhase::Hidden:
        return "hidden";
    case SpeechPhase::NeedsSetup:
        return "needs-setup";
    case SpeechPhase::Ready:
        return "ready";
    case SpeechPhase::Recording:
        return "recording";
    case SpeechPhase::Transcribing:
        return "transcribing";
    case SpeechPhase::Cancelling:
        return "cancelling";
    case SpeechPhase::Failed:
        return "failed";
    }
    return "hidden";
}

} // namespace superior::api
