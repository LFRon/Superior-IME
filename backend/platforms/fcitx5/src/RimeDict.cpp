// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/fcitx5/RimeDict.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <vector>

namespace superior::fcitx5 {

namespace {

/// Tone-marked vowels, mapped to the plain letter. `ü` becomes `v`, which is how
/// libime and every cloud provider spell it.
struct ToneMap {
    const char *marked;
    char plain;
};

constexpr ToneMap kTones[] = {
    {"ā", 'a'}, {"á", 'a'}, {"ǎ", 'a'}, {"à", 'a'}, {"ē", 'e'}, {"é", 'e'}, {"ě", 'e'},
    {"è", 'e'}, {"ê", 'e'}, {"ī", 'i'}, {"í", 'i'}, {"ǐ", 'i'}, {"ì", 'i'}, {"ō", 'o'},
    {"ó", 'o'}, {"ǒ", 'o'}, {"ò", 'o'}, {"ū", 'u'}, {"ú", 'u'}, {"ǔ", 'u'}, {"ù", 'u'},
    {"ǖ", 'v'}, {"ǘ", 'v'}, {"ǚ", 'v'}, {"ǜ", 'v'}, {"ü", 'v'}, {"ń", 'n'}, {"ň", 'n'},
    {"ǹ", 'n'}, {"ḿ", 'm'},
    // Uppercase forms appear in a few dictionaries.
    {"Ā", 'a'}, {"Á", 'a'}, {"Ǎ", 'a'}, {"À", 'a'}, {"Ē", 'e'}, {"É", 'e'}, {"Ě", 'e'},
    {"È", 'e'}, {"Ī", 'i'}, {"Í", 'i'}, {"Ǐ", 'i'}, {"Ì", 'i'}, {"Ō", 'o'}, {"Ó", 'o'},
    {"Ǒ", 'o'}, {"Ò", 'o'}, {"Ū", 'u'}, {"Ú", 'u'}, {"Ǔ", 'u'}, {"Ù", 'u'}, {"Ǖ", 'v'},
    {"Ǘ", 'v'}, {"Ǚ", 'v'}, {"Ǜ", 'v'}, {"Ü", 'v'},
};

bool isHanzi(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    // Every code point must be a CJK unified ideograph (3 bytes in UTF-8 in the
    // BMP, 4 outside it). Length is checked per byte below.
    for (std::size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t width = 0;
        if (lead >= 0xE0 && lead < 0xF0) {
            width = 3;
        } else if (lead >= 0xF0) {
            width = 4;
        } else {
            return false; // ASCII or a stray continuation byte
        }
        if (index + width > text.size()) {
            return false;
        }
        index += width;
    }
    return true;
}

/// Converts one Rime syllable to libime's spelling. Returns false when the
/// syllable cannot be represented (digits, Latin letters used as aux codes…).
bool convertSyllable(std::string_view input, std::string &out) {
    out.clear();
    for (std::size_t index = 0; index < input.size();) {
        bool matched = false;
        for (const auto &entry : kTones) {
            const std::string_view marked(entry.marked);
            if (input.compare(index, marked.size(), marked) == 0) {
                out.push_back(entry.plain);
                index += marked.size();
                matched = true;
                break;
            }
        }
        if (matched) {
            continue;
        }
        const auto byte = static_cast<unsigned char>(input[index]);
        if (byte >= 0x80) {
            return false; // an unmapped multi-byte character
        }
        const char lower = static_cast<char>(byte >= 'A' && byte <= 'Z' ? byte - 'A' + 'a' : byte);
        if (lower < 'a' || lower > 'z') {
            return false;
        }
        out.push_back(lower);
        ++index;
    }
    return !out.empty();
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\r' ||
                              value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\r' ||
                              value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

} // namespace

std::int32_t calibrateRimeWeight(double raw, std::int32_t maxWeight, double boost) noexcept {
    if (!(raw > 0.0)) {
        return 0;
    }
    const double safeBoost = boost > 0.0 ? boost : 1.0;
    const double scaled = std::log2(1.0 + raw) * 1000.0 * safeBoost;
    const double bounded = std::min(scaled, static_cast<double>(std::max(0, maxWeight)));
    return static_cast<std::int32_t>(std::lround(bounded));
}

std::string convertRimeDictionary(std::string_view yaml, const RimeConvertOptions &options,
                                  RimeConvertStats &stats) {
    std::string out;
    // Roughly one line per entry; avoids repeated reallocation on a 50 MB file.
    out.reserve(yaml.size() / 3);

    std::unordered_set<std::string> seen;
    bool inBody = false;
    std::string wordKey;

    std::size_t position = 0;
    while (position <= yaml.size()) {
        const auto lineEnd = yaml.find('\n', position);
        const auto length = (lineEnd == std::string_view::npos ? yaml.size() : lineEnd) - position;
        const std::string_view line = yaml.substr(position, length);
        position += length + 1;

        const std::string_view trimmed = trim(line);
        if (!inBody) {
            // Rime keeps metadata before `...`; entries start after it.
            if (trimmed == "...") {
                inBody = true;
            }
            continue;
        }
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        if (lineEnd == std::string_view::npos && trimmed.empty()) {
            break;
        }

        // word <TAB> pinyin [<TAB> weight]
        const auto firstTab = trimmed.find('\t');
        if (firstTab == std::string_view::npos) {
            continue; // "A\tA" style aux lines without pinyin are not entries
        }
        const std::string_view word = trim(trimmed.substr(0, firstTab));
        const auto secondTab = trimmed.find('\t', firstTab + 1);
        const std::string_view pinyin = trim(trimmed.substr(
            firstTab + 1, secondTab == std::string_view::npos
                               ? std::string_view::npos
                               : secondTab - firstTab - 1));

        if (!isHanzi(word)) {
            ++stats.notHanzi;
            continue;
        }

        std::string syllables;
        bool badPinyin = false;
        std::size_t start = 0;
        while (start <= pinyin.size()) {
            const auto space = pinyin.find(' ', start);
            const std::string_view piece = pinyin.substr(
                start, space == std::string_view::npos ? std::string_view::npos : space - start);
            if (!piece.empty()) {
                std::string converted;
                if (!convertSyllable(piece, converted)) {
                    badPinyin = true;
                    break;
                }
                if (!syllables.empty()) {
                    syllables.push_back('\'');
                }
                syllables.append(converted);
            }
            if (space == std::string_view::npos) {
                break;
            }
            start = space + 1;
        }
        if (badPinyin || syllables.empty()) {
            ++stats.badPinyin;
            continue;
        }

        double weight = 100.0;
        if (secondTab != std::string_view::npos) {
            const std::string_view weightText = trim(trimmed.substr(secondTab + 1));
            try {
                weight = std::stod(std::string(weightText));
            } catch (const std::exception &) {
                ++stats.badWeight;
                continue;
            }
        }

        wordKey.assign(word);
        wordKey.push_back('\t');
        wordKey.append(syllables);
        if (!seen.insert(wordKey).second) {
            ++stats.duplicate;
            continue;
        }

        const auto value = options.calibrateWeights
                               ? calibrateRimeWeight(weight, options.maxWeight,
                                                     options.weightBoost)
                               : static_cast<std::int32_t>(std::lround(weight));
        out.append(word);
        out.push_back('\t');
        out.append(syllables);
        out.push_back('\t');
        out.append(std::to_string(value));
        out.push_back('\n');
        ++stats.kept;

        if (options.maxEntries != 0 && stats.kept >= options.maxEntries) {
            break;
        }
    }

    return out;
}

} // namespace superior::fcitx5
