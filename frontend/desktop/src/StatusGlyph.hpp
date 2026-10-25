// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "superior/api/Content.hpp"

namespace superior::ui {

/// Shape used for a status item that is drawn instead of written.
enum class Glyph {
    /// Draw the item's text (the normal case).
    None,
    /// Half-width punctuation: a crescent moon.
    Moon,
    /// Full-width punctuation: a filled circle.
    Circle,
};

/// How far the crescent's "bite" sits from the moon centre, as a direction.
///
/// Screen coordinates: x grows right, y grows *down*. The bite starts on the
/// right (0°) and is rotated counter-clockwise, so 45° points up-right — the
/// crescent opens towards the upper right, which is how Sogou draws it.
struct MoonBite {
    double x = 1.0;
    double y = 0.0;
};

/// Rotation of the crescent, in degrees counter-clockwise.
inline constexpr double kMoonRotationDegrees = 45.0;

[[nodiscard]] MoonBite moonBiteDirection(double degreesCounterClockwise) noexcept;

/// Maps a status item to the glyph that represents it.
///
/// Only the punctuation toggle is drawn: Sogou shows it as a moon/circle rather
/// than the words 「半角标点」/「全角标点」, which are wide and change length as
/// you toggle them (the bar would jump on every click).
[[nodiscard]] Glyph glyphFor(const api::StatusItem &item) noexcept;

} // namespace superior::ui
