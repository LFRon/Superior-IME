// SPDX-License-Identifier: GPL-3.0-or-later
#include <filesystem>
#include <fstream>
#include <string>

#include "support/TestSupport.hpp"
#include "superior/theme/ClassicUiTheme.hpp"
#include "superior/skin/Skin.hpp"

namespace {

namespace fs = std::filesystem;
using superior::theme::ThemeExportOptions;
using superior::theme::exportClassicUiTheme;

class TempDir {
public:
    TempDir() {
        static int counter = 0;
        root_ = fs::temp_directory_path() /
                ("superior-theme-test-" + std::to_string(++counter));
        std::error_code error;
        fs::remove_all(root_, error);
        fs::create_directories(root_);
    }

    ~TempDir() {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    [[nodiscard]] const fs::path &root() const { return root_; }

private:
    fs::path root_;
};

std::string read(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>()};
}

const superior::skin::SkinSpec &defaultSkin() {
    static const superior::skin::LoadResult result =
        superior::skin::loadSkin(fs::path(SUPERIOR_SOURCE_DIR) / "skins" / "default");
    return *result.spec;
}

void testExportCreatesTheme() {
    TempDir temp;
    const auto target = temp.root() / "org.superior.default";

    const auto result = exportClassicUiTheme(defaultSkin(), target);
    CHECK(result.ok);
    CHECK(result.styleId == "h1");

    CHECK(fs::exists(target / "theme.conf"));
    CHECK(fs::exists(target / "panel.svg"));
    CHECK(fs::exists(target / "highlight.svg"));
    CHECK(fs::exists(target / "next.svg"));
    CHECK(fs::exists(target / "prev.svg"));

    const std::string conf = read(target / "theme.conf");
    CHECK(conf.find("[Metadata]") != std::string::npos);
    CHECK(conf.find("Name=Superior Default") != std::string::npos);
    CHECK(conf.find("[InputPanel]") != std::string::npos);
    CHECK(conf.find("NormalColor=#e8eaed") != std::string::npos);
    CHECK(conf.find("Image=panel.svg") != std::string::npos);
    CHECK(conf.find("[InputPanel/Background/Margin]") != std::string::npos);
    CHECK(conf.find("[InputPanel/Highlight]") != std::string::npos);
    CHECK(conf.find("[Menu/Background]") != std::string::npos);
}

void testRefusesOverwrite() {
    TempDir temp;
    const auto target = temp.root() / "theme";

    CHECK(exportClassicUiTheme(defaultSkin(), target).ok);
    const auto second = exportClassicUiTheme(defaultSkin(), target);
    CHECK(!second.ok);
    CHECK(second.detail.find("already exists") != std::string::npos);

    ThemeExportOptions overwrite;
    overwrite.overwrite = true;
    CHECK(exportClassicUiTheme(defaultSkin(), target, overwrite).ok);
}

void testStyleSelection() {
    TempDir temp;
    ThemeExportOptions options;
    options.styleId = "v1";
    const auto vertical = exportClassicUiTheme(defaultSkin(), temp.root() / "v1", options);
    CHECK(vertical.ok);
    CHECK(vertical.styleId == "v1");

    ThemeExportOptions unknown;
    unknown.styleId = "does-not-exist";
    const auto fallback = exportClassicUiTheme(defaultSkin(), temp.root() / "fallback", unknown);
    CHECK(fallback.ok);
    CHECK(fallback.styleId == "h1");
}

void testSkinWithoutStylesFails() {
    superior::skin::SkinSpec empty;
    empty.id = "org.test.empty";
    TempDir temp;
    const auto result = exportClassicUiTheme(empty, temp.root() / "empty");
    CHECK(!result.ok);
    CHECK(!result.detail.empty());
}

void testDefaultThemeName() {
    CHECK(superior::theme::defaultThemeName(defaultSkin()) == "org.superior.default");

    superior::skin::SkinSpec unnamed;
    unnamed.name = "Demo";
    CHECK(superior::theme::defaultThemeName(unnamed) == "Demo");
}

} // namespace

int main() {
    testExportCreatesTheme();
    testRefusesOverwrite();
    testStyleSelection();
    testSkinWithoutStylesFails();
    testDefaultThemeName();
    return superior::test::finish("theme tests");
}
