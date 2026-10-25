// SPDX-License-Identifier: GPL-3.0-or-later
#include "CandidateLabel.hpp"

#include <array>
#include <cctype>

namespace superior::ui {

namespace {

bool isSpace(char value) {
    return std::isspace(static_cast<unsigned char>(value)) != 0;
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && isSpace(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && isSpace(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

/// Separators engines append to their label. Listed explicitly instead of
/// "any non-ASCII byte" so CJK numerals like ① survive.
constexpr std::array<std::string_view, 9> kSeparators{
    ".", "、", "。", "．", ")", "）", ",", "：", ":"};

std::string_view stripSeparators(std::string_view value) {
    bool changed = true;
    while (changed && !value.empty()) {
        changed = false;
        for (const auto separator : kSeparators) {
            if (value.size() >= separator.size() &&
                value.substr(value.size() - separator.size()) == separator) {
                value.remove_suffix(separator.size());
                changed = true;
                break;
            }
        }
    }
    return trim(value);
}

} // namespace

CandidateLabelParts formatCandidateLabel(std::string_view label, std::string_view text,
                                         int index) {
    CandidateLabelParts parts;
    const std::string_view trimmedText = trim(text);
    const std::string_view trimmedLabel = stripSeparators(trim(label));

    if (trimmedText.empty()) {
        // The engine put the whole candidate in the label. Numbering it would
        // render "1. 1".
        parts.text = std::string(trim(label));
        return parts;
    }

    parts.text = std::string(trimmedText);
    if (trimmedLabel.empty()) {
        parts.prefix = std::to_string(index + 1) + ".";
        return parts;
    }
    // Keep CJK numerals and other engine labels, but normalise the separator.
    parts.prefix = std::string(trimmedLabel) + ".";
    return parts;
}

} // namespace superior::ui
