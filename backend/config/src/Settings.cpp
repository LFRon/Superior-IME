// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/config/Settings.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace superior::config {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::string_view kSchema = "superior.settings/1";

std::string readText(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    return std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

bool validPlacement(std::string_view value) {
    return value == "bottomRight" || value == "bottomLeft" || value == "topRight" ||
           value == "topLeft";
}

} // namespace

fs::path defaultSettingsPath() {
    if (const char *home = std::getenv("XDG_CONFIG_HOME"); home != nullptr && home[0] != '\0') {
        return fs::path(home) / "superior-ime" / "settings.json";
    }
    if (const char *userHome = std::getenv("HOME"); userHome != nullptr && userHome[0] != '\0') {
        return fs::path(userHome) / ".config" / "superior-ime" / "settings.json";
    }
    return {};
}

SettingsLoad loadSettings(const fs::path &path) {
    SettingsLoad result;
    std::error_code ec;
    if (path.empty() || !fs::is_regular_file(path, ec)) {
        return result;
    }
    result.existed = true;

    const std::string text = readText(path);
    Json document;
    try {
        document = Json::parse(text);
    } catch (const std::exception &error) {
        result.warnings.emplace_back(std::string("cannot parse settings: ") + error.what());
        return result;
    }

    const std::string schema = document.value("schema", std::string{});
    if (schema != kSchema) {
        result.warnings.push_back("unknown settings schema '" + schema + "', using defaults");
        return result;
    }

    Settings &settings = result.settings;
    settings.skinId = document.value("skinId", settings.skinId);
    settings.themeName = document.value("themeName", settings.themeName);
    settings.shiftToggle = document.value("shiftToggle", settings.shiftToggle);
    settings.showStatusBar = document.value("showStatusBar", settings.showStatusBar);
    settings.statusPlacement = document.value("statusPlacement", settings.statusPlacement);
    settings.uiScale = document.value("uiScale", settings.uiScale);
    settings.voiceEnabled = document.value("voiceEnabled", settings.voiceEnabled);
    settings.logLevel = document.value("logLevel", settings.logLevel);
    settings.cloudPinyinEnabled = document.value("cloudPinyinEnabled", settings.cloudPinyinEnabled);
    settings.cloudPinyinBackend =
        document.value("cloudPinyinBackend", settings.cloudPinyinBackend);
    settings.cloudPinyinMinLength =
        document.value("cloudPinyinMinLength", settings.cloudPinyinMinLength);
    settings.cloudPinyinIndex = document.value("cloudPinyinIndex", settings.cloudPinyinIndex);

    if (!validPlacement(settings.statusPlacement)) {
        result.warnings.push_back("invalid statusPlacement, falling back to bottomRight");
        settings.statusPlacement = "bottomRight";
    }
    settings.uiScale = std::clamp(settings.uiScale, 0.75, 3.0);
    // Clamp instead of rejecting: a bad number here is a nuisance, not a reason
    // to throw away the rest of the file.
    settings.cloudPinyinMinLength = std::clamp(settings.cloudPinyinMinLength, 1, 32);
    settings.cloudPinyinIndex = std::clamp(settings.cloudPinyinIndex, 1, 9);
    if (settings.cloudPinyinBackend != "googlecn" && settings.cloudPinyinBackend != "baidu" &&
        settings.cloudPinyinBackend != "sogou") {
        result.warnings.push_back("invalid cloudPinyinBackend, falling back to googlecn");
        settings.cloudPinyinBackend = "googlecn";
    }
    static const std::vector<std::string> kLevels{"trace", "debug", "info", "warning", "error",
                                                  "off"};
    if (std::find(kLevels.begin(), kLevels.end(), settings.logLevel) == kLevels.end()) {
        result.warnings.push_back("invalid logLevel, falling back to info");
        settings.logLevel = "info";
    }
    return result;
}

bool saveSettings(const fs::path &path, const Settings &settings, std::string &error) {
    if (path.empty()) {
        error = "no settings path (HOME is unset)";
        return false;
    }

    Json document;
    document["schema"] = std::string(kSchema);
    document["skinId"] = settings.skinId;
    document["themeName"] = settings.themeName;
    document["shiftToggle"] = settings.shiftToggle;
    document["showStatusBar"] = settings.showStatusBar;
    document["statusPlacement"] = settings.statusPlacement;
    document["uiScale"] = settings.uiScale;
    document["voiceEnabled"] = settings.voiceEnabled;
    document["logLevel"] = settings.logLevel;
    document["cloudPinyinEnabled"] = settings.cloudPinyinEnabled;
    document["cloudPinyinBackend"] = settings.cloudPinyinBackend;
    document["cloudPinyinMinLength"] = settings.cloudPinyinMinLength;
    document["cloudPinyinIndex"] = settings.cloudPinyinIndex;

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "cannot create '" + path.parent_path().string() + "': " + ec.message();
        return false;
    }

    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "cannot write '" + temporary.string() + "'";
            return false;
        }
        const std::string text = document.dump(2) + "\n";
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream) {
            error = "short write on '" + temporary.string() + "'";
            return false;
        }
    }
    fs::rename(temporary, path, ec);
    if (ec) {
        error = "cannot replace '" + path.string() + "': " + ec.message();
        return false;
    }
    return true;
}

fs::path userSkinDirectory(const fs::path &dataHome) {
    return dataHome / "superior-ime" / "skins";
}

std::vector<fs::path> skinRoots(const fs::path &dataHome,
                                const std::vector<fs::path> &dataDirs) {
    std::vector<fs::path> roots;
    roots.push_back(userSkinDirectory(dataHome));
    for (const auto &root : dataDirs) {
        roots.push_back(root / "superior-ime" / "skins");
    }
    return roots;
}

} // namespace superior::config
