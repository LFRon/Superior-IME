// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "superior/api/Key.hpp"
#include "superior/pinyin/Types.hpp"

namespace superior::pinyin {

/// Translates the product's key model into librime's wire format.
///
/// librime speaks X11 keysym on **every** platform, including Windows. That
/// makes the keysym a property of the engine, not of the host: the translation
/// lives here so a TSF or Android host never has to know an X11 number.
[[nodiscard]] KeyInput rimeKeyFor(const api::KeyPress &key) noexcept;

} // namespace superior::pinyin
