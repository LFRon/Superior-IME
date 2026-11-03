// SPDX-License-Identifier: GPL-3.0-or-later
#include "Overlay.hpp"

#include <QGuiApplication>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <vector>

#include "OverlayLayout.hpp"

namespace superior::ui {

void makeOverlay(QWidget &widget) {
    widget.setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint |
                          Qt::WindowStaysOnTopHint | Qt::BypassWindowManagerHint |
                          Qt::WindowDoesNotAcceptFocus);
    widget.setAttribute(Qt::WA_ShowWithoutActivating);
    widget.setAttribute(Qt::WA_TranslucentBackground);
    widget.setFocusPolicy(Qt::NoFocus);
}

void makeCompositorPlacedPanel(QWidget &widget) {
    widget.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
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

void placeAtCaret(QWidget &widget, const api::Anchor &anchor, int gap) {
    if (!anchor.valid || anchor.cursor.width < 0 || anchor.cursor.height < 0) {
        placeBottomRight(widget, 120);
        return;
    }

    // See `caretToLogical`: the division is per screen, and the screen has to be
    // picked from the device point because the logical point is the output.
    std::vector<QScreen *> ordered;
    if (QScreen *primary = QGuiApplication::primaryScreen()) {
        ordered.push_back(primary);
    }
    for (QScreen *candidate : QGuiApplication::screens()) {
        if (std::find(ordered.begin(), ordered.end(), candidate) == ordered.end()) {
            ordered.push_back(candidate);
        }
    }
    std::vector<ScreenSpace> spaces;
    spaces.reserve(ordered.size());
    for (const QScreen *candidate : ordered) {
        const QRect geometry = candidate->geometry();
        spaces.push_back(ScreenSpace{Rect{geometry.x(), geometry.y(), geometry.width(),
                                          geometry.height()},
                                     candidate->devicePixelRatio()});
    }

    std::size_t screenIndex = 0;
    const Rect caret = caretToLogical(anchor.cursor.x, anchor.cursor.y, anchor.cursor.width,
                                      anchor.cursor.height, spaces, &screenIndex);
    QScreen *screen = screenIndex < ordered.size() ? ordered[screenIndex] : nullptr;
    if (screen == nullptr) {
        return;
    }
    const QRect available = screen->availableGeometry();
    const Point offset{static_cast<int>(std::lround(anchor.offsetX)),
                       static_cast<int>(std::lround(anchor.offsetY))};
    const Rect placed =
        caretPopupPosition(caret, Size{widget.width(), widget.height()},
                           Rect{available.left(), available.top(), available.width(),
                                available.height()},
                           offset, gap);
    widget.move(placed.left, placed.top);
}

} // namespace superior::ui
