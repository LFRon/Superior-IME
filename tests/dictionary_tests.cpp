// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "support/TestSupport.hpp"
#include "superior/fcitx5/Dictionary.hpp"

namespace {

namespace fs = std::filesystem;
using superior::fcitx5::ImportError;
using superior::fcitx5::ImportOptions;
using superior::fcitx5::ImportResult;

class EnvGuard {
public:
    EnvGuard(const char *name, const std::string &value) : name_(name) {
        if (const char *previous = std::getenv(name)) {
            previous_ = previous;
            hadPrevious_ = true;
        }
        ::setenv(name, value.c_str(), 1);
    }

    ~EnvGuard() {
        if (hadPrevious_) {
            ::setenv(name_, previous_.c_str(), 1);
        } else {
            ::unsetenv(name_);
        }
    }

    EnvGuard(const EnvGuard &) = delete;
    EnvGuard &operator=(const EnvGuard &) = delete;

private:
    const char *name_;
    std::string previous_;
    bool hadPrevious_ = false;
};

class TempTree {
public:
    TempTree() {
        static int counter = 0;
        root_ = fs::temp_directory_path() /
                ("superior-dict-test-" + std::to_string(++counter));
        std::error_code error;
        fs::remove_all(root_, error);
        fs::create_directories(root_);
    }

    ~TempTree() {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    TempTree(const TempTree &) = delete;
    TempTree &operator=(const TempTree &) = delete;

    void write(const fs::path &relative, std::string_view content) const {
        const fs::path path = root_ / relative;
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    void touch(const fs::path &relative) const { write(relative, "x"); }

    [[nodiscard]] const fs::path &root() const { return root_; }

private:
    fs::path root_;
};

void testLayoutFromEnvironment() {
    TempTree dataDir;
    dataDir.touch("libime/sc.dict");
    dataDir.touch("libime/extb.dict");
    dataDir.touch("fcitx5/pinyin/chaizi.dict");

    TempTree userDir;
    userDir.touch("fcitx5/pinyin/user.dict");
    userDir.touch("fcitx5/pinyin/user.history");
    userDir.touch("fcitx5/pinyin/dictionaries/extra.dict");
    userDir.touch("fcitx5/pinyin/dictionaries/off.dict.disable");

    EnvGuard dataDirs("XDG_DATA_DIRS", dataDir.root().string());
    EnvGuard dataHome("XDG_DATA_HOME", userDir.root().string());

    const auto layout = superior::fcitx5::detectDictionaryLayout();
    CHECK(layout.hasSystemDict());
    CHECK(layout.systemDict == dataDir.root() / "libime/sc.dict");
    CHECK(layout.extbDict == dataDir.root() / "libime/extb.dict");
    CHECK(layout.chaiziDict == dataDir.root() / "fcitx5/pinyin/chaizi.dict");
    CHECK(layout.userDict == userDir.root() / "fcitx5/pinyin/user.dict");
    CHECK(layout.userHistory == userDir.root() / "fcitx5/pinyin/user.history");
    CHECK(layout.extraDictionaries.size() == 1);
    CHECK(layout.disabledDictionaries.size() == 1);
    CHECK(layout.usable());
}

void testLayoutWithoutData() {
    TempTree empty;
    EnvGuard dataDirs("XDG_DATA_DIRS", empty.root().string());
    EnvGuard dataHome("XDG_DATA_HOME", empty.root().string());

    const auto layout = superior::fcitx5::detectDictionaryLayout();
    CHECK(!layout.hasSystemDict());
    CHECK(!layout.usable());
    CHECK(layout.extraDictionaries.empty());
}

void testDescribeLayout() {
    TempTree dataDir;
    dataDir.touch("libime/sc.dict");
    EnvGuard dataDirs("XDG_DATA_DIRS", dataDir.root().string());
    EnvGuard dataHome("XDG_DATA_HOME", dataDir.root().string());

    const auto layout = superior::fcitx5::detectDictionaryLayout();
    const std::string text = superior::fcitx5::describeDictionaryLayout(layout);
    CHECK(text.find("system dict") != std::string::npos);
    CHECK(text.find("language model") != std::string::npos);
    CHECK(text.find("extra dicts") != std::string::npos);
}

void testDefaultTarget() {
    superior::fcitx5::DictionaryLayout layout;
    layout.userDictDirectory = "/tmp/superior-dicts";

    const auto target = superior::fcitx5::defaultImportTarget("/tmp/网络流行.scel", layout);
    CHECK(target == fs::path("/tmp/superior-dicts/网络流行.dict"));

    superior::fcitx5::DictionaryLayout empty;
    CHECK(superior::fcitx5::defaultImportTarget("/tmp/a.scel", empty).empty());
}

void testImportRejectsMissingInput() {
    const ImportResult result =
        superior::fcitx5::importSogouScel("/nonexistent/path/file.scel");
    CHECK(!result.ok());
    CHECK(result.error == ImportError::InputMissing);
}

void testImportRejectsDirectory() {
    TempTree tree;
    const ImportResult result = superior::fcitx5::importSogouScel(tree.root());
    CHECK(!result.ok());
    CHECK(result.error == ImportError::InputNotRegular);
}

void testImportRejectsOversizedInput() {
    TempTree tree;
    tree.write("big.scel", std::string(2048, 'x'));

    ImportOptions options;
    options.maxInputBytes = 1024;
    options.targetDirectory = tree.root() / "out";

    const ImportResult result = superior::fcitx5::importSogouScel(tree.root() / "big.scel", options);
    CHECK(!result.ok());
    CHECK(result.error == ImportError::InputTooLarge);
}

void testImportWithoutConverter() {
    TempTree tree;
    tree.write("sample.scel", "not a real cell dictionary");
    tree.touch("pinyin/dictionaries/.keep");

    EnvGuard path("PATH", "/nonexistent-superior-test-bin");

    ImportOptions options;
    options.targetDirectory = tree.root() / "pinyin" / "dictionaries";

    const ImportResult result =
        superior::fcitx5::importSogouScel(tree.root() / "sample.scel", options);
    CHECK(!result.ok());
    CHECK(result.error == ImportError::ToolMissing);
    CHECK(result.detail.find("scel2org") != std::string::npos);
}

void testFindConverterWithEmptyPath() {
    EnvGuard path("PATH", "/nonexistent-superior-test-bin");
    CHECK(!superior::fcitx5::findScelConverter().has_value());
}

} // namespace

int main() {
    testLayoutFromEnvironment();
    testLayoutWithoutData();
    testDescribeLayout();
    testDefaultTarget();
    testImportRejectsMissingInput();
    testImportRejectsDirectory();
    testImportRejectsOversizedInput();
    testImportWithoutConverter();
    testFindConverterWithEmptyPath();
    return superior::test::finish("dictionary tests");
}
