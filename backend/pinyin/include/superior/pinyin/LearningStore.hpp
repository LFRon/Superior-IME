// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "superior/config/Learning.hpp"

namespace superior::pinyin {

/// What this user picked for a pinyin, persisted across sessions.
///
/// The store is owned by the runtime and shared by every composer. It is
/// deliberately a *data* object: it knows how to load, normalize, record and
/// save; it does not decide when a pick counts (that is `Composer::select`).
class LearningStore {
public:
    explicit LearningStore(std::filesystem::path path = config::defaultLearningPath());

    void load();

    [[nodiscard]] const std::vector<config::LearnedWord> &entries() const noexcept {
        return entries_;
    }

    [[nodiscard]] std::optional<std::string> preferred(std::string_view pinyin) const;

    /// Records one pick. Existing entries are bumped, not duplicated.
    void record(std::string_view pinyin, std::string_view word, std::int64_t nowMs);

    /// Writes the file only when something changed. Errors are logged, never
    /// thrown: losing a preference must not take the input method down.
    void saveIfDirty();

    void clear();

private:
    std::filesystem::path path_;
    std::vector<config::LearnedWord> entries_;
    bool dirty_ = false;
};

} // namespace superior::pinyin
