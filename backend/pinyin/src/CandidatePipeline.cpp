// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/pinyin/CandidatePipeline.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "superior/cloudpinyin/Cloud.hpp"

namespace superior::pinyin {

namespace {

/// Moves the entry at `index` to the front, preserving its identity.
void moveToFront(std::vector<api::CandidateFact> &entries, std::size_t index) {
    if (index == 0 || index >= entries.size()) {
        return;
    }
    api::CandidateFact entry = std::move(entries[index]);
    entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(index));
    entries.insert(entries.begin(), std::move(entry));
}

std::vector<std::string> textsOf(const std::vector<api::CandidateFact> &entries) {
    std::vector<std::string> texts;
    texts.reserve(entries.size());
    for (const auto &entry : entries) {
        texts.push_back(entry.text);
    }
    return texts;
}

} // namespace

void CandidatePipeline::setEngineId(std::string id) {
    engineId_ = std::move(id);
}

void CandidatePipeline::clear() {
    fact_ = {};
    pinyin_.clear();
    cursor_ = 0;
    highlightedText_.clear();
    lastPreedit_.clear();
    lastPageIndex_ = 1;
    lastEngineHighlight_ = -1;
    lastTexts_.clear();
}

std::optional<std::size_t> CandidatePipeline::indexOfText(
    const std::vector<api::CandidateFact> &entries, std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (entries[index].text == text) {
            return index;
        }
    }
    return std::nullopt;
}

bool CandidatePipeline::sameTexts(const std::vector<api::CandidateFact> &entries,
                                  const std::vector<std::string> &texts) {
    if (entries.size() != texts.size()) {
        return false;
    }
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (entries[index].text != texts[index]) {
            return false;
        }
    }
    return true;
}

void CandidatePipeline::rebuild(const Snapshot &snapshot,
                                const std::vector<config::LearnedWord> &learned,
                                std::string_view cloudText,
                                const api::PipelineOptions &options) {
    fact_ = {};
    fact_.preedit = snapshot.composition.preedit;
    fact_.preeditCursor = snapshot.composition.cursor;
    fact_.pageIndex = snapshot.menu.pageIndex;
    fact_.pageCount = snapshot.menu.pageCount;
    fact_.vertical = true;
    fact_.engineId = engineId_;
    fact_.composing = !fact_.preedit.empty() || !snapshot.menu.candidates.empty();
    fact_.visible = fact_.composing;

    // The raw preedit is the change signal, not the normalised pinyin: a schema
    // whose composition is not pinyin would otherwise look like the same word
    // forever, and a rebuild for the same word (cloud, learned refresh) must not
    // be mistaken for a new one.
    const bool newWord = fact_.preedit != lastPreedit_;
    lastPreedit_ = fact_.preedit;
    pinyin_ = cloudpinyin::normalizePinyin(fact_.preedit).value_or("");

    if (!fact_.composing) {
        cursor_ = 0;
        highlightedText_.clear();
        lastPageIndex_ = snapshot.menu.pageIndex;
        lastEngineHighlight_ = snapshot.menu.highlighted;
        lastTexts_.clear();
        return;
    }

    std::vector<api::CandidateFact> entries;
    entries.reserve(snapshot.menu.candidates.size() + 2);
    for (std::size_t index = 0; index < snapshot.menu.candidates.size(); ++index) {
        const auto &candidate = snapshot.menu.candidates[index];
        api::CandidateFact entry;
        entry.text = candidate.text;
        entry.comment = candidate.comment;
        entry.source = api::CandidateFact::Source::Engine;
        entry.engineRow = static_cast<std::uint32_t>(index);
        entries.push_back(std::move(entry));
    }

    // ---------------------------------------------------------------- learned
    bool learnedAtFront = false;
    if (!pinyin_.empty()) {
        if (const auto preferred = config::preferredWord(learned, pinyin_)) {
            const auto found = std::find_if(
                entries.begin(), entries.end(),
                [&preferred](const api::CandidateFact &entry) { return entry.text == *preferred; });
            if (found != entries.end()) {
                moveToFront(entries, static_cast<std::size_t>(found - entries.begin()));
            } else {
                api::CandidateFact entry;
                entry.text = *preferred;
                entry.source = api::CandidateFact::Source::Learned;
                entries.insert(entries.begin(), std::move(entry));
            }
            learnedAtFront = true;
        }
    }

    // ------------------------------------------------------------------ cloud
    if (options.cloudEnabled && !cloudText.empty() && !pinyin_.empty() &&
        static_cast<int>(pinyin_.size()) >= options.cloudMinLength) {
        const bool alreadyOffered =
            std::any_of(entries.begin(), entries.end(), [cloudText](const api::CandidateFact &entry) {
                return entry.text == cloudText;
            });
        if (!alreadyOffered) {
            const int offset = learnedAtFront ? 1 : 0;
            const auto requested = static_cast<std::ptrdiff_t>(std::max(0, options.cloudIndex - 1 + offset));
            const auto at = std::min<std::ptrdiff_t>(requested,
                                                     static_cast<std::ptrdiff_t>(entries.size()));
            api::CandidateFact entry;
            entry.text = std::string(cloudText);
            entry.source = api::CandidateFact::Source::Cloud;
            entries.insert(entries.begin() + at, std::move(entry));
        }
    }

    // ----------------------------------------------------------------- dedupe
    std::vector<api::CandidateFact> unique;
    unique.reserve(std::min(entries.size(), kMaxCandidates));
    std::unordered_set<std::string> seen;
    for (auto &entry : entries) {
        if (unique.size() >= kMaxCandidates) {
            break;
        }
        if (!seen.insert(entry.text).second) {
            continue;
        }
        unique.push_back(std::move(entry));
    }

    // ----------------------------------------------------------------- cursor
    //
    // The highlight addresses the *merged* page, so the rules are about that
    // list, not about the engine's row numbers:
    //
    // 1. a new word starts on the first row. That row is the learned word when
    //    rule 2 promoted one, and it is what space commits;
    // 2. a page change also starts at the first row: the user asked for another
    //    page, not for the old row to be carried over;
    // 3. otherwise a rebuilt list of the same word (a cloud answer, a learned
    //    refresh) keeps the marker on the entry it was on;
    // 4. an engine that *moved its own* highlight is followed, because some
    //    schemas bind their own selection keys. Its row 0 is deliberately not
    //    followed: after rule 2 moved a learned word in front, the engine's row
    //    0 sits on the second line, which is the "the marker jumped to the
    //    second candidate" bug.
    const int engineHighlighted = snapshot.menu.highlighted;
    const bool engineMoved = engineHighlighted != lastEngineHighlight_;
    lastEngineHighlight_ = engineHighlighted;

    int cursor = cursor_;
    if (unique.empty()) {
        cursor = 0;
    } else if (engineHighlighted > 0 && (newWord || engineMoved)) {
        cursor = 0;
        for (std::size_t index = 0; index < unique.size(); ++index) {
            if (unique[index].source == api::CandidateFact::Source::Engine &&
                unique[index].engineRow == static_cast<std::uint32_t>(engineHighlighted)) {
                cursor = static_cast<int>(index);
                break;
            }
        }
    } else if (newWord) {
        cursor = 0;
    } else if (snapshot.menu.pageIndex != lastPageIndex_) {
        // Paging is a deliberate "show me the next page": its first row is the
        // one to act on, exactly like a fresh word. `lastPageIndex_` is what
        // separates this from a cloud answer landing on the current page.
        cursor = 0;
    } else if (!sameTexts(unique, lastTexts_)) {
        cursor = static_cast<int>(indexOfText(unique, highlightedText_).value_or(0));
    }
    // `std::clamp` with a negative upper bound is undefined, so the empty page
    // (a preedit whose syllables produced no candidates) keeps the row at 0.
    cursor_ = unique.empty() ? 0
                             : std::clamp(cursor, 0, static_cast<int>(unique.size()) - 1);

    fact_.highlighted = unique.empty() ? -1 : cursor_;
    fact_.candidates = std::move(unique);
    highlightedText_ = fact_.candidates.empty()
                           ? std::string{}
                           : fact_.candidates[static_cast<std::size_t>(cursor_)].text;
    lastPageIndex_ = snapshot.menu.pageIndex;
    lastTexts_ = textsOf(fact_.candidates);
}

PipelineSelection CandidatePipeline::selectionAt(std::size_t row) const {
    PipelineSelection selection;
    if (row >= fact_.candidates.size()) {
        return selection;
    }
    const auto &entry = fact_.candidates[row];
    if (entry.source == api::CandidateFact::Source::Engine) {
        selection.kind = PipelineSelection::Kind::Engine;
        selection.engineRow = entry.engineRow;
    } else {
        selection.kind = PipelineSelection::Kind::Direct;
        selection.text = entry.text;
    }
    return selection;
}

bool CandidatePipeline::moveSelection(int delta) {
    if (fact_.candidates.empty()) {
        return false;
    }
    const int next = std::clamp(cursor_ + delta, 0, static_cast<int>(fact_.candidates.size()) - 1);
    if (next == cursor_) {
        return false;
    }
    cursor_ = next;
    fact_.highlighted = cursor_;
    highlightedText_ = fact_.candidates[static_cast<std::size_t>(cursor_)].text;
    return true;
}

} // namespace superior::pinyin
