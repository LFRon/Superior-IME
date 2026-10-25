// SPDX-License-Identifier: GPL-3.0-or-later
#include "SkinTheme.hpp"

#include <algorithm>
#include <cmath>

namespace superior::ui {

namespace {

QColor toColor(const skin::Color &color) {
    return QColor(color.r, color.g, color.b, color.a);
}

int toPixels(double value) {
    return static_cast<int>(std::lround(value));
}

double luminance(const QColor &color) {
    return (0.299 * color.red() + 0.587 * color.green() + 0.114 * color.blue()) / 255.0;
}

} // namespace

SkinTheme SkinTheme::fallback() {
    return SkinTheme{};
}

SkinTheme SkinTheme::from(const skin::SkinSpec &spec, const std::string &styleId) {
    SkinTheme theme = fallback();

    auto it = spec.panelStyles.find(styleId);
    if (it == spec.panelStyles.end()) {
        it = spec.panelStyles.find("h1");
    }
    if (it == spec.panelStyles.end() && !spec.panelStyles.empty()) {
        it = spec.panelStyles.begin();
    }
    if (it == spec.panelStyles.end()) {
        return theme;
    }

    const skin::PanelStyle &style = it->second;
    if (style.background.color) {
        theme.surface = toColor(*style.background.color);
    }
    if (style.candidates.text.color) {
        theme.text = toColor(*style.candidates.text.color);
    }
    if (style.candidates.text.firstColor) {
        theme.firstText = toColor(*style.candidates.text.firstColor);
    }
    if (style.candidates.text.commentColor) {
        theme.comment = toColor(*style.candidates.text.commentColor);
        theme.text = theme.text;
    }
    if (style.candidates.selected.color) {
        theme.selection = toColor(*style.candidates.selected.color);
        theme.selectionText =
            luminance(theme.selection) > 0.6 ? QColor(0, 0, 0) : QColor(255, 255, 255);
    }
    if (style.separator.enabled) {
        theme.border = toColor(style.separator.color);
        theme.showSeparator = true;
    } else if (style.background.color) {
        theme.border = QColor(255, 255, 255, 24);
    }

    theme.radius = toPixels(style.background.radius);
    theme.paddingX = toPixels(std::max(style.padding.left, style.padding.right));
    theme.paddingY = toPixels(std::max(style.padding.top, style.padding.bottom));
    theme.cellPaddingX = toPixels(std::max(style.candidates.cell.padding.left,
                                           style.candidates.cell.padding.right));
    theme.cellPaddingY = toPixels(std::max(style.candidates.cell.padding.top,
                                           style.candidates.cell.padding.bottom));
    theme.gap = std::max(0, toPixels(style.candidates.cell.gap));
    return theme;
}

} // namespace superior::ui
