// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/fcitx5/InlineText.hpp"

namespace superior::fcitx5 {

std::optional<std::string> inlineTextFor(std::uint32_t sym, bool shift) {
    if (sym < 0x20 || sym > 0x7E) {
        return std::nullopt;
    }

    char value = static_cast<char>(sym);
    if (value >= 'a' && value <= 'z') {
        if (shift) {
            value = static_cast<char>(value - 'a' + 'A');
        }
        return std::string(1, value);
    }
    if (!shift) {
        return std::string(1, value);
    }

    switch (value) {
    case '1': value = '!'; break;
    case '2': value = '@'; break;
    case '3': value = '#'; break;
    case '4': value = '$'; break;
    case '5': value = '%'; break;
    case '6': value = '^'; break;
    case '7': value = '&'; break;
    case '8': value = '*'; break;
    case '9': value = '('; break;
    case '0': value = ')'; break;
    case '-': value = '_'; break;
    case '=': value = '+'; break;
    case '[': value = '{'; break;
    case ']': value = '}'; break;
    case '\\': value = '|'; break;
    case ';': value = ':'; break;
    case '\'': value = '"'; break;
    case ',': value = '<'; break;
    case '.': value = '>'; break;
    case '/': value = '?'; break;
    case '`': value = '~'; break;
    default: break;
    }
    return std::string(1, value);
}

} // namespace superior::fcitx5
