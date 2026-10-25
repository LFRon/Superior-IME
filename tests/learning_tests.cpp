// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "support/TestSupport.hpp"
#include "superior/config/Learning.hpp"

namespace {

namespace fs = std::filesystem;
using superior::config::LearnedWord;
using superior::config::preferredWord;
using superior::config::recordLearnedWord;

fs::path temporaryDirectory() {
    const fs::path base = fs::temp_directory_path() / "superior-learning-tests";
    std::error_code error;
    fs::remove_all(base, error);
    fs::create_directories(base, error);
    return base;
}

void testFirstRecord() {
    std::vector<LearnedWord> entries;
    recordLearnedWord(entries, "yuanshen", "原神", 1000);
    CHECK(entries.size() == 1);
    CHECK(entries.front().pinyin == "yuanshen");
    CHECK(entries.front().word == "原神");
    CHECK(entries.front().count == 1);
    CHECK(preferredWord(entries, "yuanshen") == std::optional<std::string>("原神"));
}

void testRepeatBumpsCount() {
    std::vector<LearnedWord> entries;
    recordLearnedWord(entries, "yuanshen", "原神", 1000);
    recordLearnedWord(entries, "yuanshen", "原神", 2000);
    CHECK(entries.size() == 1);
    CHECK(entries.front().count == 2);
    CHECK(entries.front().lastUsedMs == 2000);
}

void testDifferentChoiceReplaces() {
    std::vector<LearnedWord> entries;
    recordLearnedWord(entries, "yuan", "元", 1000);
    recordLearnedWord(entries, "yuan", "原", 2000);
    CHECK(entries.size() == 1);
    // The user changed their mind; the old word must not linger as a candidate.
    CHECK(preferredWord(entries, "yuan") == std::optional<std::string>("原"));
    CHECK(entries.front().count == 1);
}

void testUnknownPinyin() {
    std::vector<LearnedWord> entries;
    recordLearnedWord(entries, "yuanshen", "原神", 1000);
    CHECK(!preferredWord(entries, "yuanshen2").has_value());
}

void testCapacityDropsOldest() {
    std::vector<LearnedWord> entries;
    for (int index = 0; index < 3; ++index) {
        recordLearnedWord(entries, "p" + std::to_string(index), "词" + std::to_string(index),
                          100 + index * 10, 3);
    }
    CHECK(entries.size() == 3);
    // The oldest (p0) is evicted, not the newest.
    recordLearnedWord(entries, "p3", "词3", 500, 3);
    CHECK(entries.size() == 3);
    CHECK(!preferredWord(entries, "p0").has_value());
    CHECK(preferredWord(entries, "p3").has_value());
}

void testEmptyInputIgnored() {
    std::vector<LearnedWord> entries;
    recordLearnedWord(entries, "", "原神", 1000);
    recordLearnedWord(entries, "yuanshen", "", 1000);
    CHECK(entries.empty());
}

void testFileIsOwnerOnly() {
    const fs::path directory = temporaryDirectory();
    const fs::path path = directory / "learning.json";
    std::vector<LearnedWord> entries;
    recordLearnedWord(entries, "a", "啊", 1);
    std::string error;
    CHECK(superior::config::saveLearning(path, entries, error));

    // The file records private typing history; `~/.config` is often 0755, so
    // the umask cannot be relied on.
    std::error_code code;
    const auto status = fs::status(path, code);
    CHECK(!code);
    const auto isOwnerOnly = (status.permissions() &
                              (fs::perms::group_read | fs::perms::group_write |
                               fs::perms::others_read | fs::perms::others_write)) ==
                             fs::perms::none;
    CHECK(isOwnerOnly);
}

void testRoundTrip() {
    const fs::path directory = temporaryDirectory();
    const fs::path path = directory / "learning.json";

    std::vector<LearnedWord> entries;
    recordLearnedWord(entries, "yuanshen", "原神", 1234);
    recordLearnedWord(entries, "yuanshen", "原神", 1235);
    recordLearnedWord(entries, "xingqiong", "星穹", 1300);

    std::string error;
    CHECK(superior::config::saveLearning(path, entries, error));
    CHECK(error.empty());

    const auto loaded = superior::config::loadLearning(path);
    CHECK(loaded.existed);
    CHECK(loaded.entries.size() == 2);
    CHECK(preferredWord(loaded.entries, "yuanshen") == std::optional<std::string>("原神"));
    CHECK(preferredWord(loaded.entries, "xingqiong") == std::optional<std::string>("星穹"));
    CHECK(loaded.warnings.empty());
}

void testMissingFile() {
    const fs::path directory = temporaryDirectory();
    const auto loaded = superior::config::loadLearning(directory / "absent.json");
    CHECK(!loaded.existed);
    CHECK(loaded.entries.empty());
    CHECK(loaded.warnings.empty());
}

void testUnknownSchemaIgnored() {
    const fs::path directory = temporaryDirectory();
    const fs::path path = directory / "learning.json";
    {
        std::ofstream stream(path);
        stream << R"({"schema":"superior.learning/99","entries":[{"pinyin":"a","word":"啊"}]})";
    }
    const auto loaded = superior::config::loadLearning(path);
    CHECK(loaded.entries.empty());
    CHECK(!loaded.warnings.empty());
}

void testMalformedIgnored() {
    const fs::path directory = temporaryDirectory();
    const fs::path path = directory / "learning.json";
    {
        std::ofstream stream(path);
        stream << "{oops";
    }
    const auto loaded = superior::config::loadLearning(path);
    CHECK(loaded.entries.empty());
    CHECK(!loaded.warnings.empty());
}

void testClear() {
    const fs::path directory = temporaryDirectory();
    const fs::path path = directory / "learning.json";
    std::vector<LearnedWord> entries;
    recordLearnedWord(entries, "a", "啊", 1);
    std::string error;
    CHECK(superior::config::saveLearning(path, entries, error));
    CHECK(fs::exists(path));
    CHECK(superior::config::clearLearning(path, error));
    CHECK(!fs::exists(path));
    // Clearing twice is not an error: callers do it unconditionally.
    CHECK(superior::config::clearLearning(path, error));
}

void testDefaultPathHonoursXdgConfigHome() {
    const char *previous = std::getenv("XDG_CONFIG_HOME");
    const std::string saved = previous != nullptr ? std::string(previous) : std::string{};
    setenv("XDG_CONFIG_HOME", "/tmp/xdg-learning-test", 1);
    CHECK(superior::config::defaultLearningPath() ==
          fs::path("/tmp/xdg-learning-test/superior-ime/learning.json"));
    if (saved.empty()) {
        unsetenv("XDG_CONFIG_HOME");
    } else {
        setenv("XDG_CONFIG_HOME", saved.c_str(), 1);
    }
}

} // namespace

int main() {
    testFirstRecord();
    testRepeatBumpsCount();
    testDifferentChoiceReplaces();
    testUnknownPinyin();
    testCapacityDropsOldest();
    testEmptyInputIgnored();
    testFileIsOwnerOnly();
    testRoundTrip();
    testMissingFile();
    testUnknownSchemaIgnored();
    testMalformedIgnored();
    testClear();
    testDefaultPathHonoursXdgConfigHome();
    return superior::test::finish("learning tests");
}
