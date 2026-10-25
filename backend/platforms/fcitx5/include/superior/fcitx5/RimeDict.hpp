// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace superior::fcitx5 {

/// Why an entry was dropped while converting a Rime dictionary.
enum class RimeSkip {
    None,
    /// The word is not pure Han characters (punctuation, Latin, emoji).
    NotHanzi,
    /// A syllable has a tone mark we cannot map, or a non `a-z` character.
    BadPinyin,
    /// The weight column is not a number.
    BadWeight,
    /// The same (word, pinyin) pair already appeared.
    Duplicate,
};

struct RimeConvertStats {
    std::size_t kept = 0;
    std::size_t notHanzi = 0;
    std::size_t badPinyin = 0;
    std::size_t badWeight = 0;
    std::size_t duplicate = 0;

    [[nodiscard]] std::size_t skipped() const noexcept {
        return notHanzi + badPinyin + badWeight + duplicate;
    }
};

struct RimeConvertOptions {
    /// Rime weights live on a very different scale from libime's: 100 is the
    /// common default, but entries reach 19,260,817. Imported raw, a small
    /// weight loses to libime's own language model, and a huge one swamps every
    /// candidate around it. Compressing with log2 measured out: a word with
    /// Rime weight 100 lands on 6658, which is enough to beat the LM without
    /// dominating.
    bool calibrateWeights = true;
    /// Upper bound for a calibrated weight. Keeps a single absurd value from
    /// owning every candidate for its pinyin.
    std::int32_t maxWeight = 60000;
    /// Multiplies the calibrated weight.
    ///
    /// 1.0 keeps libime's language model in charge (good default): the words it
    /// already prefers stay first. Raising it makes the dictionary weight win
    /// instead, which is closer to Rime's `sort: by_weight`; the cost is that the
    /// surrounding candidates flatten to dictionary order.
    double weightBoost = 1.0;
    /// 0 = no limit. Mostly useful for tests and for sampling a huge file.
    std::size_t maxEntries = 0;
};

/// Maps a raw Rime weight onto libime's scale.
///
/// `log2(1 + w) * 1000`, clamped. Monotonic, so relative order inside the source
/// dictionary survives; the compression is what makes the weights usable next to
/// libime's own scores.
[[nodiscard]] std::int32_t calibrateRimeWeight(double raw,
                                               std::int32_t maxWeight = 60000,
                                               double boost = 1.0) noexcept;

/// Converts one Rime `*.dict.yaml` body into libime's text dictionary format.
///
/// Output lines are `词<TAB>pin'yin<TAB>weight`, which is exactly what
/// `libime_pinyindict` expects. Rime's own front matter (`---` … `...`) and
/// comments are skipped, tone marks are stripped (`ā` → `a`), `ü` becomes `v`,
/// and syllables are joined with `'` — feeding spaces instead produces an
/// **empty** dictionary without any error, which is the trap this function
/// exists to avoid.
[[nodiscard]] std::string convertRimeDictionary(std::string_view yaml,
                                                const RimeConvertOptions &options,
                                                RimeConvertStats &stats);

} // namespace superior::fcitx5
