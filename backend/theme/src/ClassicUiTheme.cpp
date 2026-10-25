// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/theme/ClassicUiTheme.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>

#include "superior/diagnostics/Log.hpp"
#include "superior/skin/Skin.hpp"

namespace superior::theme {

namespace {

namespace fs = std::filesystem;
namespace diag = ::superior::diagnostics;

std::string hex(const skin::Color &color) {
    char buffer[10] = {};
    std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", static_cast<unsigned>(color.r),
                  static_cast<unsigned>(color.g), static_cast<unsigned>(color.b));
    return std::string(buffer);
}

double luminance(const skin::Color &color) {
    return (0.299 * color.r + 0.587 * color.g + 0.114 * color.b) / 255.0;
}

const skin::Color *backgroundOf(const skin::PanelStyle &style) {
    return style.background.color ? &*style.background.color : nullptr;
}

std::string pickStyle(const skin::SkinSpec &spec, const std::string &requested) {
    if (!requested.empty() && spec.panelStyles.count(requested) != 0) {
        return requested;
    }
    for (const char *candidate : {"h1", "v1", "h2", "v2"}) {
        if (spec.panelStyles.count(candidate) != 0) {
            return candidate;
        }
    }
    if (!spec.panelStyles.empty()) {
        return spec.panelStyles.begin()->first;
    }
    return {};
}

std::string svgRoundedRect(int size, double radius, const std::string &fill, double opacity) {
    char opacityText[32] = {};
    std::snprintf(opacityText, sizeof(opacityText), "%.3f", opacity);
    std::string svg;
    svg += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + std::to_string(size) +
           "\" height=\"" + std::to_string(size) + "\" viewBox=\"0 0 " + std::to_string(size) +
           " " + std::to_string(size) + "\">\n";
    svg += "  <rect x=\"0\" y=\"0\" width=\"" + std::to_string(size) + "\" height=\"" +
           std::to_string(size) + "\" rx=\"" + std::to_string(radius) + "\" ry=\"" +
           std::to_string(radius) + "\" fill=\"" + fill + "\" fill-opacity=\"" + opacityText +
           "\"/>\n";
    svg += "</svg>\n";
    return svg;
}

std::string svgArrow(int size, const std::string &fill, bool pointsRight) {
    const std::string path = pointsRight ? "M6 3 L15 11 L6 19" : "M16 3 L7 11 L16 19";
    std::string svg;
    svg += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + std::to_string(size) +
           "\" height=\"" + std::to_string(size) + "\" viewBox=\"0 0 22 22\">\n";
    svg += "  <path d=\"" + path +
           "\" fill=\"none\" stroke=\"" + fill +
           "\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>\n";
    svg += "</svg>\n";
    return svg;
}

bool writeFile(const fs::path &path, const std::string &content, std::string &error) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "cannot write '" + path.string() + "'";
        return false;
    }
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!stream) {
        error = "short write on '" + path.string() + "'";
        return false;
    }
    return true;
}

std::string marginSection(const char *section, const skin::Insets &insets) {
    std::string out = "[";
    out += section;
    out += "]\nLeft=" + std::to_string(static_cast<int>(std::lround(insets.left)));
    out += "\nRight=" + std::to_string(static_cast<int>(std::lround(insets.right)));
    out += "\nTop=" + std::to_string(static_cast<int>(std::lround(insets.top)));
    out += "\nBottom=" + std::to_string(static_cast<int>(std::lround(insets.bottom)));
    out += "\n\n";
    return out;
}

} // namespace

std::string defaultThemeName(const skin::SkinSpec &spec) {
    if (!spec.id.empty()) {
        return spec.id;
    }
    if (!spec.name.empty()) {
        return spec.name;
    }
    return "superior";
}

ThemeExportResult exportClassicUiTheme(const skin::SkinSpec &spec,
                                       const fs::path &outputDirectory,
                                       const ThemeExportOptions &options) {
    ThemeExportResult result;
    result.directory = outputDirectory;

    const std::string styleId = pickStyle(spec, options.styleId);
    if (styleId.empty()) {
        result.detail = "skin has no panel styles to export";
        return result;
    }
    result.styleId = styleId;
    const skin::PanelStyle &style = spec.panelStyles.at(styleId);

    std::error_code error;
    fs::create_directories(outputDirectory, error);
    if (error) {
        result.detail = "cannot create '" + outputDirectory.string() + "': " + error.message();
        return result;
    }

    const fs::path themeConf = outputDirectory / "theme.conf";
    if (fs::exists(themeConf, error) && !options.overwrite) {
        result.detail = "'" + themeConf.string() + "' already exists (use --overwrite)";
        return result;
    }

    const skin::Color *background = backgroundOf(style);
    const skin::Color surface = background != nullptr ? *background
                                                      : skin::Color{30, 33, 38, 242};
    const skin::Color textColor =
        style.candidates.text.color.value_or(skin::Color{232, 234, 237, 255});
    const skin::Color selection =
        style.candidates.selected.color.value_or(skin::Color{42, 111, 219, 255});
    const skin::Color selectionText =
        luminance(selection) > 0.6 ? skin::Color{0, 0, 0, 255} : skin::Color{255, 255, 255, 255};
    // Only use the separator colour when it is actually enabled; otherwise the
    // default (white) would leak into theme.conf as a misleading border colour.
    const skin::Color border = style.separator.enabled
                                   ? style.separator.color
                                   : (background != nullptr ? *background
                                                            : skin::Color{30, 33, 38, 255});
    const double radius = std::max({style.background.radius, 2.0,
                                    static_cast<double>(style.background.image
                                                            ? style.background.image->slice.left
                                                            : 0)});

    // Assets: classicui accepts SVG, so generate rounded 9-slice sources.
    const int assetSize = 64;
    const std::string panelSvg =
        svgRoundedRect(assetSize, radius, hex(surface), surface.a / 255.0);
    const std::string highlightSvg =
        svgRoundedRect(assetSize, std::max(2.0, radius - 2.0), hex(selection),
                       selection.a / 255.0);
    const std::string arrowSvg = svgArrow(22, hex(textColor), true);
    const std::string arrowBackSvg = svgArrow(22, hex(textColor), false);

    if (!writeFile(outputDirectory / "panel.svg", panelSvg, result.detail) ||
        !writeFile(outputDirectory / "highlight.svg", highlightSvg, result.detail) ||
        !writeFile(outputDirectory / "next.svg", arrowSvg, result.detail) ||
        !writeFile(outputDirectory / "prev.svg", arrowBackSvg, result.detail)) {
        return result;
    }

    const int slice = std::max(1, static_cast<int>(std::lround(radius)));
    const std::string displayName =
        options.displayName.empty() ? spec.name : options.displayName;

    skin::Insets textInsets = style.candidates.cell.padding;
    skin::Insets contentInsets = style.padding;

    std::string conf;
    conf += "[Metadata]\n";
    conf += "Name=" + displayName + "\n";
    conf += "Version=1\n";
    conf += "Author=" + (spec.author.empty() ? std::string("Superior-IME") : spec.author) + "\n";
    conf += "Description=" + (spec.description.empty() ? std::string("Exported from ") + spec.id
                                                       : spec.description) +
            "\n";
    conf += "ScaleWithDPI=True\n\n";

    conf += "[InputPanel]\n";
    conf += "NormalColor=" + hex(textColor) + "\n";
    conf += "HighlightColor=" + hex(selectionText) + "\n";
    conf += "PageButtonAlignment=Last Candidate\n\n";

    conf += marginSection("InputPanel/TextMargin", textInsets);
    conf += marginSection("InputPanel/ContentMargin", contentInsets);

    conf += "[InputPanel/Background]\n";
    conf += "Image=panel.svg\n";
    conf += "Color=" + hex(surface) + "\n";
    conf += "BorderColor=" + hex(border) + "\n";
    conf += "BorderWidth=" + std::to_string(style.separator.enabled ? 1 : 0) + "\n\n";
    conf += marginSection("InputPanel/Background/Margin",
                          skin::Insets{static_cast<double>(slice), static_cast<double>(slice),
                                       static_cast<double>(slice), static_cast<double>(slice)});

    conf += "[InputPanel/Highlight]\n";
    conf += "Image=highlight.svg\n";
    conf += "Color=" + hex(selection) + "\n\n";
    conf += marginSection("InputPanel/Highlight/Margin",
                          skin::Insets{4.0, 4.0, 4.0, 4.0});

    conf += "[InputPanel/PrevPage]\nImage=prev.svg\n\n";
    conf += marginSection("InputPanel/PrevPage/ClickMargin",
                          skin::Insets{4.0, 4.0, 4.0, 4.0});
    conf += "[InputPanel/NextPage]\nImage=next.svg\n\n";
    conf += marginSection("InputPanel/NextPage/ClickMargin",
                          skin::Insets{4.0, 4.0, 4.0, 4.0});

    conf += "[Menu/Background]\n";
    conf += "Image=panel.svg\n";
    conf += "Color=" + hex(surface) + "\n";
    conf += "BorderColor=" + hex(border) + "\n";
    conf += "BorderWidth=" + std::to_string(style.separator.enabled ? 1 : 0) + "\n\n";
    conf += marginSection("Menu/Background/Margin",
                          skin::Insets{static_cast<double>(slice), static_cast<double>(slice),
                                       static_cast<double>(slice), static_cast<double>(slice)});

    if (!writeFile(themeConf, conf, result.detail)) {
        return result;
    }

    result.ok = true;
    SUPERIOR_INFO(diag::categories::Skin)
        .field("skin", spec.id)
        .field("style", styleId)
        .field("dir", outputDirectory.filename().string())
        .field("bytes", static_cast<std::uint64_t>(conf.size()))
        << "classicui theme exported";
    return result;
}

} // namespace superior::theme
