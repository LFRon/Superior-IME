// SPDX-License-Identifier: GPL-3.0-or-later
#include <filesystem>
#include <fstream>
#include <string>

#include "support/TestSupport.hpp"

namespace {

namespace fs = std::filesystem;

std::string readFile(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>()};
}

const fs::path &sourceRoot() {
    static const fs::path root = SUPERIOR_SOURCE_DIR;
    return root;
}

std::string entry() {
    static const std::string text =
        readFile(sourceRoot() / "backend/platforms/fcitx5/data/super-ime.conf.in");
    return text;
}

bool has(const std::string &haystack, const std::string &needle) {
    return haystack.find(needle) != std::string::npos;
}

void testEntryExists() {
    const auto path = sourceRoot() / "backend/platforms/fcitx5/data/super-ime.conf.in";
    CHECK(fs::is_regular_file(path));
    CHECK(!entry().empty());
}

void testEntryNameMatchesRequest() {
    const std::string text = entry();
    // Display name uses full-width exclamation marks.
    CHECK(has(text, "Name=先！进！输入法"));
    CHECK(has(text, "Name[zh_CN]=先！进！输入法"));
    CHECK(has(text, "Name[zh_TW]=先！進！輸入法"));
    // Half-width variants must not linger.
    CHECK(!has(text, "先!进!"));
    CHECK(!has(text, "先!進!"));
}

void testEntryBindsToTheBundledEngine() {
    const std::string text = entry();
    CHECK(has(text, "[InputMethod]"));
    CHECK(has(text, "Addon=superiorrime"));
    CHECK(has(text, "LangCode=zh_CN"));
    CHECK(has(text, "Configurable=False"));
    // The old entry was removed with the libime engine; a stray binding would
    // silently register a second input method.
    CHECK(!has(text, "Addon=pinyin"));
}

void testEntryIdentityAndIcon() {
    const std::string text = entry();
    // The unique name is the file stem, so no pinyin collision is possible.
    CHECK(has(text, "Label=进"));
    CHECK(has(text, "Icon=org.superior.ime"));
    CHECK(!has(text, "\nName=pinyin\n"));
}

void testIconMatchesEntry() {
    const auto icon =
        sourceRoot() / "backend/platforms/fcitx5/data/icons/org.superior.ime.svg";
    CHECK(fs::is_regular_file(icon));
    const std::string svg = readFile(icon);
    CHECK(has(svg, "<svg"));
    CHECK(has(svg, "</svg>"));
}

void testNoDuplicateEntry() {
    // Exactly one input method entry may be installed. The Rime-specific one was
    // folded into super-ime.conf, and the very old path-based file never existed.
    const fs::path data = sourceRoot() / "backend/platforms/fcitx5/data";
    CHECK(!fs::exists(data / "super-ime-rime.conf.in"));
    CHECK(!fs::exists(data / "inputmethod/superior.conf.in"));
}

} // namespace

int main() {
    testEntryExists();
    testEntryNameMatchesRequest();
    testEntryBindsToTheBundledEngine();
    testEntryIdentityAndIcon();
    testIconMatchesEntry();
    testNoDuplicateEntry();
    return superior::test::finish("input method entry tests");
}
