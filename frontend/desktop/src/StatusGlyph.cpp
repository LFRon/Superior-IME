// SPDX-License-Identifier: GPL-3.0-or-later
#include "StatusGlyph.hpp"

#include <cmath>

namespace superior::ui {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

MoonBite moonBiteDirection(double degreesCounterClockwise) noexcept {
    const double radians = degreesCounterClockwise * kPi / 180.0;
    // Negated y: a counter-clockwise turn moves the bite upwards on screen.
    return MoonBite{std::cos(radians), -std::sin(radians)};
}

Glyph glyphFor(const api::StatusItem &item) noexcept {
    if (item.role == api::StatusRole::Speech) {
        return Glyph::Microphone;
    }
    if (item.role != api::StatusRole::Punctuation) {
        return Glyph::None;
    }
    // `active` is the engine's checked state: punctuation is a toggle, and
    // checked means full width.
    return item.active ? Glyph::Circle : Glyph::Moon;
}

} // namespace superior::ui
