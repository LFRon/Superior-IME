// SPDX-License-Identifier: GPL-3.0-or-later
#include "LanguageIndicator.hpp"

#include <QFont>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRectF>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "Overlay.hpp"

#include "superior/diagnostics/Log.hpp"

namespace superior::ui {

namespace diag = ::superior::diagnostics;

namespace {

/// Below this the glyph is unreadable; above it the box clips it.
constexpr int kMinimumPixelSize = 1;

} // namespace

LanguageIndicator::LanguageIndicator(QWidget *parent) : QWidget(parent) {
    makeOverlay(*this);
    // The badge is feedback, never a click target: an empty input region lets
    // clicks fall through to the application instead of being swallowed by an
    // override-redirect window.
    setWindowFlags(windowFlags() | Qt::WindowTransparentForInput);
    setWindowTitle(QStringLiteral("Superior-IME Language"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    resize(sizeHint());
    hide();
}

int LanguageIndicator::scaled(int value) const {
    return static_cast<int>(std::lround(static_cast<double>(value) * scale_));
}

void LanguageIndicator::setScale(double scale) {
    const double bounded = clampScale(scale);
    if (std::abs(bounded - scale_) < 0.001) {
        return;
    }
    scale_ = bounded;
    if (visible_) {
        resize(sizeHint());
        placeAtCaret(*this, anchor_, scaled(theme_.indicator.gap));
        update();
    }
}

void LanguageIndicator::apply(const api::IndicatorScene &indicator,
                              const SkinTheme &theme) {
    theme_ = theme;
    if (!indicator.visible || indicator.text.empty()) {
        if (visible_) {
            SUPERIOR_DEBUG(diag::categories::Ui) << "language indicator hidden";
        }
        visible_ = false;
        hide();
        return;
    }

    const QString nextText = QString::fromStdString(indicator.text);
    if (!visible_ || nextText != text_) {
        SUPERIOR_DEBUG(diag::categories::Ui)
            .field("text", indicator.text)
            .field("anchorValid", indicator.anchor.valid)
            .field("x", static_cast<std::int64_t>(indicator.anchor.cursor.x))
            .field("y", static_cast<std::int64_t>(indicator.anchor.cursor.y))
            << "language indicator shown";
    }
    text_ = nextText;
    anchor_ = indicator.anchor;
    visible_ = true;
    resize(sizeHint());
    placeAtCaret(*this, anchor_, scaled(theme_.indicator.gap));
    if (!isVisible()) {
        show();
    }
    update();
}

QSize LanguageIndicator::sizeHint() const {
    return QSize(std::max(1, scaled(theme_.indicator.width)),
                 std::max(1, scaled(theme_.indicator.height)));
}

void LanguageIndicator::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const int radius = std::max(0, scaled(theme_.indicator.radius));
    QPainterPath background;
    background.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    painter.fillPath(background, theme_.indicator.surface);
    painter.setPen(QPen(theme_.indicator.border, 1.0));
    painter.drawPath(background);

    QFont badge = font();
    if (!theme_.indicator.family.isEmpty()) {
        badge.setFamily(theme_.indicator.family);
    }
    badge.setPixelSize(std::max(kMinimumPixelSize, scaled(theme_.indicator.pixelSize)));
    badge.setWeight(static_cast<QFont::Weight>(std::clamp(theme_.indicator.weight, 1, 1000)));
    badge.setItalic(theme_.indicator.italic);
    painter.setFont(badge);
    painter.setPen(theme_.indicator.text);
    painter.drawText(rect(), Qt::AlignCenter, text_);
}

} // namespace superior::ui
