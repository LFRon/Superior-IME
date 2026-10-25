// SPDX-License-Identifier: GPL-3.0-or-later
#include "support/TestSupport.hpp"
#include "superior/core/KeyPolicy.hpp"

namespace {

using namespace superior::core;

KeyPolicyInput base() {
    KeyPolicyInput input;
    input.key.sym = 'a';
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
        input.printable = true;
        const auto decision = decideKey(input);
        CHECK(decision.intent == KeyIntent::Forward);
        CHECK(!decision.consumed);
    }
}

/// While composing, Escape abandons the composition and nothing is committed.
void testEscapeCancelsComposition() {
    auto input = base();
    input.key.sym = kKeyEscape;
    input.composing = true;
    const auto decision = decideKey(input);
    CHECK(decision.intent == KeyIntent::Cancel);
    CHECK(decision.consumed);
}

/// Backspace while composing belongs to the engine: it deletes one letter of the
/// pinyin, not one character of the client's text.
void testBackspaceWhileComposingGoesToEngine() {
    auto input = base();
    input.key.sym = kKeyBackspace;
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
    input.key.sym = kKeyBackspace;
    input.english = true;
    input.printable = false;
    const auto decision = decideKey(input);
    CHECK(decision.intent == KeyIntent::Forward);
    CHECK(!decision.consumed);
}

void testEditingKeysInEnglishAreForwarded() {
    for (const auto sym : {kKeyBackspace, kKeyDelete, kKeyReturn, kKeyKpEnter, kKeyTab}) {
        auto input = base();
        input.key.sym = sym;
        input.english = true;
        const auto decision = decideKey(input);
        CHECK(decision.intent == KeyIntent::Forward);
        CHECK(!decision.consumed);
    }
}

void testPrintableInEnglishIsCommittedInline() {
    auto input = base();
    input.english = true;
    input.printable = true;
    const auto decision = decideKey(input);
    CHECK(decision.intent == KeyIntent::CommitInline);
    CHECK(decision.consumed);
}

void testEscapeInEnglishReturnsToChinese() {
    auto input = base();
    input.key.sym = kKeyEscape;
    input.english = true;
    const auto decision = decideKey(input);
    CHECK(decision.intent == KeyIntent::ToggleLanguage);
    CHECK(decision.consumed);
}

void testPrintableInChineseStartsComposition() {
    auto input = base();
    input.printable = true;
    const auto decision = decideKey(input);
    CHECK(decision.intent == KeyIntent::Engine);
    CHECK(!decision.consumed);
}

/// Navigation keys in Chinese idle mode must not be swallowed: widgets that
/// never compose still need their arrow keys.
void testNavigationInChineseIdleIsForwarded() {
    auto input = base();
    input.key.sym = 0xff51; // Left
    input.printable = false;
    input.english = false;
    const auto decision = decideKey(input);
    CHECK(decision.intent == KeyIntent::Forward);
}

void testEditingKeyPredicate() {
    CHECK(isEditingKey(kKeyBackspace));
    CHECK(isEditingKey(kKeyDelete));
    CHECK(isEditingKey(kKeyReturn));
    CHECK(isEditingKey(kKeyKpEnter));
    CHECK(isEditingKey(kKeyTab));
    CHECK(!isEditingKey('a'));
    CHECK(!isEditingKey(kKeyEscape));
}

/// `+` is Shift+`=` on most layouts, so the paging decision must be about the
/// keysym alone; the adapter filters out Ctrl/Alt/Super itself.
void testPagingKeys() {
    CHECK(pageStep(kKeyPlus) == 1);
    CHECK(pageStep(kKeyKpAdd) == 1);
    CHECK(pageStep(kKeyKpSubtract) == -1);
    // `-` and `=` keep going to librime's own key_binder.
    CHECK(pageStep(0x002dU) == 0);
    CHECK(pageStep(0x003dU) == 0);
    CHECK(pageStep('a') == 0);
    CHECK(pageStep(kKeyEscape) == 0);
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
