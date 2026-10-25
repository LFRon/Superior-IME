// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace superior::fcitx5 {

/// Maps a printable keysym to the text that inline English mode should commit.
///
/// Kept free of Fcitx types so it can be unit tested. `sym` uses the X11 keysym
/// value (ASCII for printable Latin keys); `shift` applies the usual US layout
/// shift pairs. Non-printable keys return `std::nullopt` and are left to the
/// application.
[[nodiscard]] std::optional<std::string> inlineTextFor(std::uint32_t sym, bool shift);

} // namespace superior::fcitx5
