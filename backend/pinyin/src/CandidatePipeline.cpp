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
    lastTexts_.clear();
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

    const auto normalized = cloudpinyin::normalizePinyin(fact_.preedit);
    pinyin_ = normalized.value_or("");

    if (!fact_.composing) {
        cursor_ = 0;
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
    if (!sameTexts(unique, lastTexts_)) {
        cursor_ = 0;
        const int highlighted = snapshot.menu.highlighted;
        if (highlighted >= 0) {
            for (std::size_t index = 0; index < unique.size(); ++index) {
                if (unique[index].source == api::CandidateFact::Source::Engine &&
                    unique[index].engineRow == static_cast<std::uint32_t>(highlighted)) {
                    cursor_ = static_cast<int>(index);
                    break;
                }
            }
        }
    }
    if (unique.empty()) {
        cursor_ = 0;
    } else {
        cursor_ = std::clamp(cursor_, 0, static_cast<int>(unique.size()) - 1);
    }

    fact_.highlighted = unique.empty() ? -1 : cursor_;
    fact_.candidates = std::move(unique);
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
    return true;
}

} // namespace superior::pinyin
