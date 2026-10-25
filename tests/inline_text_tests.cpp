// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>

#include "support/TestSupport.hpp"
#include "superior/fcitx5/InlineText.hpp"

namespace {

void testLetters() {
    CHECK(superior::fcitx5::inlineTextFor(0x61, false) == std::optional<std::string>("a"));
    CHECK(superior::fcitx5::inlineTextFor(0x61, true) == std::optional<std::string>("A"));
    CHECK(superior::fcitx5::inlineTextFor(0x7A, true) == std::optional<std::string>("Z"));
}

void testDigitsAndSymbols() {
    CHECK(superior::fcitx5::inlineTextFor(0x31, false) == std::optional<std::string>("1"));
    CHECK(superior::fcitx5::inlineTextFor(0x31, true) == std::optional<std::string>("!"));
    CHECK(superior::fcitx5::inlineTextFor(0x2D, true) == std::optional<std::string>("_"));
    CHECK(superior::fcitx5::inlineTextFor(0x3B, true) == std::optional<std::string>(":"));
    CHECK(superior::fcitx5::inlineTextFor(0x20, false) == std::optional<std::string>(" "));
}

void testNonPrintableRejected() {
    CHECK(!superior::fcitx5::inlineTextFor(0xFF0D, false).has_value());  // Return
    CHECK(!superior::fcitx5::inlineTextFor(0xFF08, false).has_value());  // BackSpace
    CHECK(!superior::fcitx5::inlineTextFor(0x09, false).has_value());    // Tab
    CHECK(!superior::fcitx5::inlineTextFor(0x4E2D, false).has_value());  // CJK
}

/// Editing/navigation keys must not be turned into committed text: the agent
/// leaves them to the application, which owns the characters we already
/// committed. A `nullopt` here is what makes Backspace work in English mode.
void testEditingKeysStayWithTheApplication() {
    CHECK(!superior::fcitx5::inlineTextFor(0xFFFF, false).has_value());  // Delete
    CHECK(!superior::fcitx5::inlineTextFor(0xFF51, false).has_value());  // Left
    CHECK(!superior::fcitx5::inlineTextFor(0xFF53, false).has_value());  // Right
    CHECK(!superior::fcitx5::inlineTextFor(0xFF52, false).has_value());  // Up
    CHECK(!superior::fcitx5::inlineTextFor(0xFF54, false).has_value());  // Down
    CHECK(!superior::fcitx5::inlineTextFor(0xFF50, false).has_value());  // Home
    CHECK(!superior::fcitx5::inlineTextFor(0xFF57, false).has_value());  // End
    CHECK(!superior::fcitx5::inlineTextFor(0xFF08, true).has_value());   // Shift+BackSpace
}

} // namespace

int main() {
    testLetters();
    testDigitsAndSymbols();
    testNonPrintableRejected();
    testEditingKeysStayWithTheApplication();
    return superior::test::finish("inline text tests");
}
