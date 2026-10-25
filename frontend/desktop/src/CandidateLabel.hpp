// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

namespace superior::ui {

/// One candidate rendered as "1. 原神".
struct CandidateLabelParts {
    /// Number shown before the text, already including the separator ("1.").
    /// Empty when the candidate has nothing to be numbered against.
    std::string prefix;
    /// The candidate itself.
    std::string text;
};

/// Formats a candidate for display.
///
/// `label` is Fcitx5's own label, which most engines fill with the candidate
/// number but may be empty or already carry a separator. The rules:
/// - a usable label wins, so engines that number differently (or use ①) keep
///   their identity;
/// - a trailing separator is stripped and re-added uniformly, so "1" and "1."
///   both render as "1.";
/// - an empty label falls back to `index + 1`, which is what makes numbering
///   work on engines that leave labels blank;
/// - when the engine gave no text (some engines put everything in the label),
///   the label is shown as-is without a prefix — no "1. 1".
[[nodiscard]] CandidateLabelParts formatCandidateLabel(std::string_view label,
                                                       std::string_view text, int index);

} // namespace superior::ui
