// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace superior::config {

/// Where the user last dragged the status bar.
///
/// Deliberately a separate file from `settings.json`: the settings page writes
/// that file, while the overlay writes this one on every drag release. Keeping
/// them apart means a drag never rewrites (and never races with) the user's
/// preferences.
struct StatusBarState {
    bool hasCustomPosition = false;
    int x = 0;
    int y = 0;
    /// Placement that was active when the position was saved. Picking another
    /// corner in the settings page invalidates the saved coordinates instead of
    /// silently ignoring the new choice.
    std::string placement = "bottomRight";
};

struct StatusBarLoad {
    StatusBarState state;
    bool existed = false;
    std::vector<std::string> warnings;
};

/// `$XDG_CONFIG_HOME/superior-ime/statusbar.json`
/// (falls back to `~/.config/superior-ime/statusbar.json`).
[[nodiscard]] std::filesystem::path defaultStatusBarStatePath();

[[nodiscard]] StatusBarLoad loadStatusBarState(const std::filesystem::path &path);
[[nodiscard]] bool saveStatusBarState(const std::filesystem::path &path,
                                      const StatusBarState &state, std::string &error);
/// Removes the file; a missing file is not an error.
[[nodiscard]] bool clearStatusBarState(const std::filesystem::path &path, std::string &error);

} // namespace superior::config
