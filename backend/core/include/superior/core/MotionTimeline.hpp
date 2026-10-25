// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

#include "superior/skin/Types.hpp"

namespace superior::core {

enum class Easing {
    Linear,
    CubicOut,
    CubicIn,
    CubicInOut,
};

[[nodiscard]] Easing easingFromName(std::string_view name);

/// Applies an easing curve to a normalized time in [0, 1].
[[nodiscard]] double applyEasing(Easing easing, double t);

struct MotionSample {
    /// Interpolated value. Springs may overshoot [from, to]; callers decide
    /// whether to clamp (opacity) or keep (geometry).
    double value = 1.0;
    bool finished = true;
};

/// Deterministic, clock-free motion sampler.
///
/// The owner supplies monotonic milliseconds; this class never reads a clock.
/// The same inputs always produce the same outputs, which is what makes UI
/// animation testable and reproducible across processes.
class MotionTimeline {
public:
    void start(const skin::MotionSpec &spec, std::uint64_t startMs, double from = 0.0,
               double to = 1.0);
    void cancel();

    /// True between start() and cancel(), even if the sampled motion has already
    /// settled. Callers normally clear after `sample().finished`.
    [[nodiscard]] bool active() const noexcept;

    [[nodiscard]] MotionSample sample(std::uint64_t nowMs) const;

    [[nodiscard]] double target() const noexcept;
    [[nodiscard]] double origin() const noexcept;

private:
    [[nodiscard]] MotionSample sampleTween(double elapsedSeconds) const;
    [[nodiscard]] MotionSample sampleSpring(double elapsedSeconds) const;

    skin::MotionSpec spec_{};
    std::uint64_t startMs_ = 0;
    double from_ = 0.0;
    double to_ = 1.0;
    bool active_ = false;
};

} // namespace superior::core
