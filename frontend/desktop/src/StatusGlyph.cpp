// SPDX-License-Identifier: GPL-3.0-or-later
#include "StatusGlyph.hpp"

namespace superior::ui {

Glyph glyphFor(const api::StatusItem &item) noexcept {
    if (item.role != api::StatusRole::Punctuation) {
        return Glyph::None;
    }
    // `active` is the engine's checked state: punctuation is a toggle, and
    // checked means full width.
    return item.active ? Glyph::Circle : Glyph::Moon;
}

} // namespace superior::ui
