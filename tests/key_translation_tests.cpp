// SPDX-License-Identifier: GPL-3.0-or-later
//
// The keysym -> KeyCode table is the seam that keeps platform numbering out of
// the decision layer, so it needs its own regression test.
//
// Two things are pinned here:
//
// 1. every key the policy branches on maps to its own code, so a policy rule can
//    never be bypassed by a translation that quietly returns `Other`;
// 2. the keypad keys stay distinct from the main block. `KpSubtract` pages the
//    candidate list backwards and `Minus` types a hyphen; merging them would
//    turn typing `-` into paging, which is exactly the bug this table exists to
//    prevent.

#include "support/TestSupport.hpp"
#include "superior/fcitx5/KeyTranslation.hpp"

namespace {

using superior::core::KeyCode;
using superior::fcitx5::keyCodeFor;

void testModifiersAndEditingKeysMap() {
    CHECK(keyCodeFor(0xffe1U) == KeyCode::ShiftLeft);
    CHECK(keyCodeFor(0xffe2U) == KeyCode::ShiftRight);
    CHECK(keyCodeFor(0xffe3U) == KeyCode::ControlLeft);
    CHECK(keyCodeFor(0xffe4U) == KeyCode::ControlRight);
    CHECK(keyCodeFor(0xffe9U) == KeyCode::AltLeft);
    CHECK(keyCodeFor(0xffeaU) == KeyCode::AltRight);
    CHECK(keyCodeFor(0xffebU) == KeyCode::SuperLeft);
    CHECK(keyCodeFor(0xffecU) == KeyCode::SuperRight);

    CHECK(keyCodeFor(0xff08U) == KeyCode::Backspace);
    CHECK(keyCodeFor(0xffffU) == KeyCode::Delete);
    CHECK(keyCodeFor(0xff09U) == KeyCode::Tab);
    CHECK(keyCodeFor(0xff0dU) == KeyCode::Return);
    // The keypad Enter is the same key to the user, and `isEditingKey` has always
    // treated them alike.
    CHECK(keyCodeFor(0xff8dU) == KeyCode::Return);
    CHECK(keyCodeFor(0xff1bU) == KeyCode::Escape);
    CHECK(keyCodeFor(0x0020U) == KeyCode::Space);
}

void testNavigationKeysMap() {
    CHECK(keyCodeFor(0xff50U) == KeyCode::Home);
    CHECK(keyCodeFor(0xff51U) == KeyCode::Left);
    CHECK(keyCodeFor(0xff52U) == KeyCode::Up);
    CHECK(keyCodeFor(0xff53U) == KeyCode::Right);
    CHECK(keyCodeFor(0xff54U) == KeyCode::Down);
    CHECK(keyCodeFor(0xff55U) == KeyCode::PageUp);
    CHECK(keyCodeFor(0xff56U) == KeyCode::PageDown);
    CHECK(keyCodeFor(0xff57U) == KeyCode::End);
}

/// The one asymmetry that must survive the refactor: only the keypad minus pages.
void testPagingKeysStayDistinct() {
    CHECK(keyCodeFor(0x002bU) == KeyCode::Plus);   // '+', i.e. Shift+=
    CHECK(keyCodeFor(0xffabU) == KeyCode::Plus);   // keypad '+'
    CHECK(keyCodeFor(0xffadU) == KeyCode::KpSubtract);
    CHECK(keyCodeFor(0x002dU) == KeyCode::Minus);  // plain '-'
    CHECK(keyCodeFor(0x003dU) == KeyCode::Other);  // '=' is not a paging key
}

/// Printable letters are not special keys: the adapter reports their text
/// separately, and the policy only needs "this is not one of my keys".
void testPrintableKeysAreOther() {
    CHECK(keyCodeFor(U'a') == KeyCode::Other);
    CHECK(keyCodeFor(U'Z') == KeyCode::Other);
    CHECK(keyCodeFor(U'1') == KeyCode::Other);
    CHECK(keyCodeFor(0x00e9U) == KeyCode::Other); // é, a printable above ASCII
    CHECK(keyCodeFor(0xffc1U) == KeyCode::Other); // an unmapped function key
}

} // namespace

int main() {
    testModifiersAndEditingKeysMap();
    testNavigationKeysMap();
    testPagingKeysStayDistinct();
    testPrintableKeysAreOther();
    return superior::test::finish("key translation tests");
}
