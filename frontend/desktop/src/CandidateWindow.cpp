// SPDX-License-Identifier: GPL-3.0-or-later
#include "CandidateWindow.hpp"

#include <QEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QString>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <string>

#include "CandidateLabel.hpp"
#include "PreeditLayout.hpp"
#include "LayoutRules.hpp"
#include "Overlay.hpp"
#include "WheelStep.hpp"

namespace superior::ui {

namespace {

constexpr double kPreeditPointSize = 9.5;
constexpr double kCandidatePointSize = 10.5;
constexpr int kCellHeight = 28;
constexpr int kPreeditRowHeight = 24;
constexpr int kCellPadding = 20;
constexpr int kMinCellWidth = 28;

QString cellText(const api::Candidate &candidate, int index) {
    const auto parts = formatCandidateLabel(candidate.label, candidate.text, index);
    if (parts.prefix.empty()) {
        return QString::fromStdString(parts.text);
    }
    return QString::fromStdString(parts.prefix) + QLatin1Char(' ') +
           QString::fromStdString(parts.text);
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
    // Hover highlight needs move events without a pressed button.
    setMouseTracking(true);
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
    preeditCursor_ = panel.preedit.cursor;
    candidates_ = panel.candidates.items;
    selected_ = panel.selected;

    if (panel.candidates.pageCount > 1) {
        pageInfo_ = QString("%1/%2")
                        .arg(panel.candidates.pageIndex + 1)
                        .arg(panel.candidates.pageCount);
    } else {
        pageInfo_.clear();
    }

    // A new scene invalidates the painted layout: drop hover so the highlight
    // does not stick to a cell that now holds a different word.
    cells_.clear();
    hoverIndex_ = -1;

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
    for (std::size_t index = 0; index < candidates_.size(); ++index) {
        width += std::max(scaled(kMinCellWidth),
                          metrics.horizontalAdvance(cellText(candidates_[index],
                                                             static_cast<int>(index))) +
                              scaled(kCellPadding)) +
                 scaled(theme_.gap);
    }
    if (!pageInfo_.isEmpty()) {
        width += metrics.horizontalAdvance(pageInfo_) + scaled(theme_.gap);
    }
    // A long composition must not be clipped by the candidate row's width.
    if (!preedit_.isEmpty()) {
        QFont preeditFont = font();
        preeditFont.setPointSizeF(kPreeditPointSize * scale_);
        width = std::max(width, QFontMetrics(preeditFont).horizontalAdvance(preedit_) +
                                    scaled(theme_.paddingX) * 2);
    }
    const int height = (preedit_.isEmpty() ? 0 : scaled(kPreeditRowHeight)) +
                       (candidates_.empty() ? scaled(20)
                                            : scaled(kCellHeight) + scaled(theme_.paddingY) * 2);
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
        // The composition is part of the skin now: the pinyin, an underline and
        // a caret, all drawn here instead of inside the application.
        QFont preeditFont = font();
        preeditFont.setPointSizeF(kPreeditPointSize * scale_);
        painter.setFont(preeditFont);
        const QFontMetrics preeditMetrics(preeditFont);

        const auto split = splitPreedit(preedit_.toStdString(), preeditCursor_);
        const QString before = QString::fromStdString(split.before);
        const QString after = QString::fromStdString(split.after);
        const int baselineTop = top;
        const int rowHeight = scaled(kPreeditRowHeight);
        const int left = scaled(theme_.paddingX);
        const int advanceBefore = preeditMetrics.horizontalAdvance(before);

        painter.setPen(theme_.text);
        painter.drawText(QRect(left, baselineTop, advanceBefore + 2, rowHeight),
                         Qt::AlignLeft | Qt::AlignVCenter, before);
        painter.drawText(QRect(left + advanceBefore, baselineTop,
                               preeditMetrics.horizontalAdvance(after) + 2, rowHeight),
                         Qt::AlignLeft | Qt::AlignVCenter, after);

        // Underline the whole composition: that is what tells the user the text
        // is not committed yet.
        const int underlineY = baselineTop + rowHeight / 2 + preeditMetrics.ascent() / 2 + 2;
        QColor underline = theme_.text;
        underline.setAlpha(140);
        painter.setPen(QPen(underline, 1.0));
        painter.drawLine(left, underlineY,
                         left + preeditMetrics.horizontalAdvance(preedit_), underlineY);

        // Caret at the composition cursor.
        const int caretX = left + advanceBefore;
        painter.setPen(QPen(theme_.firstText, std::max(1.0, 1.5 * scale_)));
        painter.drawLine(caretX, baselineTop + scaled(4), caretX,
                         baselineTop + rowHeight - scaled(4));
        top += rowHeight;
    }

    if (candidates_.empty()) {
        return;
    }

    QFont candidateFont = font();
    candidateFont.setPointSizeF(kCandidatePointSize * scale_);
    painter.setFont(candidateFont);
    const QFontMetrics metrics(candidateFont);

    int x = scaled(theme_.paddingX);
    const int cellHeight = scaled(kCellHeight);
    const int cellRadius = theme_.radius > 4 ? 6 : 2;

    cells_.clear();
    cells_.reserve(candidates_.size());
    for (std::size_t position = 0; position < candidates_.size(); ++position) {
        const int index = static_cast<int>(position);
        const auto &candidate = candidates_[position];
        const QString text = cellText(candidate, index);
        const int cellWidth =
            std::max(scaled(kMinCellWidth), metrics.horizontalAdvance(text) + scaled(kCellPadding));
        const QRect cell(x, top, cellWidth, cellHeight);
        cells_.push_back(Cell{cell, candidate.id});

        const bool isSelected = candidate.id == selected_ && selected_ != 0;
        const bool isHovered = index == hoverIndex_ && !isSelected;

        if (isSelected) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme_.selection);
            painter.drawRoundedRect(cell, cellRadius, cellRadius);
            painter.setPen(theme_.selectionText);
        } else {
            if (isHovered) {
                QColor hover = theme_.selection;
                hover.setAlpha(70);
                painter.setPen(Qt::NoPen);
                painter.setBrush(hover);
                painter.drawRoundedRect(cell, cellRadius, cellRadius);
            }
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

void CandidateWindow::setActivateHandler(std::function<void(api::CandidateId)> handler) {
    activate_ = std::move(handler);
}

void CandidateWindow::setPageHandler(std::function<void(int)> handler) {
    page_ = std::move(handler);
}

void CandidateWindow::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton || activate_ == nullptr) {
        return;
    }
    for (const auto &cell : cells_) {
        if (!cell.rect.contains(event->pos())) {
            continue;
        }
        // The id travels back to the agent, which re-validates it against the
        // live list: a click on a stale frame cannot select the wrong word.
        activate_(cell.id);
        event->accept();
        return;
    }
}

void CandidateWindow::mouseMoveEvent(QMouseEvent *event) {
    int hovered = -1;
    for (std::size_t index = 0; index < cells_.size(); ++index) {
        if (cells_[index].rect.contains(event->pos())) {
            hovered = static_cast<int>(index);
            break;
        }
    }
    if (hovered != hoverIndex_) {
        hoverIndex_ = hovered;
        setCursor(hovered >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void CandidateWindow::leaveEvent(QEvent *event) {
    if (hoverIndex_ != -1) {
        hoverIndex_ = -1;
        update();
    }
    QWidget::leaveEvent(event);
}

void CandidateWindow::wheelEvent(QWheelEvent *event) {
    if (page_ == nullptr) {
        QWidget::wheelEvent(event);
        return;
    }
    const int step =
        pageStepForWheel(static_cast<int>(event->angleDelta().y()));
    if (step == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    page_(step);
    event->accept();
}

} // namespace superior::ui
