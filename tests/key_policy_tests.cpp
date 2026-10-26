// SPDX-License-Identifier: GPL-3.0-or-later
#include "support/TestSupport.hpp"
#include "superior/core/KeyPolicy.hpp"

namespace {

using namespace superior::core;

KeyPolicyInput base() {
    KeyPolicyInput input;
    input.key.code = KeyCode::Other;
    input.key.text = U'a';
    return input;
}

void testReleaseIsIgnored() {
    auto input = base();
    input.key.release = true;
    const auto decision = decideKey(input);
    CHECK(decision.intent == KeyIntent::Ignore);
    CHECK(!decision.consumed);
}

void testShortcutsAreForwarded() {
    for (int modifier = 0; modifier < 3; ++modifier) {
        auto input = base();
        input.key.control = modifier == 0;
        input.key.alt = modifier == 1;
        input.key.super = modifier == 2;
        input.composing = true;
        const auto decision = decideKey(input);
        CHECK(decision.intent == KeyIntent::Forward);
        CHECK(!decision.consumed);
    }
}

/// While composing, Escape abandons the composition and nothing is committed.
void testEscapeCancelsComposition() {
    auto input = base();
    input.key.code = KeyCode::Escape;
    input.key.text = 0;
    input.composing = true;
    const auto decision = decideKey(input);
    CHECK(decision.intent == KeyIntent::Cancel);
    CHECK(decision.consumed);
}

/// Backspace while composing belongs to the engine: it deletes one letter of the
/// pinyin, not one character of the client's text.
void testBackspaceWhileComposingGoesToEngine() {
    auto input = base();
    input.key.code = KeyCode::Backspace;
    input.key.text = 0;
    input.composing = true;
    const auto decision = decideKey(input);
    CHECK(decision.intent == KeyIntent::Engine);
    CHECK(!decision.consumed);
}

/// This is the regression the old code documented: deleting through
/// `deleteSurroundingText` broke Backspace on clients without surrounding-text
/// support, so the key must reach the application untouched.
void testBackspaceInEnglishIsForwarded() {
    auto input = base();
    input.key.code = KeyCode::Backspace;
    input.key.text = 0;
    input.english = true;
    const auto decision = decideKey(input);
    CHECK(decision.intent == KeyIntent::Forward);
    CHECK(!decision.consumed);
}

void testEditingKeysInEnglishAreForwarded() {
    for (const auto code : {KeyCode::Backspace, KeyCode::Delete, KeyCode::Return, KeyCode::Tab}) {
        auto input = base();
        input.key.code = code;
        input.key.text = 0;
        input.english = true;
        const auto decision = decideKey(input);
        CHECK(decision.intent == KeyIntent::Forward);
        CHECK(!decision.consumed);
    }
}

void testPrintableInEnglishIsCommittedInline() {
    auto input = base();
    input.english = true;
    const auto decision = decideKey(input);
    CHECK(decision.intent == KeyIntent::CommitInline);
    CHECK(decision.consumed);
}

void testEscapeInEnglishReturnsToChinese() {
    auto input = base();
    input.key.code = KeyCode::Escape;
    input.key.text = 0;
    input.english = true;
    const auto decision = decideKey(input);
    CHECK(decision.intent == KeyIntent::ToggleLanguage);
    CHECK(decision.consumed);
}

void testPrintableInChineseStartsComposition() {
    auto input = base();
    const auto decision = decideKey(input);
    CHECK(decision.intent == KeyIntent::Engine);
    CHECK(!decision.consumed);
}

/// Navigation keys in Chinese idle mode must not be swallowed: widgets that
/// never compose still need their arrow keys.
void testNavigationInChineseIdleIsForwarded() {
    auto input = base();
    input.key.code = KeyCode::Left;
    input.key.text = 0;
    input.english = false;
    const auto decision = decideKey(input);
    CHECK(decision.intent == KeyIntent::Forward);
}

void testEditingKeyPredicate() {
    CHECK(isEditingKey(KeyCode::Backspace));
    CHECK(isEditingKey(KeyCode::Delete));
    CHECK(isEditingKey(KeyCode::Return));
    CHECK(isEditingKey(KeyCode::Tab));
    CHECK(!isEditingKey(KeyCode::Other));
    CHECK(!isEditingKey(KeyCode::Escape));
    CHECK(!isEditingKey(KeyCode::Left));
}

/// `+` is Shift+`=` on most layouts, so the paging decision must be about the
/// key alone; the adapter filters out Ctrl/Alt/Super itself.
void testPagingKeys() {
    CHECK(pageStep(KeyCode::Plus) == 1);
    CHECK(pageStep(KeyCode::KpSubtract) == -1);
    // Main-row `-` and `=` keep going to librime's own key_binder: typing a
    // hyphen while a candidate list is open must type a hyphen.
    CHECK(pageStep(KeyCode::Minus) == 0);
    CHECK(pageStep(KeyCode::Other) == 0);
    CHECK(pageStep(KeyCode::Escape) == 0);
}

} // namespace

int main() {
    testReleaseIsIgnored();
    testShortcutsAreForwarded();
    testEscapeCancelsComposition();
    testBackspaceWhileComposingGoesToEngine();
    testBackspaceInEnglishIsForwarded();
    testEditingKeysInEnglishAreForwarded();
    testPrintableInEnglishIsCommittedInline();
    testEscapeInEnglishReturnsToChinese();
    testPrintableInChineseStartsComposition();
    testNavigationInChineseIdleIsForwarded();
    testEditingKeyPredicate();
    testPagingKeys();
    return superior::test::finish("key policy tests");
}
