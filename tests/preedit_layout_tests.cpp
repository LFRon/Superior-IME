// SPDX-License-Identifier: GPL-3.0-or-later
#include "PreeditLayout.hpp"
#include "support/TestSupport.hpp"
#include "superior/fcitx5/PreeditSource.hpp"

namespace {

void testSplitAscii() {
    const auto split = superior::ui::splitPreedit("ni hao", 2);
    CHECK(split.before == "ni");
    CHECK(split.after == " hao");
}

void testSplitAtStartAndEnd() {
    const auto start = superior::ui::splitPreedit("nihao", 0);
    CHECK(start.before.empty());
    CHECK(start.after == "nihao");

    const auto end = superior::ui::splitPreedit("nihao", 5);
    CHECK(end.before == "nihao");
    CHECK(end.after.empty());
}

void testCursorPastEndIsClamped() {
    const auto split = superior::ui::splitPreedit("abc", 99);
    CHECK(split.before == "abc");
    CHECK(split.after.empty());
}

void testSplitOnMultiByteBoundary() {
    // "你好": 6 bytes. Cursor 3 is right between the two characters.
    const std::string text = "你好";
    const auto middle = superior::ui::splitPreedit(text, 3);
    CHECK(middle.before == "你");
    CHECK(middle.after == "好");

    // Cursor 1 or 2 would split "你"; must snap back to 0.
    const auto inside = superior::ui::splitPreedit(text, 2);
    CHECK(inside.before.empty());
    CHECK(inside.after == text);
}

void testEmpty() {
    const auto split = superior::ui::splitPreedit("", 0);
    CHECK(split.before.empty());
    CHECK(split.after.empty());
}

void testPanelPreeditWins() {
    const auto view = superior::fcitx5::choosePreedit("ni", 2, "NIHAO", 5);
    CHECK(view.text == "ni");
    CHECK(view.cursor == 2);
    CHECK(!view.fromClient);
}

void testClientPreeditFallback() {
    // pinyin in "show actual pinyin" mode fills only clientPreedit; without the
    // fallback the composition row would be empty.
    const auto view = superior::fcitx5::choosePreedit("", 0, "ni hao", 3);
    CHECK(view.text == "ni hao");
    CHECK(view.cursor == 3);
    CHECK(view.fromClient);
}

void testBothEmpty() {
    const auto view = superior::fcitx5::choosePreedit("", 0, "", 0);
    CHECK(view.text.empty());
    CHECK(view.cursor == 0);
}

void testClientCursorClampedOnBoundary() {
    const auto view = superior::fcitx5::choosePreedit("", 0, "你好", 1);
    CHECK(view.text == "你好");
    CHECK(view.cursor == 0); // snapped off the middle of "你"
}

} // namespace

int main() {
    testSplitAscii();
    testSplitAtStartAndEnd();
    testCursorPastEndIsClamped();
    testSplitOnMultiByteBoundary();
    testEmpty();
    testPanelPreeditWins();
    testClientPreeditFallback();
    testBothEmpty();
    testClientCursorClampedOnBoundary();
    return superior::test::finish("preedit layout tests");
}
