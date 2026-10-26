// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <vector>

namespace superior::paths {

/// Filesystem roots of the running session.
///
/// This is the **only** place below `platforms/` that reads `XDG_*` or `HOME`;
/// `scripts/check-boundaries.sh` fails the build if another capability starts
/// parsing the environment on its own. Two reasons:
///
/// 1. every capability (pinyin, speech, diagnostics, config) needs the same
///    roots, and four copies of the same fallback chain drift apart;
/// 2. on Windows the root set comes from `KnownFolders` instead of the
///    environment, so a port replaces this one function instead of hunting for
///    `getenv` calls.
///
/// A field is empty when the environment offers no answer — the caller decides
/// the fallback, because the right fallback differs. `runtime` has no default at
/// all: `$XDG_RUNTIME_DIR` is a per-session directory, so a caller that cannot
/// live without one falls back to `std::filesystem::temp_directory_path()`
/// itself, where the choice stays visible.
struct Roots {
    std::filesystem::path home;
    std::filesystem::path data;
    std::filesystem::path config;
    std::filesystem::path state;
    std::filesystem::path cache;
    std::filesystem::path runtime;

    /// `XDG_DATA_DIRS`, or `/usr/local/share:/usr/share` when unset.
    std::vector<std::filesystem::path> dataDirs;

    [[nodiscard]] static Roots fromEnvironment();
};

} // namespace superior::paths
