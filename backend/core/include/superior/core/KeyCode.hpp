// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace superior::core {

/// A key, named the way the product talks about it.
///
/// No platform value appears here on purpose. X11 keysyms, Windows virtual keys
/// and Android key codes are all translated by the adapter *before* they reach
/// the decision layer (`platforms/fcitx5/KeyTranslation`). The rule exists
/// because a keysym constant inside `core` makes the policy know one platform's
/// numbering, and the other two platforms then have to imitate it forever.
///
/// The native value does not disappear — the adapter still needs it to feed
/// librime, which speaks keysym on every platform. It simply does not travel
/// through the decision layer: `SuperiorAgent` answers "whose key is this" and
/// returns `false`, and `RimeInputMethod` still holds the original event.
///
/// Keypad keys stay distinct from the main block on purpose: `KpSubtract` pages
/// backwards while the main-row `-` types a hyphen. Collapsing them would
/// silently turn typing `-` into paging.
enum class KeyCode : std::uint8_t {
    Other,
    Space,
    Escape,
    Return,
    Tab,
    Backspace,
    Delete,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    PageUp,
    PageDown,
    Plus,
    Minus,
    KpSubtract,
    ShiftLeft,
    ShiftRight,
    ControlLeft,
    ControlRight,
    AltLeft,
    AltRight,
    SuperLeft,
    SuperRight,
};

} // namespace superior::core
