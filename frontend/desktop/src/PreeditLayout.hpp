// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>

namespace superior::ui {

/// Composition split at the caret, so it can be painted as
/// `before | caret | after`.
struct PreeditSplit {
    std::string before;
    std::string after;
};

/// Splits UTF-8 `text` at byte offset `cursor`.
///
/// The cursor comes from Fcitx5 and may land past the end (after an engine
/// edit) or inside a multi-byte code point. Both are clamped to a character
/// boundary instead of producing a half-drawn glyph.
[[nodiscard]] PreeditSplit splitPreedit(const std::string &text, std::size_t cursor);

} // namespace superior::ui
