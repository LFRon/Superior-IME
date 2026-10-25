// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/fcitx5/PreeditSource.hpp"

namespace superior::fcitx5 {

namespace {

/// Clamps a byte cursor to a UTF-8 boundary inside `text`.
///
/// A client can report a cursor that does not fall on a character boundary (or
/// past the end after an engine edit). Drawing would split a code point and
/// produce a replacement glyph, so snap backwards to a boundary.
std::size_t clampCursor(const std::string &text, std::size_t cursor) {
    if (cursor >= text.size()) {
        return text.size();
    }
    while (cursor > 0 && (static_cast<unsigned char>(text[cursor]) & 0xC0U) == 0x80U) {
        --cursor;
    }
    return cursor;
}

} // namespace

PreeditView choosePreedit(const std::string &panelPreedit, std::size_t panelCursor,
                          const std::string &clientPreedit, std::size_t clientCursor) {
    if (!panelPreedit.empty()) {
        return PreeditView{panelPreedit, clampCursor(panelPreedit, panelCursor), false};
    }
    if (!clientPreedit.empty()) {
        return PreeditView{clientPreedit, clampCursor(clientPreedit, clientCursor), true};
    }
    return PreeditView{};
}

} // namespace superior::fcitx5
