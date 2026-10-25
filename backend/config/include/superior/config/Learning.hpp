// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace superior::config {

/// One word the user actually picked for a given pinyin.
struct LearnedWord {
    std::string pinyin;
    std::string word;
    /// How many times it was picked. Ties are broken by recency.
    int count = 0;
    std::int64_t lastUsedMs = 0;
};

struct LearningLoad {
    std::vector<LearnedWord> entries;
    bool existed = false;
    std::vector<std::string> warnings;
};

/// `$XDG_CONFIG_HOME/superior-ime/learning.json`
/// (falls back to `~/.config/superior-ime/learning.json`).
///
/// Deliberately in the **config** directory, not next to Fcitx5's own data:
/// this file is our record of what the user chose from *our* cloud candidates.
/// The engine's own learning (`~/.local/share/fcitx5/pinyin/user.dict`) belongs
/// to Fcitx5 and is never touched.
[[nodiscard]] std::filesystem::path defaultLearningPath();

[[nodiscard]] LearningLoad loadLearning(const std::filesystem::path &path);
[[nodiscard]] bool saveLearning(const std::filesystem::path &path,
                                const std::vector<LearnedWord> &entries, std::string &error);
[[nodiscard]] bool clearLearning(const std::filesystem::path &path, std::string &error);

/// Number of entries kept. Enough for a person's real vocabulary of one-off
/// choices, small enough that the file stays trivially readable.
inline constexpr std::size_t kLearningCapacity = 512;

/// Records a pick. Existing entries are bumped, not duplicated; when the list is
/// full the least recently used entry is dropped.
void recordLearnedWord(std::vector<LearnedWord> &entries, std::string pinyin, std::string word,
                       std::int64_t nowMs, std::size_t capacity = kLearningCapacity);

/// The word this user prefers for `pinyin`, if any.
[[nodiscard]] std::optional<std::string> preferredWord(
    const std::vector<LearnedWord> &entries, std::string_view pinyin);

} // namespace superior::config
