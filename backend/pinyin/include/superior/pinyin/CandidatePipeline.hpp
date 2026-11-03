// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "superior/api/Composition.hpp"
#include "superior/config/Learning.hpp"
#include "superior/pinyin/Types.hpp"

namespace superior::pinyin {

/// What selecting a visible row means.
struct PipelineSelection {
    enum class Kind : std::uint8_t {
        None,
        /// Select `engineRow` in librime and take its commit.
        Engine,
        /// Commit `text` directly (a learned or cloud row).
        Direct,
    };

    Kind kind = Kind::None;
    std::size_t engineRow = 0;
    std::string text;
};

/// Merges the engine page with local history and the cloud answer.
///
/// This is the one place that decides candidate order. It is platform-neutral
/// and has no network, disk or UI dependency: the caller supplies the engine
/// snapshot, the learned words and the current cloud answer.
///
/// Rules (`架构设计.md` §6.3):
/// 1. the engine page is the base; `engineRow` stays the engine's own row;
/// 2. the user's learned word moves to the front, or is prepended when the
///    engine does not offer it;
/// 3. one cloud word is inserted at `cloudIndex - 1`, unless the engine already
///    offers it;
/// 4. duplicates keep the higher-priority row (learned > cloud > engine);
/// 5. the page is capped at `kMaxCandidates`;
/// 6. the highlight belongs to the **merged** list, so it is reset to the first
///    row whenever the user starts a new word. Following the engine's row
///    instead put the marker on the second candidate as soon as rule 2 moved a
///    learned word in front of the engine's own first row, and space then
///    committed the engine row rather than the word shown first.
class CandidatePipeline {
public:
    static constexpr std::size_t kMaxCandidates = 32;

    void setEngineId(std::string id);

    void rebuild(const Snapshot &snapshot,
                 const std::vector<config::LearnedWord> &learned,
                 std::string_view cloudText,
                 const api::PipelineOptions &options);

    void clear();

    [[nodiscard]] const api::CompositionFact &frame() const noexcept { return fact_; }
    [[nodiscard]] std::string_view pinyin() const noexcept { return pinyin_; }
    [[nodiscard]] bool composing() const noexcept { return fact_.composing; }

    [[nodiscard]] PipelineSelection selectionAt(std::size_t row) const;

    /// Moves the highlight by `delta`, clamped. Returns true when it changed.
    bool moveSelection(int delta);

private:
    static bool sameTexts(const std::vector<api::CandidateFact> &entries,
                          const std::vector<std::string> &texts);

    /// Position of the row whose text is `text`, or `std::nullopt`.
    static std::optional<std::size_t> indexOfText(
        const std::vector<api::CandidateFact> &entries, std::string_view text);

    api::CompositionFact fact_;
    std::string engineId_;
    std::string pinyin_;
    int cursor_ = 0;
    /// Text of the highlighted row, used only as an identity token: `cursor_`
    /// is still the selection, and a rebuild of the *same* word (a cloud answer,
    /// a learned-word refresh) re-finds that row instead of clamping the old
    /// index onto a list whose rows moved.
    std::string highlightedText_;
    /// Preedit of the last rebuild: a change means "a different word", which is
    /// what resets the highlight to the top.
    std::string lastPreedit_;
    /// Page the last rebuild saw, so a page change is distinguishable from a
    /// cloud answer landing on the same page.
    int lastPageIndex_ = 1;
    /// Engine highlight of the last rebuild, to tell an engine-driven move from
    /// the engine simply still reporting its row 0.
    int lastEngineHighlight_ = -1;
    std::vector<std::string> lastTexts_;
};

} // namespace superior::pinyin
