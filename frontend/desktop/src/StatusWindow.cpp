// SPDX-License-Identifier: GPL-3.0-or-later
#include "StatusWindow.hpp"

#include <QEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPoint>
#include <QScreen>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QRectF>
#include <QString>

#include <algorithm>
#include <cmath>

#include "DragHandle.hpp"
#include "StatusGlyph.hpp"
#include "Overlay.hpp"

namespace superior::ui {

namespace {
// Base metrics are the 100 % size: the desktop's scale factor multiplies them
// (`platform/ScreenScale`), and the user's `uiScale` multiplies them again. They
// are sized next to the system's own bars, not as small as they used to be: at
// 30 px / 9.5 pt the toolbar was visibly smaller than every other panel on a
// 125 % desktop.
constexpr int kBaseHeight = 36;
constexpr int kBasePadding = 9;
constexpr int kBaseGap = 7;
constexpr int kBasePointSize = 105; // 10.5 pt in tenths

QString itemText(const api::StatusItem &item) {
    if (!item.label.empty()) {
        return QString::fromStdString(item.label);
    }
    if (!item.icon.empty()) {
        return QString::fromStdString(item.icon);
    }
    return QString::fromStdString(item.id);
}

/// A glyph cell is square-ish and fixed: the moon/circle must not resize the bar
/// when the punctuation mode flips.
constexpr int kGlyphCellWidth = 26;
int itemWidth(const QFontMetrics &metrics, const api::StatusItem &item, double scale) {
    if (glyphFor(item) != Glyph::None) {
        return static_cast<int>(std::lround(static_cast<double>(kGlyphCellWidth) * scale));
    }
    return std::max(28, metrics.horizontalAdvance(itemText(item)) + 18);
}

/// Paints the punctuation glyph: crescent for half width, filled circle for
/// full width.
void paintGlyph(QPainter &painter, Glyph glyph, const QRect &cell, const QColor &color,
                const QColor &background, double scale) {
    painter.save();
    painter.setPen(Qt::NoPen);

    const double radius = 5.0 * scale;
    const QPointF center(cell.center().x() + 0.5, cell.center().y() + 0.5);
    const QRectF outer(center.x() - radius, center.y() - radius, radius * 2.0, radius * 2.0);

    if (glyph == Glyph::Microphone) {
        // Capsule + handle + stem, filled with the item colour so the toolbar
        // matches the panel's own microphone.
        const double scaleUnit = 0.9 * scale;
        QRectF body(center.x() - 2.6 * scaleUnit, center.y() - 6.4 * scaleUnit,
                    5.2 * scaleUnit, 8.6 * scaleUnit);
        painter.setBrush(color);
        painter.drawRoundedRect(body, 2.6 * scaleUnit, 2.6 * scaleUnit);
        painter.setBrush(Qt::NoBrush);
        QPen pen(color, 1.4 * scaleUnit, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(pen);
        painter.drawArc(QRectF(center.x() - 3.8 * scaleUnit, center.y() - 1.6 * scaleUnit,
                               7.6 * scaleUnit, 6.0 * scaleUnit),
                        200 * 16, 140 * 16);
        painter.drawLine(QPointF(center.x(), center.y() + 4.4 * scaleUnit),
                         QPointF(center.x(), center.y() + 6.0 * scaleUnit));
        painter.drawLine(QPointF(center.x() - 1.8 * scaleUnit, center.y() + 6.0 * scaleUnit),
                         QPointF(center.x() + 1.8 * scaleUnit, center.y() + 6.0 * scaleUnit));
        painter.restore();
        return;
    }

    painter.setBrush(color);
    painter.drawEllipse(outer);
    if (glyph == Glyph::Moon) {
        // Crescent = circle minus an offset circle. The bite is painted in the
        // bar's own colour instead of clearing to transparent: clearing would
        // punch a hole through the skin background and show the window behind.
        //
        // Only the offset is rotated, not the painter: a circle minus a circle
        // is rotationally symmetric, so rotating the direction is the same
        // shape with less bookkeeping.
        const MoonBite bite = moonBiteDirection(kMoonRotationDegrees);
        const double distance = radius * 0.62;
        painter.setBrush(background);
        painter.drawEllipse(outer.translated(bite.x * distance, bite.y * distance));
    }

    painter.restore();
}

} // namespace

StatusWindow::StatusWindow(QWidget *parent) : QWidget(parent) {
    makeOverlay(*this);
    setWindowTitle(QStringLiteral("Superior-IME Status"));
    // Needed for the hand cursor on the grip: without tracking we get no move
    // events while hovering.
    setMouseTracking(true);
    resize(200, kBaseHeight);
}

int StatusWindow::scaled(int value) const {
    return static_cast<int>(std::lround(static_cast<double>(value) * scale_));
}

void StatusWindow::setPlacement(Placement placement) {
    if (placement_ == placement) {
        return;
    }
    placement_ = placement;
    if (isVisible() && !customPosition_) {
        placeOverlay(*this, placement_);
    }
}

void StatusWindow::setSavedPosition(std::optional<Point> position) {
    customPosition_ = position;
    if (isVisible()) {
        reposition();
    }
}

void StatusWindow::setPositionChangeHandler(std::function<void(int, int)> handler) {
    positionChange_ = std::move(handler);
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
        reposition();
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
    if (!dragging_) {
        // A scene arrives on every keystroke; while the user is dragging, the
        // window must follow the mouse instead of snapping back.
        reposition();
    }
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
        width += scaled(itemWidth(metrics, item, scale_)) + scaled(kBaseGap);
    }
    width += scaled(kBasePadding);
    return QSize(std::max(scaled(126), width), scaled(kBaseHeight));
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

    // Hover/drag feedback on the grip only: it is the one place where dragging
    // works, so it has to look like a handle.
    if (handleHover_ || dragging_) {
        QColor highlight = theme_.text;
        highlight.setAlpha(30);
        painter.setPen(Qt::NoPen);
        painter.setBrush(highlight);
        painter.drawRoundedRect(
            QRectF(1.5, 1.5, scaled(kDragHandleWidth) - 3.0, height() - 3.0), scaled(5),
            scaled(5));
    }

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
        const int width = scaled(itemWidth(metrics, item, scale_));
        const QRect cell(x, scaled(3), width, height() - scaled(6));
        const Glyph glyph = glyphFor(item);

        QColor textColor = item.enabled ? theme_.text : QColor(120, 124, 130);
        // A glyph item carries its state in the shape, so it must not also get
        // the "active" chip behind it — the moon and the circle are the state.
        if (item.active && glyph == Glyph::None) {
            textColor = theme_.firstText;
            QColor highlight = theme_.selection;
            highlight.setAlpha(90);
            painter.setBrush(highlight);
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(cell, scaled(5), scaled(5));
        }

        if (glyph != Glyph::None) {
            paintGlyph(painter, glyph, cell, textColor, theme_.surface, scale_);
        } else {
            painter.setPen(textColor);
            painter.drawText(cell, Qt::AlignCenter, itemText(item));
        }
        itemRects_.push_back(cell);
        x += width + scaled(kBaseGap);
    }

    Q_UNUSED(program_);
}

Rect StatusWindow::availableRectAt(const QPoint &global) const {
    QScreen *screen = QGuiApplication::screenAt(global);
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        return Rect{0, 0, 0, 0};
    }
    const QRect available = screen->availableGeometry();
    return Rect{available.left(), available.top(), available.width(), available.height()};
}

void StatusWindow::reposition() {
    if (!customPosition_) {
        placeOverlay(*this, placement_);
        return;
    }
    const Point target{customPosition_->x, customPosition_->y};
    const Rect available = availableRectAt(QPoint(target.x, target.y));
    if (available.width <= 0 || available.height <= 0) {
        return;
    }
    const Point clamped =
        clampToScreen(target, Point{width(), height()}, available, kOverlayMargin);
    move(clamped.x, clamped.y);
}

void StatusWindow::mousePressEvent(QMouseEvent *event) {
    // An overlay must never take focus; only the left button is meaningful and
    // every click turns into a semantic command the agent validates.
    if (event->button() != Qt::LeftButton) {
        return;
    }

    // The grip is the only draggable area: everything else is a click target,
    // so "中"/"全角标点" can never be dragged by accident.
    const Point local{event->pos().x(), event->pos().y()};
    if (isDragHandleHit(local, width(), height(), scale_)) {
        dragging_ = true;
        dragOffset_ = event->globalPosition().toPoint() - pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    if (itemClick_ == nullptr) {
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

void StatusWindow::mouseMoveEvent(QMouseEvent *event) {
    if (dragging_) {
        move(event->globalPosition().toPoint() - dragOffset_);
        event->accept();
        return;
    }

    const Point local{event->pos().x(), event->pos().y()};
    const bool onHandle = isDragHandleHit(local, width(), height(), scale_);
    if (onHandle) {
        setCursor(Qt::OpenHandCursor);
    } else {
        unsetCursor();
    }
    if (onHandle != handleHover_) {
        handleHover_ = onHandle;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void StatusWindow::mouseReleaseEvent(QMouseEvent *event) {
    if (!dragging_ || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    dragging_ = false;
    setCursor(Qt::OpenHandCursor);

    const Point dropped{pos().x(), pos().y()};
    const Rect available = availableRectAt(QPoint(dropped.x, dropped.y));
    const Point clamped =
        clampToScreen(dropped, Point{width(), height()}, available, kOverlayMargin);
    if (clamped.x != dropped.x || clamped.y != dropped.y) {
        move(clamped.x, clamped.y);
    }
    customPosition_ = clamped;
    if (positionChange_) {
        positionChange_(clamped.x, clamped.y);
    }
    event->accept();
}

void StatusWindow::leaveEvent(QEvent *event) {
    if (!dragging_) {
        unsetCursor();
    }
    if (handleHover_) {
        handleHover_ = false;
        update();
    }
    QWidget::leaveEvent(event);
}

} // namespace superior::ui
