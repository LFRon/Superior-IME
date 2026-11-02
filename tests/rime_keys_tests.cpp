// SPDX-License-Identifier: GPL-3.0-or-later
#include "support/TestSupport.hpp"
#include "superior/api/Key.hpp"
#include "superior/pinyin/RimeKeys.hpp"

namespace {

using superior::api::KeyCode;
using superior::api::KeyPress;
using superior::pinyin::rimeKeyFor;

void testShiftIsNotAppliedTwice() {
    KeyPress key;
    key.baseText = U'a';
    key.text = U'A';
    key.shift = true;
    const auto wire = rimeKeyFor(key);
    CHECK(wire.sym == static_cast<std::uint32_t>('a'));
    CHECK((wire.states & 1U) != 0U);
}

void testShiftedPunctuationUsesTheBaseKey() {
    KeyPress key;
    key.baseText = U'1';
    key.text = U'!';
    key.shift = true;
    const auto wire = rimeKeyFor(key);
    CHECK(wire.sym == static_cast<std::uint32_t>('1'));
}

void testSpecialKeysUseTheirKeysym() {
    KeyPress space;
    space.code = KeyCode::Space;
    CHECK(rimeKeyFor(space).sym == 0x20U);

    KeyPress backspace;
    backspace.code = KeyCode::Backspace;
    CHECK(rimeKeyFor(backspace).sym == 0xFF08U);

    KeyPress pageUp;
    pageUp.code = KeyCode::PageUp;
    CHECK(rimeKeyFor(pageUp).sym == 0xFF55U);
}

void testUnicodeUsesTheX11UnicodeEncoding() {
    KeyPress key;
    key.baseText = U'中';
    key.text = U'中';
    CHECK(rimeKeyFor(key).sym == 0x01000000U + 0x4E2DU);
}

void testReleaseBit() {
    KeyPress key;
    key.baseText = U'a';
    key.release = true;
    CHECK(rimeKeyFor(key).release);
}

} // namespace

int main() {
    testShiftIsNotAppliedTwice();
    testShiftedPunctuationUsesTheBaseKey();
    testSpecialKeysUseTheirKeysym();
    testUnicodeUsesTheX11UnicodeEncoding();
    testReleaseBit();
    return superior::test::finish("rime-keys");
}
