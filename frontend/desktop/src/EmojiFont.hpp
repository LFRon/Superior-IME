// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace superior::ui {

/// Font families that draw emoji as the colour pictures users expect.
///
/// A candidate can be an emoji: the wanxiang schema's `emoji` filter emits them
/// as whole candidates (type 哭, see 😭). Qt does fall back for a missing glyph,
/// but on a desktop with several emoji fonts installed the fallback is not bound
/// to the colour one — measured here, `QFont` alone drew a monochrome outline
/// while the same text with an explicit family drew the yellow face. So the
/// family is named instead of hoped for.
///
/// Most preferred first. Only names that are actually installed are used; see
/// `pickEmojiFamily`.
[[nodiscard]] std::span<const std::string_view> emojiFamilyPreference() noexcept;

/// The family list a candidate font should carry, in priority order.
///
/// `families` is the widget's own list (it may hold a generic name such as
/// "Sans Serif"), `resolved` is the concrete family Qt actually picked for that
/// font, and `emoji` is the colour emoji family to fall back to (empty when none
/// is installed).
///
/// The concrete family goes **first** and the emoji family **last**. With a
/// generic name in front, Qt resolves the whole font to the last family: the
/// candidate text was drawn in Noto Color Emoji, whose ASCII digits are the dim
/// keycap glyphs — which is what "the candidate numbers look wrong" was.
[[nodiscard]] std::vector<std::string> candidateFamilies(
    std::span<const std::string> families, std::string_view resolved, std::string_view emoji);

/// The emoji family to append to the candidate font, or an empty string.
///
/// `installed` is the list of families the platform has (in Qt,
/// `QFontDatabase::families()`). Preference order is honoured first; failing
/// that, any installed family whose name mentions "emoji" is taken, so a font
/// nobody listed here still gets used. Empty means "no emoji font installed":
/// the candidate still draws, it just cannot be in colour.
[[nodiscard]] std::string pickEmojiFamily(std::span<const std::string> installed);

} // namespace superior::ui
