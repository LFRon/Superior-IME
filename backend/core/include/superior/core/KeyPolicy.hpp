// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace superior::core {

/// X11 keysyms used by the policy. These values are the lingua franca between
/// X11, librime and (after translation) Windows VK / Android key codes, so the
/// policy can stay platform neutral.
inline constexpr std::uint32_t kKeyTab = 0xff09U;
inline constexpr std::uint32_t kKeyReturn = 0xff0dU;
inline constexpr std::uint32_t kKeyEscape = 0xff1bU;
inline constexpr std::uint32_t kKeyBackspace = 0xff08U;
inline constexpr std::uint32_t kKeyKpEnter = 0xff8dU;
inline constexpr std::uint32_t kKeyDelete = 0xffffU;
inline constexpr std::uint32_t kKeyPlus = 0x002bU;
inline constexpr std::uint32_t kKeyKpAdd = 0xffabU;
inline constexpr std::uint32_t kKeyKpSubtract = 0xffadU;

/// One key press, already stripped of toolkit types.
struct KeyPress {
    std::uint32_t sym = 0;
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
    /// True when the key produces text in the current layout. Only the platform
    /// knows this (keyboard layout, dead keys), so the caller fills it in.
    bool printable = false;
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
[[nodiscard]] bool isEditingKey(std::uint32_t sym) noexcept;

/// Sogou-style candidate paging: `+` (or keypad `+`) is "next page", keypad `-`
/// is "previous page", anything else is 0.
///
/// The rule lives here rather than in the schema because librime's `key_binder`
/// only ships `minus`/`equal`, and `+` is Shift+`=` — a key the punctuation
/// engine happily commits as text, ending the composition. Returning a step
/// instead of acting keeps this decision testable and reusable by the Windows
/// and Android frontends.
///
/// Modifiers are the caller's business: `+` *is* Shift on most layouts, so a
/// plain "no Shift" test would reject the very key this describes.
[[nodiscard]] int pageStep(std::uint32_t sym) noexcept;

} // namespace superior::core
