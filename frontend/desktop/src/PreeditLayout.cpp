// SPDX-License-Identifier: GPL-3.0-or-later
#include "PreeditLayout.hpp"

namespace superior::ui {

PreeditSplit splitPreedit(const std::string &text, std::size_t cursor) {
    if (cursor > text.size()) {
        cursor = text.size();
    }
    // Continuation bytes are 10xxxxxx; stepping back to a lead byte keeps the
    // split on a character boundary.
    while (cursor > 0 && cursor < text.size() &&
           (static_cast<unsigned char>(text[cursor]) & 0xC0U) == 0x80U) {
        --cursor;
    }
    return PreeditSplit{text.substr(0, cursor), text.substr(cursor)};
}

} // namespace superior::ui
