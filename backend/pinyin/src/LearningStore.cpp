// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/pinyin/LearningStore.hpp"

#include <unordered_map>
#include <utility>

#include "superior/cloudpinyin/Cloud.hpp"
#include "superior/diagnostics/Log.hpp"

namespace superior::pinyin {

namespace {

namespace diag = ::superior::diagnostics;

/// The same spelling can arrive as `xiu fu`, `xiu'fu` or `xiufu`. Learning is
/// keyed by the bare letters so all three are one decision.
std::string normalizeKey(std::string_view pinyin) {
    if (const auto normalized = cloudpinyin::normalizePinyin(pinyin)) {
        return *normalized;
    }
    return std::string(pinyin);
}

} // namespace

LearningStore::LearningStore(std::filesystem::path path) : path_(std::move(path)) {}

void LearningStore::load() {
    const config::LearningLoad loaded = config::loadLearning(path_);
    for (const auto &warning : loaded.warnings) {
        SUPERIOR_WARN(diag::categories::Pinyin)
            .field("file", path_.string())
            .field("warning", warning)
            << "learning file issue";
    }

    entries_.clear();
    entries_.reserve(loaded.entries.size());
    std::unordered_map<std::string, std::size_t> byKey;
    for (const auto &entry : loaded.entries) {
        config::LearnedWord normalized = entry;
        normalized.pinyin = normalizeKey(normalized.pinyin);
        if (normalized.pinyin.empty() || normalized.word.empty()) {
            continue;
        }
        const std::string key = normalized.pinyin + '\x1f' + normalized.word;
        if (const auto found = byKey.find(key); found != byKey.end()) {
            config::LearnedWord &existing = entries_[found->second];
            existing.count += normalized.count;
            existing.lastUsedMs = std::max(existing.lastUsedMs, normalized.lastUsedMs);
        } else {
            byKey.emplace(key, entries_.size());
            entries_.push_back(std::move(normalized));
        }
    }
    dirty_ = false;
}

std::optional<std::string> LearningStore::preferred(std::string_view pinyin) const {
    if (pinyin.empty()) {
        return std::nullopt;
    }
    return config::preferredWord(entries_, normalizeKey(pinyin));
}

void LearningStore::record(std::string_view pinyin, std::string_view word, std::int64_t nowMs) {
    const std::string key = normalizeKey(pinyin);
    if (key.empty() || word.empty()) {
        return;
    }
    config::recordLearnedWord(entries_, key, std::string(word), nowMs);
    dirty_ = true;
}

void LearningStore::saveIfDirty() {
    if (!dirty_) {
        return;
    }
    std::string error;
    if (config::saveLearning(path_, entries_, error)) {
        dirty_ = false;
        return;
    }
    SUPERIOR_WARN(diag::categories::Pinyin)
        .field("file", path_.string())
        .field("error", error)
        .field("entries", static_cast<std::int64_t>(entries_.size()))
        << "cannot save learning file";
}

void LearningStore::clear() {
    entries_.clear();
    dirty_ = true;
}

} // namespace superior::pinyin
