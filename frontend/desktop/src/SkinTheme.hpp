// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QString>

#include <string>

#include "superior/skin/Types.hpp"

namespace superior::ui {

/// Language badge shown next to the caret while the toolbar is hidden.
///
/// It is a separate surface from the candidate panel, so it carries only the
/// values that surface paints. `SkinTheme::from` fills the defaults from the
/// panel style for skins written before the badge existed.
struct IndicatorTheme {
    QColor surface{30, 33, 38, 242};
    QColor text{232, 234, 237};
    QColor border{255, 255, 255, 30};
    int radius = 8;
    int width = 34;
    int height = 34;
    int gap = 6;
    /// Empty means "use the widget's font", which is what a skin written before
    /// the badge existed expects.
    QString family;
    int pixelSize = 16;
    int weight = 500;
    bool italic = false;
};

/// Colours and metrics extracted from a `superior.skin/1` panel style.
///
/// The desktop overlay draws the candidate window itself, so the skin is
/// applied here instead of through Fcitx5's global classicui theme. Only the
/// values the renderer needs are copied; the SkinSpec stays immutable.
struct SkinTheme {
    QColor surface{30, 33, 38, 242};
    QColor text{232, 234, 237};
    QColor firstText{124, 196, 255};
    QColor comment{154, 160, 166};
    QColor selection{42, 111, 219};
    QColor selectionText{255, 255, 255};
    QColor border{255, 255, 255, 30};

    int radius = 10;
    int paddingX = 12;
    int paddingY = 8;
    int cellPaddingX = 8;
    int cellPaddingY = 4;
    int gap = 6;
    bool showSeparator = false;

    IndicatorTheme indicator;

    [[nodiscard]] static SkinTheme from(const skin::SkinSpec &spec, const std::string &styleId);

    /// Built-in fallback used when no skin is installed or the id is unknown.
    [[nodiscard]] static SkinTheme fallback();
};

} // namespace superior::ui
