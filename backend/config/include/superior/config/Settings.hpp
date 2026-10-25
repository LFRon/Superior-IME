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
    /// Show the microphone button in the toolbar. The recogniser itself is a
    /// separate capability: this is only the product-level entry point.
    bool speechRecognitionEnabled = false;
    /// Which catalogue model the capability should load and the panel offers.
    std::string speechRecognitionModelId = "qwen3-asr-0.6b";
    std::string logLevel = "info";

    /// Cloud pinyin. Off by default: enabling it sends what you type to a third
    /// party, so it is an explicit choice, never a surprise.
    bool cloudPinyinEnabled = false;
    /// `googlecn` / `baidu` / `sogou`; see `superior/cloudpinyin/Cloud.hpp`.
    std::string cloudPinyinBackend = "googlecn";
    /// Two characters is the floor: one letter matches far too much to be a
    /// useful candidate. Longer strings are debounced, not skipped.
    int cloudPinyinMinLength = 2;
    /// 1-based position of the cloud candidate; 2 = right after the first.
    int cloudPinyinIndex = 2;
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
