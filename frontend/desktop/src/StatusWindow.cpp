// SPDX-License-Identifier: GPL-3.0-or-later
#include "StatusWindow.hpp"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QRectF>
#include <QString>

#include <algorithm>
#include <cmath>

#include "Overlay.hpp"

namespace superior::ui {

namespace {
constexpr int kBaseHeight = 30;
constexpr int kBasePadding = 8;
constexpr int kBaseGap = 6;
constexpr int kBasePointSize = 95; // 9.5 pt in tenths

QString itemText(const api::StatusItem &item) {
    if (!item.label.empty()) {
        return QString::fromStdString(item.label);
    }
    if (!item.icon.empty()) {
        return QString::fromStdString(item.icon);
    }
    return QString::fromStdString(item.id);
}
} // namespace

StatusWindow::StatusWindow(QWidget *parent) : QWidget(parent) {
    makeOverlay(*this);
    setWindowTitle(QStringLiteral("Superior-IME Status"));
    resize(180, kBaseHeight);
}

int StatusWindow::scaled(int value) const {
    return static_cast<int>(std::lround(static_cast<double>(value) * scale_));
}

void StatusWindow::setPlacement(Placement placement) {
    if (placement_ == placement) {
        return;
    }
    placement_ = placement;
    if (isVisible()) {
        placeOverlay(*this, placement_);
    }
}

void StatusWindow::setScale(double scale) {
    const double bounded = clampScale(scale);
    if (std::abs(bounded - scale_) < 0.001) {
        return;
    }
    scale_ = bounded;
    QFont scaledFont = font();
    scaledFont.setPointSizeF(static_cast<double>(kBasePointSize) / 10.0 * scale_);
    setFont(scaledFont);
    if (isVisible()) {
        resize(sizeHint());
        placeOverlay(*this, placement_);
        update();
    }
}

void StatusWindow::setEnabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled_) {
        hide();
        return;
    }
    if (visible_) {
        show();
    }
}

void StatusWindow::setItemClickHandler(std::function<void(const std::string &)> handler) {
    itemClick_ = std::move(handler);
}

void StatusWindow::apply(const api::StatusScene &status, const std::string &mode,
                         const std::string &program, const SkinTheme &theme) {
    theme_ = theme;
    program_ = QString::fromStdString(program);

    // `inline-mode` is our own state item: the agent injects it so the UI has a
    // single source of truth, but it is drawn as the 中/英 badge instead of a
    // separate chip. Rendering it twice would also put two click targets for
    // the same action next to each other.
    items_.clear();
    items_.reserve(status.items.size());
    QString inlineMode = QString::fromStdString(mode);
    for (const auto &item : status.items) {
        if (item.id == "inline-mode") {
            if (!item.label.empty()) {
                inlineMode = QString::fromStdString(item.label);
            }
            continue;
        }
        items_.push_back(item);
    }
    mode_ = inlineMode;

    if (!status.visible || !enabled_) {
        visible_ = status.visible;
        hide();
        return;
    }
    visible_ = true;
    resize(sizeHint());
    placeOverlay(*this, placement_);
    show();
    update();
}

QSize StatusWindow::sizeHint() const {
    const QFontMetrics metrics(font());
    int width = scaled(kBasePadding) + scaled(10) + scaled(kBaseGap);
    if (!mode_.isEmpty()) {
        width += metrics.horizontalAdvance(mode_) + scaled(kBaseGap);
    }
    for (const auto &item : items_) {
        width += std::max(scaled(24), metrics.horizontalAdvance(itemText(item)) + scaled(16)) +
                 scaled(kBaseGap);
    }
    width += scaled(kBasePadding);
    return QSize(std::max(scaled(110), width), scaled(kBaseHeight));
}

void StatusWindow::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QPainterPath background;
    const int radius = std::max(scaled(6), theme_.radius);
    background.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    painter.fillPath(background, theme_.surface);
    painter.setPen(theme_.showSeparator ? theme_.border : QColor(255, 255, 255, 24));
    painter.drawPath(background);

    painter.setFont(font());
    const QFontMetrics metrics(font());

    int x = scaled(kBasePadding);
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme_.firstText);
    painter.drawEllipse(QPointF(x + scaled(4), height() / 2.0), scaled(4), scaled(4));
    x += scaled(10) + scaled(kBaseGap);

    itemRects_.clear();
    modeRect_ = QRect();

    if (!mode_.isEmpty()) {
        const int width = metrics.horizontalAdvance(mode_) + scaled(16);
        modeRect_ = QRect(x, scaled(3), width, height() - scaled(6));
        if (theme_.showSeparator) {
            QColor badge = theme_.surface.lighter(118);
            painter.setPen(Qt::NoPen);
            painter.setBrush(badge);
            painter.drawRoundedRect(modeRect_, scaled(5), scaled(5));
        }
        painter.setPen(theme_.text);
        painter.drawText(modeRect_, Qt::AlignCenter, mode_);
        x += width + scaled(kBaseGap);
    }

    itemRects_.reserve(items_.size());
    for (const auto &item : items_) {
        const int width =
            std::max(scaled(24), metrics.horizontalAdvance(itemText(item)) + scaled(16));
        const QRect cell(x, scaled(3), width, height() - scaled(6));

        QColor textColor = item.enabled ? theme_.text : QColor(120, 124, 130);
        if (item.active) {
            textColor = theme_.firstText;
            QColor highlight = theme_.selection;
            highlight.setAlpha(90);
            painter.setBrush(highlight);
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(cell, scaled(5), scaled(5));
        }

        painter.setPen(textColor);
        painter.drawText(cell, Qt::AlignCenter, itemText(item));
        itemRects_.push_back(cell);
        x += width + scaled(kBaseGap);
    }

    Q_UNUSED(program_);
}

void StatusWindow::mousePressEvent(QMouseEvent *event) {
    // An overlay must never take focus; only the left button is meaningful and
    // every click turns into a semantic command the agent validates.
    if (event->button() != Qt::LeftButton || itemClick_ == nullptr) {
        return;
    }
    if (!modeRect_.isNull() && modeRect_.contains(event->pos())) {
        itemClick_("inline-mode");
        return;
    }
    for (std::size_t index = 0; index < itemRects_.size(); ++index) {
        if (itemRects_[index].contains(event->pos())) {
            itemClick_(items_[index].id);
            return;
        }
    }
}

} // namespace superior::ui
