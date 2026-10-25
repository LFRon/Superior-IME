// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>

#include "superior/skin/Types.hpp"

/// Projects a `superior.skin/1` package onto Fcitx5's classicui theme format.
///
/// This is pure data transformation: it reads a SkinSpec and writes INI + SVG.
/// It does not include any Fcitx5 header, so both backend tools and the Qt
/// settings frontend may use it.
namespace superior::theme {

/// Exports a `superior.skin/1` package as an fcitx5 classicui theme.
///
/// Why this exists: on Wayland the candidate panel is drawn by Fcitx5's
/// classicui (a third-party UI cannot obtain the input-popup surface through a
/// public API). Exporting our skin as a classicui theme is therefore the
/// supported way to make Wayland candidates look like Superior.
///
/// Assets are emitted as SVG. classicui loads `svg`, `svgz`, `png`, `bmp` and
/// `xpm` (see `theme.cpp:loadImage`), so no rasterizer or Qt is needed here.
struct ThemeExportOptions {
    /// Panel style to project onto classicui. Empty means "h1", then "v1",
    /// then the first style in the skin.
    std::string styleId;

    /// Theme display name. Empty means `SkinSpec::name`.
    std::string displayName;

    /// Overwrite an existing `theme.conf`.
    bool overwrite = false;
};

struct ThemeExportResult {
    bool ok = false;
    std::string detail;
    std::filesystem::path directory;
    std::string styleId;
};

/// Writes `<outputDirectory>/theme.conf` plus generated SVG assets.
///
/// The directory is created if missing. The id is not inferred from the path:
/// callers decide where to place it (typically
/// `~/.local/share/fcitx5/themes/<id>` or `/usr/share/fcitx5/themes/<id>`).
[[nodiscard]] ThemeExportResult exportClassicUiTheme(const skin::SkinSpec &spec,
                                                     const std::filesystem::path &outputDirectory,
                                                     const ThemeExportOptions &options = {});

/// Default theme directory name derived from the skin id.
[[nodiscard]] std::string defaultThemeName(const skin::SkinSpec &spec);

} // namespace superior::theme
