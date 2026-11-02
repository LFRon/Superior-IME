// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "superior/cloudpinyin/Cloud.hpp"

namespace superior::cloudpinyin {

/// How well a provider's reading matches what the user typed.
enum class ReadingMatch {
    /// The reading does not account for the input. Never show it.
    None,
    /// Every syllable was typed, but at least one only partly: `yyds` →
    /// yong'yuan'dou'shi, `yuanshenqd` → yuan'shen'qi'dong.
    Abbreviated,
    /// Every syllable typed out.
    Full,
};

/// Lowercase letters with one `'` per syllable boundary the user typed.
///
/// Distinct from `normalizePinyin` on purpose. The URL needs the bare letters
/// (`nihao`); matching needs the boundaries, because they are what tells
/// `xi'an` (2 syllables) from `xian` (1) — without them a vowel-initial syllable
/// can only be matched by guessing.
///
/// Spaces and apostrophes both become `'` (librime uses either depending on the
/// schema), runs collapse, leading/trailing are dropped. Returns `nullopt` when
/// the text is not pinyin at all.
[[nodiscard]] std::optional<std::string> normalizeSegments(std::string_view preedit);

/// True when the input is made only of whole pinyin syllables.
///
/// This is the signal `resolveBackend` routes on, so it errs towards "no": an
/// input that *could* be initials (`xian` = xi+an or xian?) is still whole
/// syllables, because the ambiguity does not change which provider is better.
[[nodiscard]] bool isWholePinyin(std::string_view segments);

/// Syllables of a provider reading.
///
/// Separators win when the provider sent them. Some providers send the reading
/// as one run (`yongyuandeshen`); those are segmented with the syllable table,
/// because "one character per syllable" needs the count and the boundaries are
/// what make it meaningful. Empty when the reading cannot be segmented — the
/// candidate is then unverifiable and must not be shown.
[[nodiscard]] std::vector<std::string_view> readingSyllables(std::string_view reading);

/// Aligns a provider reading against the typed input.
///
/// Each syllable must be typed in full or as a prefix, in order, and together
/// they must consume the whole input. A syllable starting with a vowel needs a
/// separator when it is not the first, or `deepseek` would read as
/// de'e'pi'se'e'ka — the reason the rule exists in Microsoft Pinyin too.
[[nodiscard]] ReadingMatch alignReading(std::string_view segments,
                                        std::string_view reading) noexcept;

/// The candidate text when `proposal` may be shown for `segments`, else
/// `nullopt`.
///
/// Rejects, in this order: an answer that does not cover the whole input; a
/// reading that does not align with it; anything that is not at least two
/// ideographs; and a character count that differs from the syllable count —
/// the last is what a single character per syllable means, and a cloud copy of
/// one character would only displace a local candidate.
[[nodiscard]] std::optional<std::string> usableCandidate(std::string_view segments,
                                                         const Proposal &proposal);

/// First usable candidate, in the provider's own order.
///
/// The provider's order is kept rather than re-scored: the first entry that
/// survives `usableCandidate` is the one that provider considers best, and
/// re-ranking by any local heuristic would be guesswork dressed up as data.
[[nodiscard]] std::optional<std::string> pickCandidate(std::string_view segments,
                                                       std::span<const Proposal> proposals);

/// True when `text` is nothing but ideographs (CJK unified, extensions,
/// compatibility, and U+3007 〇) and there are at least two of them.
[[nodiscard]] bool isIdeographSequence(std::string_view text) noexcept;

/// Number of UTF-8 characters in `text`. Surrogate pairs and 4-byte sequences
/// count once, which is what "one character per syllable" needs.
[[nodiscard]] std::size_t utf8Length(std::string_view text) noexcept;

} // namespace superior::cloudpinyin
