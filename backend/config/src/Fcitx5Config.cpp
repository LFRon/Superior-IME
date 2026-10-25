// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/config/Fcitx5Config.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "superior/config/IniFile.hpp"
#include "superior/diagnostics/Log.hpp"

namespace superior::config {

namespace {

namespace fs = std::filesystem;
namespace diag = ::superior::diagnostics;

constexpr std::string_view kThemeKey = "Theme";
constexpr std::string_view kDarkThemeKey = "DarkTheme";
constexpr std::string_view kUseDarkThemeKey = "UseDarkTheme";

std::vector<fs::path> splitPaths(const char *raw) {
    std::vector<fs::path> result;
    if (raw == nullptr) {
        return result;
    }
    const std::string text(raw);
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t colon = text.find(':', start);
        const std::size_t end = colon == std::string::npos ? text.size() : colon;
        if (end > start) {
            result.emplace_back(text.substr(start, end - start));
        }
        if (colon == std::string::npos) {
            break;
        }
        start = colon + 1;
    }
    return result;
}

std::string readText(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    return std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

bool writeTextAtomic(const fs::path &path, const std::string &content, std::string &error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "cannot create '" + path.parent_path().string() + "': " + ec.message();
        return false;
    }

    const fs::path temporary = path.string() + ".superior-tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "cannot write '" + temporary.string() + "'";
            return false;
        }
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!stream) {
            error = "short write on '" + temporary.string() + "'";
            return false;
        }
    }
    fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write |
                                   fs::perms::group_read | fs::perms::others_read,
                    fs::perm_options::replace, ec);
    fs::rename(temporary, path, ec);
    if (ec) {
        error = "cannot replace '" + path.string() + "': " + ec.message();
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    return true;
}

bool backupOnce(const fs::path &path, fs::path &backupPath, std::string &error) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return true;
    }
    backupPath = path.string() + ".bak-superior";
    if (fs::exists(backupPath, ec)) {
        return true;
    }
    fs::copy_file(path, backupPath, fs::copy_options::none, ec);
    if (ec) {
        error = "cannot back up '" + path.string() + "': " + ec.message();
        return false;
    }
    return true;
}

} // namespace

Paths Paths::fromEnvironment() {
    Paths paths;
    if (const char *home = std::getenv("XDG_CONFIG_HOME"); home != nullptr && home[0] != '\0') {
        paths.configHome = home;
    } else if (const char *userHome = std::getenv("HOME"); userHome != nullptr) {
        paths.configHome = fs::path(userHome) / ".config";
    }

    if (const char *home = std::getenv("XDG_DATA_HOME"); home != nullptr && home[0] != '\0') {
        paths.dataHome = home;
    } else if (const char *userHome = std::getenv("HOME"); userHome != nullptr) {
        paths.dataHome = fs::path(userHome) / ".local" / "share";
    }

    paths.dataDirs = splitPaths(std::getenv("XDG_DATA_DIRS"));
    if (paths.dataDirs.empty()) {
        paths.dataDirs.emplace_back("/usr/local/share");
        paths.dataDirs.emplace_back("/usr/share");
    }
    return paths;
}

fs::path Paths::fcitxConfigDir() const {
    return configHome / "fcitx5";
}

fs::path Paths::classicUiConf() const {
    return fcitxConfigDir() / "conf" / "classicui.conf";
}

fs::path Paths::globalConf() const {
    return fcitxConfigDir() / "config";
}

fs::path Paths::userProfile() const {
    return fcitxConfigDir() / "profile";
}

fs::path Paths::userThemeDir() const {
    return dataHome / "fcitx5" / "themes";
}

FcitxStatus inspect(const Paths &paths, std::string_view superiorThemeName) {
    FcitxStatus status;

    const IniFile classic = IniFile::parse(readText(paths.classicUiConf()));
    if (const auto theme = classic.get("", kThemeKey)) {
        status.activeTheme = *theme;
    }
    if (const auto dark = classic.get("", kDarkThemeKey)) {
        status.darkThemeSelected = *dark == superiorThemeName;
    }
    if (status.activeTheme.empty()) {
        status.darkThemeSelected = false;
    }

    const IniFile global = IniFile::parse(readText(paths.globalConf()));
    for (const auto &key : global.keys("Hotkey/TriggerKeys")) {
        if (const auto value = global.get("Hotkey/TriggerKeys", key); value && !value->empty()) {
            status.triggerKeys.push_back(*value);
        }
    }

    for (const auto &root : paths.dataDirs) {
        const fs::path themes = root / "fcitx5" / "themes";
        std::error_code ec;
        if (!fs::is_directory(themes, ec)) {
            continue;
        }
        for (const auto &entry : fs::directory_iterator(themes, ec)) {
            if (ec) {
                break;
            }
            if (entry.is_directory()) {
                status.availableThemes.push_back(entry.path().filename().string());
            }
        }
    }
    {
        std::error_code ec;
        const fs::path userThemes = paths.userThemeDir();
        if (fs::is_directory(userThemes, ec)) {
            for (const auto &entry : fs::directory_iterator(userThemes, ec)) {
                if (ec) {
                    break;
                }
                if (entry.is_directory()) {
                    status.availableThemes.push_back(entry.path().filename().string());
                }
            }
        }
    }
    std::sort(status.availableThemes.begin(), status.availableThemes.end());
    status.availableThemes.erase(
        std::unique(status.availableThemes.begin(), status.availableThemes.end()),
        status.availableThemes.end());

    status.superiorThemeInstalled =
        std::find(status.availableThemes.begin(), status.availableThemes.end(),
                  std::string(superiorThemeName)) != status.availableThemes.end();

    return status;
}

ApplyOutcome setClassicUiTheme(const Paths &paths, std::string_view themeName) {
    ApplyOutcome outcome;
    const fs::path file = paths.classicUiConf();

    std::string error;
    if (!backupOnce(file, outcome.backup, error)) {
        outcome.detail = error;
        return outcome;
    }

    IniFile ini = IniFile::parse(readText(file));
    bool changed = false;
    changed = ini.set("", kThemeKey, themeName) || changed;
    changed = ini.set("", kDarkThemeKey, themeName) || changed;
    changed = ini.set("", kUseDarkThemeKey, "True") || changed;

    if (!changed) {
        outcome.ok = true;
        outcome.detail = "theme already selected";
        return outcome;
    }
    if (!writeTextAtomic(file, ini.serialize(), error)) {
        outcome.detail = error;
        return outcome;
    }

    outcome.ok = true;
    outcome.changed = true;
    outcome.detail = "classicui theme set to " + std::string(themeName);
    SUPERIOR_INFO(diag::categories::Skin)
        .field("theme", themeName)
        .field("file", file.string())
        << "classicui theme selected";
    return outcome;
}

ApplyOutcome installThemeDir(const Paths &paths, const fs::path &sourceDirectory,
                             std::string_view themeName) {
    ApplyOutcome outcome;
    std::error_code ec;
    if (!fs::is_directory(sourceDirectory, ec)) {
        outcome.detail = "theme source does not exist: " + sourceDirectory.string();
        return outcome;
    }

    const fs::path target = paths.userThemeDir() / std::string(themeName);
    fs::remove_all(target, ec);
    fs::create_directories(target, ec);
    if (ec) {
        outcome.detail = "cannot create '" + target.string() + "': " + ec.message();
        return outcome;
    }
    fs::copy(sourceDirectory, target, fs::copy_options::recursive, ec);
    if (ec) {
        outcome.detail = "cannot copy theme: " + ec.message();
        return outcome;
    }

    outcome.ok = true;
    outcome.changed = true;
    outcome.detail = "theme installed to " + target.string();
    return outcome;
}

} // namespace superior::config
