// SPDX-License-Identifier: GPL-3.0-or-later
#include "Overlay.hpp"

#include <QGuiApplication>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QWidget>

namespace superior::ui {

void makeOverlay(QWidget &widget) {
    widget.setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint |
                          Qt::WindowStaysOnTopHint | Qt::BypassWindowManagerHint |
                          Qt::WindowDoesNotAcceptFocus);
    widget.setAttribute(Qt::WA_ShowWithoutActivating);
    widget.setAttribute(Qt::WA_TranslucentBackground);
    widget.setFocusPolicy(Qt::NoFocus);
}

void placeOverlay(QWidget &widget, Placement placement, int extraBottom) {
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        return;
    }
    const QRect available = screen->availableGeometry();
    const Point position =
        overlayPosition(Point{widget.width(), widget.height()}, placement,
                        Rect{available.left(), available.top(), available.width(),
                             available.height()},
                        kOverlayMargin, extraBottom);
    widget.move(position.x, position.y);
}

void placeBottomRight(QWidget &widget, int extraBottom) {
    placeOverlay(widget, Placement::BottomRight, extraBottom);
}

} // namespace superior::ui
