// SPDX-License-Identifier: GPL-3.0-or-later
#include <filesystem>
#include <string>
#include <vector>

#include "support/TestSupport.hpp"
#include "superior/config/Learning.hpp"
#include "superior/pinyin/LearningStore.hpp"

namespace {

namespace fs = std::filesystem;
using superior::config::LearnedWord;
using superior::pinyin::LearningStore;

fs::path freshPath() {
    const fs::path dir = fs::temp_directory_path() / "superior-learning-store-tests";
    std::error_code error;
    fs::remove_all(dir, error);
    fs::create_directories(dir, error);
    return dir / "learning.json";
}

const LearnedWord *find(const std::vector<LearnedWord> &entries, const std::string &pinyin,
                        const std::string &word) {
    for (const auto &entry : entries) {
        if (entry.pinyin == pinyin && entry.word == word) {
            return &entry;
        }
    }
    return nullptr;
}

void testRecordNormalizeAndReload() {
    const fs::path path = freshPath();
    LearningStore store(path);
    store.load();
    CHECK(store.entries().empty());
    CHECK(!store.preferred("wo"));

    store.record("xiu fu", "秀夫", 1000);
    store.record("xiu fu", "秀夫", 2000);
    store.record("wo", "握", 3000);
    store.saveIfDirty();

    LearningStore reloaded(path);
    reloaded.load();
    CHECK(reloaded.entries().size() == 2);
    CHECK(reloaded.preferred("xiufu") == std::optional<std::string>("秀夫"));
    CHECK(reloaded.preferred("xiu fu") == std::optional<std::string>("秀夫"));
    CHECK(reloaded.preferred("wo") == std::optional<std::string>("握"));

    const LearnedWord *word = find(reloaded.entries(), "xiufu", "秀夫");
    CHECK(word != nullptr);
    if (word != nullptr) {
        CHECK(word->count == 2);
    }
}

void testLegacySpacesAreNormalizedOnLoad() {
    const fs::path path = freshPath();
    std::vector<LearnedWord> entries;
    LearnedWord legacy;
    legacy.pinyin = "xiu fu";
    legacy.word = "秀夫";
    legacy.count = 3;
    legacy.lastUsedMs = 10;
    entries.push_back(legacy);
    std::string error;
    CHECK(superior::config::saveLearning(path, entries, error));

    LearningStore store(path);
    store.load();
    CHECK(store.entries().size() == 1);
    CHECK(store.entries().front().pinyin == "xiufu");
    CHECK(store.preferred("xiufu") == std::optional<std::string>("秀夫"));
}

void testClearRemovesEverything() {
    const fs::path path = freshPath();
    LearningStore store(path);
    store.load();
    store.record("wo", "握", 1000);
    store.saveIfDirty();
    store.clear();
    store.saveIfDirty();

    LearningStore reloaded(path);
    reloaded.load();
    CHECK(reloaded.entries().empty());
    CHECK(!reloaded.preferred("wo"));
}

} // namespace

int main() {
    testRecordNormalizeAndReload();
    testLegacySpacesAreNormalizedOnLoad();
    testClearRemovesEverything();
    return superior::test::finish("learning-store");
}
