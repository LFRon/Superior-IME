// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace superior::config {

/// Superior-IME's own preferences. This is deliberately separate from Fcitx5's
/// configuration: our settings survive engine changes, and we never guess at
/// Fcitx5 internals to store UI preferences.
struct Settings {
    std::string skinId = "org.superior.default";
    std::string themeName = "org.superior.default";
    bool shiftToggle = true;
    bool showStatusBar = true;
    std::string statusPlacement = "bottomRight";
    double uiScale = 1.0;
    bool voiceEnabled = false;
    std::string logLevel = "info";
};

struct SettingsLoad {
    Settings settings;
    bool existed = false;
    std::vector<std::string> warnings;
};

[[nodiscard]] std::filesystem::path defaultSettingsPath();

[[nodiscard]] SettingsLoad loadSettings(const std::filesystem::path &path);
[[nodiscard]] bool saveSettings(const std::filesystem::path &path, const Settings &settings,
                                std::string &error);

/// Highest-priority skin directory: `<dataHome>/superior-ime/skins`.
[[nodiscard]] std::filesystem::path userSkinDirectory(const std::filesystem::path &dataHome);

/// Skin roots in priority order: the user directory first, then each system
/// data directory (`<datadir>/superior-ime/skins`). Later roots are only used
/// for skins the user has not overridden.
[[nodiscard]] std::vector<std::filesystem::path> skinRoots(const std::filesystem::path &dataHome,
                                                          const std::vector<std::filesystem::path> &dataDirs);

} // namespace superior::config
