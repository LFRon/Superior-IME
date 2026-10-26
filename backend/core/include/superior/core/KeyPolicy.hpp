// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include "superior/core/KeyCode.hpp"

namespace superior::core {

/// One key press, already stripped of toolkit types *and* of platform values.
///
/// `text` is the single source of "this key produces something the user can
/// see". It used to be a separate `printable` flag computed by the adapter from
/// the same helper, which meant two fields could disagree; now there is one.
///
/// A non-zero `text` is what inline English commits and what starts a new
/// composition. The adapter fills it only for keys whose text it can actually
/// produce (`InlineText.cpp` covers printable ASCII), so an unknown layout key
/// arrives with `text == 0` and is forwarded untouched — same behaviour as
/// before, with one field fewer to get wrong.
struct KeyPress {
    KeyCode code = KeyCode::Other;
    char32_t text = 0;
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool super = false;
    bool release = false;
};

/// What the decision layer wants to happen with a key.
enum class KeyIntent {
    /// The keyboard event is not ours; let it reach the application untouched.
    Forward,
    /// Hand the key to the active capability (pinyin composer, handwriting, …).
    Engine,
    /// Commit the character this key produces (inline English mode).
    CommitInline,
    /// Switch Chinese/English.
    ToggleLanguage,
    /// Abandon the composition without committing anything.
    Cancel,
    /// Nothing to do (modifier release, dead key).
    Ignore,
};

struct KeyDecision {
    KeyIntent intent = KeyIntent::Forward;
    /// True when the event must not reach the application. `Engine` and
    /// `Forward` are never consumed here: the capability or the client decides.
    bool consumed = false;
};

/// Everything the policy needs to know about the current session.
struct KeyPolicyInput {
    KeyPress key;
    /// The engine currently holds a non-empty composition.
    bool composing = false;
    /// The *product* language mode, i.e. `core::Modes::english()`.
    bool english = false;
};

/// Pure key policy: the single place that decides "who handles this key".
///
/// It exists because the previous implementation answered that question inside
/// the Fcitx5 adapter, with a comment explaining why Backspace must never be
/// consumed (`SuperiorAgent.cpp`). That answer is a product decision, and it has
/// to be testable without a running Fcitx5.
///
/// Rules, in order:
/// 1. releases are ignored (the capability sees them, the policy does not);
/// 2. any Ctrl/Alt/Super combination is forwarded — shortcuts belong to the app;
/// 3. while composing, Escape cancels and everything else goes to the engine;
/// 4. in English mode, Escape returns to Chinese, editing/navigation keys are
///    forwarded, and printable keys are committed inline;
/// 5. otherwise a printable key starts a composition (engine).
[[nodiscard]] KeyDecision decideKey(const KeyPolicyInput &input) noexcept;

/// True for keys the application must always own, even in English mode.
///
/// Backspace is here on purpose: inline text is committed straight to the
/// client, so the client already owns those characters. Deleting them via
/// `deleteSurroundingText` broke Backspace on every client without surrounding
/// text support.
[[nodiscard]] bool isEditingKey(KeyCode code) noexcept;

/// Sogou-style candidate paging: `+` (or keypad `+`) is "next page", keypad `-`
/// is "previous page", anything else is 0.
///
/// The rule lives here rather than in the schema because librime's `key_binder`
/// only ships `minus`/`equal`, and `+` is Shift+`=` — a key the punctuation
/// engine happily commits as text, ending the composition. Returning a step
/// instead of acting keeps this decision testable and reusable by the Windows
/// and Android frontends.
///
/// The asymmetry is deliberate and preserved from the shipped behaviour: the
/// main-row `-` (`KeyCode::Minus`) is **not** a paging key, because typing a
/// hyphen while a candidate list is open must type a hyphen. Only the keypad
/// key pages backwards.
///
/// Modifiers are the caller's business: `+` *is* Shift on most layouts, so a
/// plain "no Shift" test would reject the very key this describes.
[[nodiscard]] int pageStep(KeyCode code) noexcept;

} // namespace superior::core
