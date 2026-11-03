// SPDX-License-Identifier: GPL-3.0-or-later
#include <initializer_list>
#include <string>
#include <vector>

#include "support/TestSupport.hpp"

#include "EmojiFont.hpp"

namespace {

using superior::ui::candidateFamilies;
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

/// The concrete family has to lead. Appending the emoji family to a generic
/// "Sans Serif" made Qt resolve the whole font to the emoji face, and its ASCII
/// digits are the dim keycap glyphs: the candidate numbers were unreadable on a
/// Wayland session, where no desktop theme supplies a font.
void testConcreteFamilyLeads() {
    CHECK(candidateFamilies(families({"Sans Serif"}), "OPPO Sans 4.0", "Noto Color Emoji") ==
          families({"OPPO Sans 4.0", "Sans Serif", "Noto Color Emoji"}));
    // Nothing to resolve (no font database answer): the emoji family alone.
    CHECK(candidateFamilies({}, "", "Noto Color Emoji") ==
          families({"Noto Color Emoji"}));
}

/// A name that appears twice must not be listed twice: Qt treats the list as an
/// ordered fallback, and duplicates only confuse the resolver.
void testFamiliesAreDeduplicated() {
    CHECK(candidateFamilies(families({"Noto Sans CJK SC"}), "Noto Sans CJK SC",
                            "Noto Color Emoji") ==
          families({"Noto Sans CJK SC", "Noto Color Emoji"}));
    CHECK(candidateFamilies(families({""}), "Noto Color Emoji", "Noto Color Emoji") ==
          families({"Noto Color Emoji"}));
}

/// Without an emoji font the concrete family is still the one that draws.
void testNoEmojiStillConcrete() {
    CHECK(candidateFamilies(families({"Sans Serif"}), "OPPO Sans 4.0", "") ==
          families({"OPPO Sans 4.0", "Sans Serif"}));
    CHECK(candidateFamilies({}, "", "").empty());
}

} // namespace

int main() {
    testPreferenceOrder();
    testFallbackToAnyEmojiFamily();
    testNoEmojiFamily();
    testConcreteFamilyLeads();
    testFamiliesAreDeduplicated();
    testNoEmojiStillConcrete();
    return superior::test::finish("emoji font tests");
}
