// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include "superior/core/KeyCode.hpp"

namespace superior::fcitx5 {

/// Translates the X11 keysym Fcitx5 reports into the product's `KeyCode`.
///
/// This is the only place in the tree that knows a keysym number for the purpose
/// of *policy*. The conversion is a pure function so it can be tested without a
/// running Fcitx5, and so the C++ side never has to guess which platform value a
/// key has.
///
/// It is deliberately not the same thing as `pinyin::KeyInput::sym`: librime
/// speaks keysym on every platform (its `keycode` uses X11 numbering even on
/// Windows), so that value stays a keysym and never becomes a `KeyCode`.
///
/// Unknown keys map to `KeyCode::Other`, which the policy treats as "not ours"
/// unless it carries text. Never map several distinct keys onto `Other` when
/// they differ in meaning: the keypad keys are separate members precisely
/// because `KpSubtract` pages and `Minus` types.
[[nodiscard]] core::KeyCode keyCodeFor(std::uint32_t keysym) noexcept;

} // namespace superior::fcitx5
