// SPDX-License-Identifier: GPL-3.0-or-later
#include <filesystem>
#include <fstream>
#include <string>

#include "support/TestSupport.hpp"
#include "superior/config/Fcitx5Config.hpp"
#include "superior/config/IniFile.hpp"
#include "superior/config/Settings.hpp"

namespace {

namespace fs = std::filesystem;
using superior::config::ApplyOutcome;
using superior::config::IniFile;
using superior::config::Paths;

class TempTree {
public:
    TempTree() {
        static int counter = 0;
        root_ = fs::temp_directory_path() / ("superior-config-test-" + std::to_string(++counter));
        std::error_code error;
        fs::remove_all(root_, error);
        fs::create_directories(root_ / "config" / "fcitx5" / "conf");
        fs::create_directories(root_ / "data");
    }

    ~TempTree() {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    TempTree(const TempTree &) = delete;
    TempTree &operator=(const TempTree &) = delete;

    void write(const fs::path &relative, const std::string &text) const {
        const fs::path path = root_ / relative;
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    [[nodiscard]] std::string read(const fs::path &relative) const {
        std::ifstream stream(root_ / relative, std::ios::binary);
        return std::string{std::istreambuf_iterator<char>(stream),
                           std::istreambuf_iterator<char>()};
    }

    [[nodiscard]] Paths paths() const {
        Paths paths;
        paths.configHome = root_ / "config";
        paths.dataHome = root_ / "data";
        paths.dataDirs = {root_ / "data"};
        return paths;
    }

    [[nodiscard]] const fs::path &root() const { return root_; }

private:
    fs::path root_;
};

void testIniSetPreservesComments() {
    IniFile ini = IniFile::parse("# caption\n[InputPanel]\nColor=#ffffff\n\n[Menu]\nX=1\n");
    CHECK(ini.set("InputPanel", "Color", "#000000"));
    CHECK(ini.get("InputPanel", "Color") == std::optional<std::string>("#000000"));

    const std::string text = ini.serialize();
    CHECK(text.find("# caption") != std::string::npos);
    CHECK(text.find("[Menu]") != std::string::npos);
    CHECK(text.find("Color=#000000") != std::string::npos);
}

void testIniAppendsInsideSection() {
    IniFile ini = IniFile::parse("[A]\nOne=1\n\n[B]\nTwo=2\n");
    CHECK(ini.set("A", "Three", "3"));
    const std::string text = ini.serialize();
    const std::size_t three = text.find("Three=3");
    const std::size_t sectionB = text.find("[B]");
    CHECK(three != std::string::npos);
    CHECK(sectionB != std::string::npos);
    CHECK(three < sectionB);
}

void testIniRemoveAndPrefix() {
    IniFile ini = IniFile::parse("[Hotkey/TriggerKeys]\n0=Shift_L\n1=Shift_R\n2=Control+space\n");
    CHECK(ini.removeWithPrefix("Hotkey/TriggerKeys", ""));
    CHECK(ini.keys("Hotkey/TriggerKeys").empty());
    CHECK(ini.set("Hotkey/TriggerKeys", "0", "Control+space"));
    CHECK(ini.get("Hotkey/TriggerKeys", "0") == std::optional<std::string>("Control+space"));
}

void testSetThemeWritesAndBacksUp() {
    TempTree tree;
    tree.write("config/fcitx5/conf/classicui.conf",
               "# keep me\nFont=\"Sans 12\"\nTheme=default\nDarkTheme=default\n");

    const ApplyOutcome outcome = superior::config::setClassicUiTheme(tree.paths(), "org.superior");
    CHECK(outcome.ok);
    CHECK(outcome.changed);
    CHECK(fs::exists(tree.root() / "config/fcitx5/conf/classicui.conf.bak-superior"));

    const std::string text = tree.read("config/fcitx5/conf/classicui.conf");
    CHECK(text.find("Theme=org.superior") != std::string::npos);
    CHECK(text.find("DarkTheme=org.superior") != std::string::npos);
    CHECK(text.find("UseDarkTheme=True") != std::string::npos);
    CHECK(text.find("# keep me") != std::string::npos);
    CHECK(text.find("Font=\"Sans 12\"") != std::string::npos);

    const ApplyOutcome again = superior::config::setClassicUiTheme(tree.paths(), "org.superior");
    CHECK(again.ok);
    CHECK(!again.changed);
}

void testInspectState() {
    TempTree tree;
    tree.write("config/fcitx5/conf/classicui.conf", "Theme=org.superior\n");
    tree.write("config/fcitx5/config", "[Hotkey/TriggerKeys]\n0=Shift_L\n1=Shift_R\n");
    tree.write("data/fcitx5/themes/org.superior/theme.conf", "[Metadata]\nName=x\n");

    const auto status = superior::config::inspect(tree.paths(), "org.superior");
    CHECK(status.activeTheme == "org.superior");
    CHECK(status.superiorThemeInstalled);
    CHECK(status.triggerKeys.size() == 2);
}

void testSettingsRoundTrip() {
    TempTree tree;
    const fs::path file = tree.root() / "config/superior-ime/settings.json";

    superior::config::Settings settings;
    settings.skinId = "org.example.skin";
    settings.themeName = "org.example.skin";
    settings.shiftToggle = false;
    settings.uiScale = 1.25;
    settings.statusPlacement = "topLeft";
    settings.logLevel = "debug";

    std::string error;
    CHECK(superior::config::saveSettings(file, settings, error));
    CHECK(error.empty());

    const auto loaded = superior::config::loadSettings(file);
    CHECK(loaded.existed);
    CHECK(loaded.warnings.empty());
    CHECK(loaded.settings.skinId == settings.skinId);
    CHECK(!loaded.settings.shiftToggle);
    CHECK(superior::test::near(loaded.settings.uiScale, 1.25));
    CHECK(loaded.settings.statusPlacement == "topLeft");
    CHECK(loaded.settings.logLevel == "debug");
}

void testSettingsRejectsBadValues() {
    TempTree tree;
    const fs::path file = tree.root() / "config/superior-ime/settings.json";
    tree.write("config/superior-ime/settings.json",
               R"({"schema":"superior.settings/1","statusPlacement":"middle","uiScale":99,"logLevel":"loud"})");

    const auto loaded = superior::config::loadSettings(file);
    CHECK(loaded.settings.statusPlacement == "bottomRight");
    CHECK(superior::test::near(loaded.settings.uiScale, 3.0));
    CHECK(loaded.settings.logLevel == "info");
    CHECK(loaded.warnings.size() == 2);
}

void testSkinRootsPriority() {
    const auto roots = superior::config::skinRoots("/home/u/.local/share", {"/usr/share"});
    CHECK(roots.size() == 2);
    CHECK(roots.front() == fs::path("/home/u/.local/share/superior-ime/skins"));
    CHECK(roots.back() == fs::path("/usr/share/superior-ime/skins"));
}

/// `[Fuzzy] Inner` is the switch behind "yubianyi -> 御笔安逸": it lets libime
/// split `bian` into `bi'an`. The settings page writes it, so it has to round-trip
/// without disturbing the rest of the engine config.
void testInnerSegmentation() {
    TempTree tree;
    tree.write("config/fcitx5/conf/pinyin.conf",
               "# 拼音\nPageSize=7\n\n[Fuzzy]\n# 内模糊音节\nInner=True\n"
               "InnerShort=True\nPartialFinal=True\n");

    // Unset in a file that has no [Fuzzy] section at all.
    TempTree empty;
    empty.write("config/fcitx5/conf/pinyin.conf", "PageSize=7\n");
    CHECK(!superior::config::readInnerSegmentation(empty.paths()).has_value());

    CHECK(superior::config::readInnerSegmentation(tree.paths()) == std::optional<bool>(true));

    const ApplyOutcome outcome = superior::config::setInnerSegmentation(tree.paths(), false);
    CHECK(outcome.ok);
    CHECK(outcome.changed);
    CHECK(superior::config::readInnerSegmentation(tree.paths()) == std::optional<bool>(false));

    // Comments and unrelated keys must survive: this file belongs to Fcitx5.
    const std::string text = tree.read("config/fcitx5/conf/pinyin.conf");
    CHECK(text.find("# 内模糊音节") != std::string::npos);
    CHECK(text.find("PageSize=7") != std::string::npos);
    CHECK(text.find("InnerShort=True") != std::string::npos);
    CHECK(text.find("Inner=False") != std::string::npos);

    // Writing the same value again is a no-op, not a rewrite.
    const ApplyOutcome again = superior::config::setInnerSegmentation(tree.paths(), false);
    CHECK(again.ok);
    CHECK(!again.changed);
}

} // namespace

int main() {
    testIniSetPreservesComments();
    testIniAppendsInsideSection();
    testIniRemoveAndPrefix();
    testSetThemeWritesAndBacksUp();
    testInspectState();
    testSettingsRoundTrip();
    testSettingsRejectsBadValues();
    testSkinRootsPriority();
    testInnerSegmentation();
    return superior::test::finish("config tests");
}
