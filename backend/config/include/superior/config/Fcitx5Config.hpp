// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace superior::config {

/// Filesystem locations used to configure Fcitx5.
///
/// This module knows INI syntax and XDG paths, but **no Fcitx5 headers**, so a
/// Qt frontend may link it without depending on the platform adapter.
struct Paths {
    std::filesystem::path configHome;
    std::filesystem::path dataHome;
    std::vector<std::filesystem::path> dataDirs;

    [[nodiscard]] static Paths fromEnvironment();

    [[nodiscard]] std::filesystem::path fcitxConfigDir() const;
    [[nodiscard]] std::filesystem::path classicUiConf() const;
    [[nodiscard]] std::filesystem::path globalConf() const;
    [[nodiscard]] std::filesystem::path userProfile() const;
    [[nodiscard]] std::filesystem::path userThemeDir() const;
};

struct FcitxStatus {
    std::string activeTheme;
    bool darkThemeSelected = false;
    std::vector<std::string> triggerKeys;
    std::vector<std::string> availableThemes;
    bool superiorThemeInstalled = false;
};

[[nodiscard]] FcitxStatus inspect(const Paths &paths, std::string_view superiorThemeName);

struct ApplyOutcome {
    bool ok = false;
    bool changed = false;
    std::string detail;
    std::filesystem::path backup;

    [[nodiscard]] bool healthy() const noexcept { return ok; }
};

/// Selects `themeName` in `classicui.conf` (Theme, DarkTheme, UseDarkTheme).
///
/// A one-time backup `classicui.conf.bak-superior` is written before the first
/// change. Comments and unrelated keys are preserved.
[[nodiscard]] ApplyOutcome setClassicUiTheme(const Paths &paths, std::string_view themeName);

/// Copies an exported theme directory into the user theme directory.
[[nodiscard]] ApplyOutcome installThemeDir(const Paths &paths,
                                           const std::filesystem::path &sourceDirectory,
                                           std::string_view themeName);

} // namespace superior::config
