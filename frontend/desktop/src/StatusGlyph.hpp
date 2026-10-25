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

/// Maps a status item to the glyph that represents it.
///
/// Only the punctuation toggle is drawn: Sogou shows it as a moon/circle rather
/// than the words 「半角标点」/「全角标点」, which are wide and change length as
/// you toggle them (the bar would jump on every click).
[[nodiscard]] Glyph glyphFor(const api::StatusItem &item) noexcept;

} // namespace superior::ui
