// SPDX-License-Identifier: GPL-3.0-or-later
#include <initializer_list>
#include <string>
#include <vector>

#include "support/TestSupport.hpp"

#include "EmojiFont.hpp"

namespace {

using superior::ui::emojiFamilyPreference;
using superior::ui::pickEmojiFamily;

std::vector<std::string> families(std::initializer_list<const char *> names) {
    std::vector<std::string> result;
    result.reserve(names.size());
    for (const char *name : names) {
        result.push_back(name);
    }
    return result;
}

/// The preference order is the whole point: a desktop usually has several emoji
/// fonts, and only naming the colour one produces the picture instead of an
/// outline.
void testPreferenceOrder() {
    const auto installed = families({"Noto Sans CJK SC", "Noto Color Emoji", "Noto Emoji"});
    CHECK(pickEmojiFamily(installed) == "Noto Color Emoji");

    // A Windows machine: only the Microsoft family is there.
    CHECK(pickEmojiFamily(families({"Segoe UI", "Segoe UI Emoji"})) == "Segoe UI Emoji");

    // Preference beats list order: the table decides, not fontconfig.
    CHECK(pickEmojiFamily(families({"Twemoji", "Apple Color Emoji"})) == "Apple Color Emoji");
}

/// A font nobody listed still counts if its name says what it is; otherwise a
/// distribution packaging "SomeNewEmoji" would silently lose the colour.
void testFallbackToAnyEmojiFamily() {
    CHECK(pickEmojiFamily(families({"DejaVu Sans", "Symbola", "FancyEMOJIFont"})) ==
          "FancyEMOJIFont");
    CHECK(pickEmojiFamily(families({"EmojiOne"})) == "EmojiOne");
}

/// No emoji font is not an error: the candidate still draws, just not in colour.
void testNoEmojiFamily() {
    CHECK(pickEmojiFamily(families({"DejaVu Sans", "Noto Sans CJK SC"})).empty());
    CHECK(pickEmojiFamily({}).empty());
    CHECK(!emojiFamilyPreference().empty());
}

} // namespace

int main() {
    testPreferenceOrder();
    testFallbackToAnyEmojiFamily();
    testNoEmojiFamily();
    return superior::test::finish("emoji font tests");
}
