// SPDX-License-Identifier: GPL-3.0-or-later
#include "EmojiFont.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace superior::ui {

namespace {

/// Colour emoji families, in the order a Linux desktop is likely to have them.
///
/// `Noto Color Emoji` first: it is what every distribution ships and what is
/// installed on this machine. The rest are here so the same build behaves on
/// Windows and macOS, where the frontend is reused.
constexpr std::array<std::string_view, 8> kPreference{
    "Noto Color Emoji", "Apple Color Emoji",  "Segoe UI Emoji", "Twemoji",
    "JoyPixels",        "EmojiOne Color",     "OpenMoji",       "Noto Emoji",
};

bool mentionsEmoji(std::string_view name) noexcept {
    const auto lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    constexpr std::string_view needle = "emoji";
    if (name.size() < needle.size()) {
        return false;
    }
    for (std::size_t start = 0; start + needle.size() <= name.size(); ++start) {
        std::size_t offset = 0;
        for (; offset < needle.size(); ++offset) {
            if (lower(name[start + offset]) != needle[offset]) {
                break;
            }
        }
        if (offset == needle.size()) {
            return true;
        }
    }
    return false;
}

bool contains(std::span<const std::string> installed, std::string_view family) noexcept {
    return std::any_of(installed.begin(), installed.end(),
                       [family](const std::string &name) { return name == family; });
}

} // namespace

std::span<const std::string_view> emojiFamilyPreference() noexcept {
    return kPreference;
}

std::string pickEmojiFamily(std::span<const std::string> installed) {
    for (const std::string_view preferred : kPreference) {
        if (contains(installed, preferred)) {
            return std::string(preferred);
        }
    }
    for (const std::string &name : installed) {
        if (mentionsEmoji(name)) {
            return name;
        }
    }
    return {};
}

} // namespace superior::ui
