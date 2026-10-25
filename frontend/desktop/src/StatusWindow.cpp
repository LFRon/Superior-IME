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
    // Needed for the hand cursor on the grip: without tracking we get no move
    // events while hovering.
    setMouseTracking(true);
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
