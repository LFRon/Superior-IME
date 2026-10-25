// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>

namespace superior::speech_recognition {

/// How the streaming step adapts to a machine that cannot keep up.
///
/// The step is a latency/CPU trade, and the cost is *quadratic* in the length of
/// the open segment, because every step re-decodes the whole utterance:
///
///     cost ≈ c · L² / step
///
/// so a machine that is comfortably fast in short sentences can still fall
/// behind during one long unbroken one. Guessing the machine's speed from the
/// model name or from the load average is not enough, so this remembers what
/// actually happened: a session that could not keep up raises the step by one
/// notch for the next one, and three clean sessions give one notch back.
///
/// The value is per user, not per model, because it describes the machine.
struct StepTuning {
    /// 0..`kMaxStrikes`. Each strike is `kStepNotchMs` of extra delay.
    int strikes = 0;
    /// Consecutive measurable sessions that kept up.
    int cleanRuns = 0;
};

inline constexpr int kStepNotchMs = 500;
inline constexpr int kMaxStrikes = 4;
inline constexpr int kMinStepMs = 200;
inline constexpr int kMaxStepMs = 3000;
/// A session shorter than this says nothing: the model is still loading.
inline constexpr std::uint64_t kMeasurableSessionMs = 8000;
/// Clean sessions needed before one strike is forgiven.
inline constexpr int kCleanRunsToRelax = 3;

/// The step to actually run, for a model baseline plus what we learned.
[[nodiscard]] inline int stepMsWithStrikes(int baselineMs, int strikes) {
    const int clamped = std::clamp(baselineMs, kMinStepMs, kMaxStepMs);
    const int extra = std::clamp(strikes, 0, kMaxStrikes) * kStepNotchMs;
    return std::min(kMaxStepMs, clamped + extra);
}

/// The tuning to remember after one session.
///
/// `fellBehind` is measured, not guessed: the capture thread notices when the
/// recogniser's pipe stays full long enough that audio is waiting to be handed
/// over. That only happens when decoding is slower than the microphone.
[[nodiscard]] inline StepTuning tunedAfterSession(StepTuning tuning, bool fellBehind,
                                                  std::uint64_t sessionMs) {
    if (fellBehind) {
        tuning.strikes = std::min(kMaxStrikes, tuning.strikes + 1);
        tuning.cleanRuns = 0;
        return tuning;
    }
    if (sessionMs < kMeasurableSessionMs) {
        return tuning;
    }
    if (++tuning.cleanRuns >= kCleanRunsToRelax) {
        tuning.cleanRuns = 0;
        tuning.strikes = std::max(0, tuning.strikes - 1);
    }
    return tuning;
}

/// `<state>/superior-ime/streaming-step` — empty when there is no state dir.
[[nodiscard]] std::filesystem::path defaultTuningPath();

/// Missing, unreadable or hand-mangled files all mean "no strikes yet": a
/// preference that can stop dictation must never fail hard.
[[nodiscard]] StepTuning readTuning(const std::filesystem::path &path);

/// Best effort. A read-only home directory must not break dictation either:
/// returns false when nothing was written, never throws.
[[nodiscard]] bool writeTuning(const std::filesystem::path &path, const StepTuning &tuning);

} // namespace superior::speech_recognition
