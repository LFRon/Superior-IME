// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/core/MotionTimeline.hpp"

#include <algorithm>
#include <cmath>

#include "superior/diagnostics/Log.hpp"

namespace superior::core {

namespace diag = ::superior::diagnostics;

namespace {
constexpr double kSettleEpsilon = 1e-3;
constexpr double kMaxSpringSeconds = 30.0;
} // namespace

Easing easingFromName(std::string_view name) {
    if (name == "linear") {
        return Easing::Linear;
    }
    if (name == "cubic-in") {
        return Easing::CubicIn;
    }
    if (name == "cubic-in-out") {
        return Easing::CubicInOut;
    }
    return Easing::CubicOut;
}

double applyEasing(Easing easing, double t) {
    const double clamped = std::clamp(t, 0.0, 1.0);
    switch (easing) {
    case Easing::Linear:
        return clamped;
    case Easing::CubicIn:
        return clamped * clamped * clamped;
    case Easing::CubicInOut:
        if (clamped < 0.5) {
            return 4.0 * clamped * clamped * clamped;
        }
        return 1.0 - std::pow(-2.0 * clamped + 2.0, 3.0) / 2.0;
    case Easing::CubicOut:
        break;
    }
    return 1.0 - std::pow(1.0 - clamped, 3.0);
}

void MotionTimeline::start(const skin::MotionSpec &spec, std::uint64_t startMs, double from,
                           double to) {
    spec_ = spec;
    startMs_ = startMs;
    from_ = from;
    to_ = to;
    active_ = true;

    SUPERIOR_TRACE(diag::categories::Core)
        .field("kind", spec_.kind == skin::MotionSpec::Kind::Spring ? "spring" : "tween")
        .field("start", static_cast<std::uint64_t>(startMs))
        << "motion started";

    if (spec_.kind == skin::MotionSpec::Kind::Tween && spec_.tween.durationMs <= 0.0) {
        active_ = false;
    }
}

void MotionTimeline::cancel() {
    active_ = false;
}

bool MotionTimeline::active() const noexcept {
    return active_;
}

double MotionTimeline::target() const noexcept {
    return to_;
}

double MotionTimeline::origin() const noexcept {
    return from_;
}

MotionSample MotionTimeline::sample(std::uint64_t nowMs) const {
    if (!active_) {
        return MotionSample{to_, true};
    }
    if (nowMs < startMs_) {
        return MotionSample{from_, false};
    }

    const double elapsedSeconds =
        static_cast<double>(nowMs - startMs_) / 1000.0;

    if (spec_.kind == skin::MotionSpec::Kind::Tween) {
        return sampleTween(elapsedSeconds);
    }
    return sampleSpring(elapsedSeconds);
}

MotionSample MotionTimeline::sampleTween(double elapsedSeconds) const {
    const double durationSeconds = std::max(0.0, spec_.tween.durationMs / 1000.0);
    if (durationSeconds <= 0.0) {
        return MotionSample{to_, true};
    }
    const double progress = std::clamp(elapsedSeconds / durationSeconds, 0.0, 1.0);
    const double eased = applyEasing(easingFromName(spec_.tween.easing), progress);
    return MotionSample{from_ + (to_ - from_) * eased, progress >= 1.0};
}

MotionSample MotionTimeline::sampleSpring(double elapsedSeconds) const {
    const double stiffness = std::max(0.0, spec_.spring.stiffness);
    const double damping = std::max(0.0, spec_.spring.damping);
    const double mass = spec_.spring.mass > 0.0 ? spec_.spring.mass : 1.0;

    if (stiffness <= 0.0) {
        return MotionSample{to_, true};
    }

    const double time = std::clamp(elapsedSeconds, 0.0, kMaxSpringSeconds);
    const double omega0 = std::sqrt(stiffness / mass);
    const double zeta = damping / (2.0 * std::sqrt(stiffness * mass));
    const double x0 = from_ - to_;
    constexpr double v0 = 0.0;

    double displacement = 0.0;
    double envelope = 0.0;

    if (zeta < 1.0 - 1e-6) {
        const double omegaD = omega0 * std::sqrt(1.0 - zeta * zeta);
        const double decay = std::exp(-zeta * omega0 * time);
        const double c1 = x0;
        const double c2 = (v0 + zeta * omega0 * x0) / omegaD;
        displacement = decay * (c1 * std::cos(omegaD * time) + c2 * std::sin(omegaD * time));
        envelope = decay * std::sqrt(c1 * c1 + c2 * c2);
    } else if (zeta > 1.0 + 1e-6) {
        const double root = std::sqrt(zeta * zeta - 1.0);
        const double r1 = -omega0 * (zeta - root);
        const double r2 = -omega0 * (zeta + root);
        const double denominator = r1 - r2;
        const double c1 = (v0 - r2 * x0) / denominator;
        const double c2 = (r1 * x0 - v0) / denominator;
        displacement = c1 * std::exp(r1 * time) + c2 * std::exp(r2 * time);
        envelope = std::abs(c1) * std::exp(r1 * time) + std::abs(c2) * std::exp(r2 * time);
    } else {
        const double decay = std::exp(-omega0 * time);
        displacement = (x0 + (v0 + omega0 * x0) * time) * decay;
        envelope = (std::abs(x0) + std::abs(v0 + omega0 * x0) * time) * decay;
    }

    if (envelope < kSettleEpsilon || time >= kMaxSpringSeconds) {
        return MotionSample{to_, true};
    }
    return MotionSample{to_ + displacement, false};
}

} // namespace superior::core
