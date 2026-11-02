// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace superior::api {

/// A key, named the way the product talks about it.
///
/// No platform value appears here on purpose. X11 keysyms, Windows virtual keys
/// and Android key codes are all translated by the host *before* they reach the
/// decision layer. The rule exists because a keysym constant inside the policy
/// makes it know one platform's numbering, and the other two platforms then have
/// to imitate it forever.
///
/// The native value does not disappear: librime speaks X11 keysym on every
/// platform. That translation belongs to the Rime port, not to every host, so
/// `PinyinComposer` receives a `KeyPress` and `LibrimePort` turns it into the
/// wire format internally.
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

/// One key press, already stripped of toolkit types *and* of platform values.
///
/// `text` is the single source of "this key produces something the user can
/// see". A non-zero `text` is what inline English commits and what starts a new
/// composition. The host fills it only for keys whose text it can actually
/// produce, so an unknown layout key arrives with `text == 0` and is forwarded
/// untouched.
struct KeyPress {
    KeyCode code = KeyCode::Other;
    /// The character this key actually produces (Shift applied). Zero when the
    /// key produces nothing the user can see.
    char32_t text = 0;
    /// The character the same physical key produces **without** Shift.
    ///
    /// librime wants the unshifted keysym plus a separate Shift state; passing
    /// the shifted character as the keysym would apply Shift twice. Keeping the
    /// base character here is platform-neutral and gives the Rime port exactly
    /// what it needs without exposing a keysym to every host.
    char32_t baseText = 0;
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool super = false;
    bool release = false;
};

/// A gesture the host's input layer recognised: a *translated* fact, not a
/// decision. Detecting a tap needs raw event timing, so it stays in the host;
/// what a tap *means* is a product rule and lives in `core::decideGesture`.
enum class Gesture {
    None,
    /// Bare Shift press+release.
    ShiftTap,
    /// Bare right-Ctrl press+release.
    RightCtrlTap,
    /// Escape pressed while the microphone is live.
    EscapeWhileSpeaking,
};

} // namespace superior::api
