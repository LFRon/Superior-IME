// SPDX-License-Identifier: GPL-3.0-or-later
#include "CandidateWindow.hpp"

#include <QFontMetrics>
#include <QGuiApplication>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRect>
#include <QScreen>
#include <QString>

#include <algorithm>
#include <cmath>

#include "LayoutRules.hpp"
#include "Overlay.hpp"

namespace superior::ui {

namespace {

constexpr double kPreeditPointSize = 9.5;
constexpr double kCandidatePointSize = 10.5;

QString candidateText(const api::Candidate &candidate) {
    QString text = QString::fromStdString(candidate.text);
    if (text.isEmpty()) {
        text = QString::fromStdString(candidate.label);
    }
    return text;
}

/// Places the panel just below the caret, clamped to the screen.
///
/// `Anchor::cursor` is reported by Fcitx5. For X11 clients it is a global
/// coordinate; for Wayland clients the compositor may not expose it, in which
/// case the anchor is invalid and the caller falls back to a fixed corner.
void placeAtCaret(QWidget &widget, const api::Anchor &anchor) {
    if (!anchor.valid || anchor.cursor.width < 0 || anchor.cursor.height < 0) {
        placeBottomRight(widget, 120);
        return;
    }

    const QPoint caret(static_cast<int>(anchor.cursor.x), static_cast<int>(anchor.cursor.y));
    QScreen *screen = QGuiApplication::screenAt(caret);
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        return;
    }
    const QRect available = screen->availableGeometry();

    int x = caret.x() + static_cast<int>(anchor.offsetX);
    int y = caret.y() + static_cast<int>(anchor.cursor.height) + 6 +
            static_cast<int>(anchor.offsetY);

    x = std::clamp(x, available.left() + 4, available.right() - widget.width() - 4);
    y = std::clamp(y, available.top() + 4, available.bottom() - widget.height() - 4);
    widget.move(x, y);
}

} // namespace

CandidateWindow::CandidateWindow(QWidget *parent) : QWidget(parent) {
    makeOverlay(*this);
    setWindowTitle(QStringLiteral("Superior-IME Candidates"));
    resize(240, 56);
    hide();
}

int CandidateWindow::scaled(int value) const {
    return static_cast<int>(std::lround(static_cast<double>(value) * scale_));
}

void CandidateWindow::setScale(double scale) {
    const double bounded = clampScale(scale);
    if (std::abs(bounded - scale_) < 0.001) {
        return;
    }
    scale_ = bounded;
    if (panelVisible_ && (!preedit_.isEmpty() || !candidates_.empty())) {
        resize(sizeHint());
        placeAtCaret(*this, lastAnchor_);
        update();
    }
}

void CandidateWindow::apply(const api::PanelScene &panel, const SkinTheme &theme) {
    theme_ = theme;
    lastAnchor_ = panel.anchor;
    panelVisible_ = panel.visible;
    preedit_ = QString::fromStdString(panel.preedit.text);
    candidates_ = panel.candidates.items;
    selected_ = panel.selected;

    if (panel.candidates.pageCount > 1) {
        pageInfo_ = QString("%1/%2")
                        .arg(panel.candidates.pageIndex + 1)
                        .arg(panel.candidates.pageCount);
    } else {
        pageInfo_.clear();
    }

    const bool shouldShow = panelVisible_ && (!preedit_.isEmpty() || !candidates_.empty());
    if (!shouldShow) {
        hide();
        return;
    }

    resize(sizeHint());
    placeAtCaret(*this, panel.anchor);
    show();
    update();
}

QSize CandidateWindow::sizeHint() const {
    QFont metricsFont = font();
    metricsFont.setPointSizeF(kCandidatePointSize * scale_);
    const QFontMetrics metrics(metricsFont);
    int width = scaled(theme_.paddingX) * 2;
    for (const auto &candidate : candidates_) {
        width += std::max(scaled(28), metrics.horizontalAdvance(candidateText(candidate)) +
                                          scaled(20)) +
                 scaled(theme_.gap);
    }
    if (!pageInfo_.isEmpty()) {
        width += metrics.horizontalAdvance(pageInfo_) + scaled(theme_.gap);
    }
    const int height = (preedit_.isEmpty() ? 0 : scaled(22)) +
                       (candidates_.empty() ? scaled(20) : scaled(28) + scaled(theme_.paddingY) * 2);
    return QSize(std::max(scaled(160), width), std::max(scaled(40), height));
}

void CandidateWindow::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QPainterPath background;
    background.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), theme_.radius,
                              theme_.radius);
    painter.fillPath(background, theme_.surface);
    painter.setPen(theme_.showSeparator ? theme_.border : QColor(255, 255, 255, 24));
    painter.drawPath(background);

    int top = scaled(theme_.paddingY);
    if (!preedit_.isEmpty()) {
        QFont preeditFont = font();
        preeditFont.setPointSizeF(kPreeditPointSize * scale_);
        painter.setFont(preeditFont);
        painter.setPen(theme_.comment);
        painter.drawText(QRect(scaled(theme_.paddingX), top,
                               width() - scaled(theme_.paddingX) * 2, scaled(18)),
                         Qt::AlignLeft | Qt::AlignVCenter, preedit_);
        top += scaled(20);
    }

    if (candidates_.empty()) {
        return;
    }

    QFont candidateFont = font();
    candidateFont.setPointSizeF(kCandidatePointSize * scale_);
    painter.setFont(candidateFont);
    const QFontMetrics metrics(candidateFont);

    int x = scaled(theme_.paddingX);
    const int cellHeight = scaled(28);
    for (std::size_t index = 0; index < candidates_.size(); ++index) {
        const auto &candidate = candidates_[index];
        const QString text = candidateText(candidate);
        const int cellWidth =
            std::max(scaled(28), metrics.horizontalAdvance(text) + scaled(20));
        const QRect cell(x, top, cellWidth, cellHeight);

        if (candidate.id == selected_ && selected_ != 0) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme_.selection);
            painter.drawRoundedRect(cell, theme_.radius > 4 ? 6 : 2, theme_.radius > 4 ? 6 : 2);
            painter.setPen(theme_.selectionText);
        } else {
            painter.setPen(index == 0 ? theme_.firstText : theme_.text);
        }
        painter.drawText(cell, Qt::AlignCenter, text);
        x += cellWidth + scaled(theme_.gap);
    }

    if (!pageInfo_.isEmpty()) {
        painter.setPen(theme_.comment);
        painter.drawText(QRect(x, top, width() - x - scaled(theme_.paddingX), cellHeight),
                         Qt::AlignVCenter | Qt::AlignRight, pageInfo_);
    }
}

} // namespace superior::ui
