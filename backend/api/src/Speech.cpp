// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/api/Speech.hpp"

namespace superior::api {

const char *speechPhaseName(SpeechPhase phase) noexcept {
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
