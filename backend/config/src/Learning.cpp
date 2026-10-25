// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/config/Learning.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace superior::config {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::string_view kSchema = "superior.learning/1";

} // namespace

fs::path defaultLearningPath() {
    if (const char *configHome = std::getenv("XDG_CONFIG_HOME");
        configHome != nullptr && configHome[0] != '\0') {
        return fs::path(configHome) / "superior-ime" / "learning.json";
    }
    if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return fs::path(home) / ".config" / "superior-ime" / "learning.json";
    }
    return {};
}

LearningLoad loadLearning(const fs::path &path) {
    LearningLoad result;
    std::error_code error;
    if (path.empty() || !fs::is_regular_file(path, error)) {
        return result;
    }
    result.existed = true;

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        result.warnings.emplace_back("cannot open learning file");
        return result;
    }

    Json document;
    try {
        document = Json::parse(std::string{std::istreambuf_iterator<char>(stream),
                                          std::istreambuf_iterator<char>()});
    } catch (const std::exception &exception) {
        result.warnings.emplace_back(std::string("cannot parse learning file: ") + exception.what());
        return result;
    }

    const std::string schema = document.value("schema", std::string{});
    if (schema != kSchema) {
        result.warnings.push_back("unknown learning schema '" + schema + "', ignoring");
        return result;
    }

    const auto entries = document.find("entries");
    if (entries == document.end() || !entries->is_array()) {
        result.warnings.emplace_back("learning file has no entries array");
        return result;
    }

    for (const auto &item : *entries) {
        if (!item.is_object()) {
            continue;
        }
        LearnedWord entry;
        entry.pinyin = item.value("pinyin", std::string{});
        entry.word = item.value("word", std::string{});
        entry.count = std::max(0, item.value("count", 0));
        entry.lastUsedMs = item.value("lastUsedMs", std::int64_t{0});
        if (entry.pinyin.empty() || entry.word.empty()) {
            continue;
        }
        result.entries.push_back(std::move(entry));
    }
    return result;
}

bool saveLearning(const fs::path &path, const std::vector<LearnedWord> &entries,
                  std::string &error) {
    if (path.empty()) {
        error = "no learning path (HOME is unset)";
        return false;
    }

    Json document;
    document["schema"] = std::string(kSchema);
    Json items = Json::array();
    for (const auto &entry : entries) {
        items.push_back(Json{{"pinyin", entry.pinyin},
                             {"word", entry.word},
                             {"count", entry.count},
                             {"lastUsedMs", entry.lastUsedMs}});
    }
    document["entries"] = std::move(items);

    std::error_code code;
    fs::create_directories(path.parent_path(), code);
    if (code) {
        error = "cannot create '" + path.parent_path().string() + "': " + code.message();
        return false;
    }

    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "cannot write '" + temporary.string() + "'";
            return false;
        }
        const std::string text = document.dump(1) + "\n";
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream) {
            error = "short write on '" + temporary.string() + "'";
            return false;
        }
    }
    // This file records what the user typed and picked. Keep it owner-only:
    // `~/.config` is 0755 on many systems, so the umask alone is not enough.
    fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, code);
    if (code) {
        error = "cannot restrict '" + temporary.string() + "': " + code.message();
        return false;
    }

    // Rename, not truncate-in-place: a crash mid-write must not leave a file
    // that fails to parse on the next start.
    fs::rename(temporary, path, code);
    if (code) {
        error = "cannot replace '" + path.string() + "': " + code.message();
        return false;
    }
    return true;
}

bool clearLearning(const fs::path &path, std::string &error) {
    if (path.empty()) {
        return true;
    }
    std::error_code code;
    fs::remove(path, code);
    if (code) {
        error = "cannot remove '" + path.string() + "': " + code.message();
        return false;
    }
    return true;
}

void recordLearnedWord(std::vector<LearnedWord> &entries, std::string pinyin, std::string word,
                       std::int64_t nowMs, std::size_t capacity) {
    if (pinyin.empty() || word.empty()) {
        return;
    }
    const auto existing = std::find_if(entries.begin(), entries.end(),
                                       [&pinyin](const LearnedWord &entry) {
                                           return entry.pinyin == pinyin;
                                       });
    if (existing != entries.end()) {
        if (existing->word == word) {
            ++existing->count;
        } else {
            // A different choice for the same pinyin replaces the old one: the
            // user changed their mind, and keeping both would mean picking one
            // arbitrarily later.
            existing->word = std::move(word);
            existing->count = 1;
        }
        existing->lastUsedMs = nowMs;
        return;
    }

    entries.push_back(LearnedWord{std::move(pinyin), std::move(word), 1, nowMs});
    if (entries.size() <= capacity) {
        return;
    }
    // Least recently used goes first; iterating to the minimum avoids sorting a
    // container that is almost always at its cap.
    const auto oldest = std::min_element(entries.begin(), entries.end(),
                                         [](const LearnedWord &lhs, const LearnedWord &rhs) {
                                             return lhs.lastUsedMs < rhs.lastUsedMs;
                                         });
    if (oldest != entries.end()) {
        entries.erase(oldest);
    }
}

std::optional<std::string> preferredWord(const std::vector<LearnedWord> &entries,
                                         std::string_view pinyin) {
    const LearnedWord *best = nullptr;
    for (const auto &entry : entries) {
        if (entry.pinyin != pinyin) {
            continue;
        }
        if (best == nullptr || entry.count > best->count ||
            (entry.count == best->count && entry.lastUsedMs > best->lastUsedMs)) {
            best = &entry;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    return best->word;
}

} // namespace superior::config
