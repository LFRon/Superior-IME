// SPDX-License-Identifier: GPL-3.0-or-later
#include "CandidateWindow.hpp"

#include "OverlayLayout.hpp"

#include <QEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPoint>
#include <QRect>
#include <QMoveEvent>
#include <QScreen>
#include <QTimer>
#include <QString>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include "CandidateLabel.hpp"
#include "EmojiFont.hpp"
#include "PreeditLayout.hpp"
#include "LayoutRules.hpp"
#include "platform/Platform.hpp"

#include "Overlay.hpp"
#include "WheelStep.hpp"

#include "superior/diagnostics/Log.hpp"

namespace superior::ui {

namespace diag = ::superior::diagnostics;

namespace {

constexpr double kPreeditPointSize = 9.5;
constexpr double kCandidatePointSize = 10.5;
constexpr int kCellHeight = 28;
constexpr int kPreeditRowHeight = 24;
constexpr int kCellPadding = 20;
constexpr int kMinCellWidth = 28;

/// The colour emoji family to fall back to, resolved once.
///
/// `QFontDatabase::families()` walks fontconfig, so it is not something to call
/// per frame; the answer cannot change without a font install, which needs a
/// restart of the overlay anyway.
QString emojiFamily() {
    static const QString family = [] {
        const QStringList families = QFontDatabase::families();
        std::vector<std::string> installed;
        installed.reserve(static_cast<std::size_t>(families.size()));
        for (const QString &name : families) {
            installed.push_back(name.toStdString());
        }
        const std::string picked = pickEmojiFamily(installed);
        if (picked.empty()) {
            // The candidate still draws; it just cannot be in colour. Worth a
            // line, because "my emoji look like outlines" has exactly this cause.
            SUPERIOR_WARN(diag::categories::Ui)
                .field("families", static_cast<std::int64_t>(families.size()))
                << "no emoji font installed; emoji candidates will be monochrome";
        } else {
            SUPERIOR_INFO(diag::categories::Ui).field("family", picked) << "emoji font";
        }
        return QString::fromStdString(picked);
    }();
    return family;
}

QString cellText(const api::Candidate &candidate, int index) {
    const auto parts = formatCandidateLabel(candidate.label, candidate.text, index);
    if (parts.prefix.empty()) {
        return QString::fromStdString(parts.text);
    }
    return QString::fromStdString(parts.prefix) + QLatin1Char(' ') +
           QString::fromStdString(parts.text);
}

/// How long a composition waits for the compositor before painting anywhere.
///
/// Treeland reads the candidate-panel property on a 50 ms asynchronous timer, so
/// this only has to be longer than that; the wait is invisible because the window
/// is painted empty until the placement arrives.
constexpr int kPlacementTimeoutMs = 200;

/// One-pixel detour used to make a size change observable.
///
/// Held for slightly longer than a compositor frame: a resize that is undone
/// before the compositor composites is invisible to it, and an invisible change
/// triggers no reposition.
constexpr int kGeometryNudgePx = 2;
constexpr int kGeometryNudgeMs = 30;

/// How long the first frame of a composition may wait for the compositor.
///
/// Kept short on purpose: Treeland also re-arranges *later* (measured: a second,
/// different position ~700 ms after the first), and the channel carries no
/// serial, so a client cannot tell "settled" from "about to change again".
constexpr int kSettleQuietMs = 20;

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

    // The scene publishes the caret in session device pixels (an X server has no
    // logical layer); Qt moves windows in logical pixels. Skipping the division
    // is what left a gap that grew with the distance from the origin on a
    // fractional-scale desktop — and doing the division with one global ratio is
    // what put the panel on the wrong screen when two screens scale differently.
    // So: describe every screen, let the caret pick one, and map inside it.
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
    const Rect caretRect = caretToLogical(anchor.cursor.x, anchor.cursor.y, anchor.cursor.width,
                                          anchor.cursor.height, spaces, &screenIndex);
    const QPoint caret(caretRect.left, caretRect.top);

    QScreen *screen = screenIndex < ordered.size() ? ordered[screenIndex] : nullptr;
    if (screen == nullptr) {
        return;
    }
    const QRect available = screen->availableGeometry();

    int x = caret.x() + static_cast<int>(anchor.offsetX);
    int y = caret.y() + caretRect.height + 6 + static_cast<int>(anchor.offsetY);

    x = std::clamp(x, available.left() + 4, available.right() - widget.width() - 4);
    y = std::clamp(y, available.top() + 4, available.bottom() - widget.height() - 4);
    widget.move(x, y);
}

/// Keeps `widget` off `avoid` without changing any other part of its position.
void stepAround(QWidget &widget, const Rect &avoid) {
    if (avoid.width <= 0 || avoid.height <= 0) {
        return;
    }
    const QRect geometry = widget.geometry();
    const Rect current{geometry.left(), geometry.top(), geometry.width(), geometry.height()};
    const QScreen *screen = QGuiApplication::screenAt(geometry.center());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        return;
    }
    const QRect available = screen->availableGeometry();
    const Rect placed =
        liftAbove(current, avoid, Rect{available.left(), available.top(), available.width(),
                                       available.height()});
    if (placed.left != current.left || placed.top != current.top) {
        widget.move(placed.left, placed.top);
    }
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
        stepAround(*this, avoid_);
        update();
    }
}

void CandidateWindow::setAvoidRect(const Rect &rect) {
    avoid_ = rect;
    // The speech panel can appear while a composition is already on screen, so
    // the rule has to apply immediately rather than on the next keystroke.
    if (panelVisible_ && (!preedit_.isEmpty() || !candidates_.empty())) {
        placeAtCaret(*this, lastAnchor_);
        stepAround(*this, avoid_);
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
        if (managedWindow_) {
            // Staying mapped is the whole point: the compositor adopts a mapped
            // surface, and an unmap drops the adoption permanently. An empty
            // 1x1 translucent window costs nothing and follows the caret, so the
            // next composition starts from the right place.
            paintReady_ = false;
            awaitingPlacement_ = false;
            resize(1, 1);
            update();
            return;
        }
        hide();
        return;
    }

    resize(sizeHint());

    // Placement has three possible owners, in decreasing order of accuracy:
    //
    // 1. the compositor, when it implements the IM candidate-panel channel and
    //    the caret is unknown to us — it sees the application's own
    //    `set_cursor_rectangle`, which an external process cannot obtain on
    //    Wayland;
    // 2. our own placement at the caret Fcitx5 reported (X11/XIM clients);
    // 3. a fixed corner, when there is neither.
    //
    // `winId()` has to be called after `show()`: the native window does not
    // exist before the first map, and the property would be written to nothing.
    const bool caretKnown = panel.anchor.valid;
    const bool compositorHandles = !caretKnown && compositorPlacesCandidatePanel(session_);
    if (compositorHandles) {
        // A compositor only manages windows it knows about, so the panel has to
        // give up override-redirect to be adopted; everything that flag bought
        // (no decoration, no focus, stays on top) the adopting compositor
        // supplies itself. Hiding between compositions is *not* an option either:
        // Treeland adopts a mapped surface, and an unmap drops the adoption.
        if (!managedWindow_) {
            setCompositorPlacement(true);
        }
        if (!isVisible()) {
            // Nothing has been adopted yet, so map it somewhere harmless: the
            // previous answer beats a corner, and it is invisible until painted.
            if (haveCompositorPos_) {
                move(lastCompositorPos_);
            } else {
                placeAtCaret(*this, panel.anchor);
            }
            paintReady_ = false;
            show();
        }
        // Write once per window: every write makes Treeland re-read the atom on
        // a 50 ms timer, and a keystroke must not trigger a polling round.
        const auto currentWindow = static_cast<std::uintptr_t>(winId());
        if (currentWindow != candidatePanelWindow_) {
            if (adoptAsCandidatePanel(session_, currentWindow) == PanelChannel::Applied) {
                candidatePanelWindow_ = currentWindow;
            }
        }
        compositorPlaced_ = true;
        if (!paintReady_) {
            awaitingPlacement_ = true;
            // Treeland repositions on a geometry change, but only when it
            // observes one: a composition whose size equals the previous one
            // produces no event and no reposition. A two-pixel detour, held for
            // one compositor frame, guarantees the observation (restoring on the
            // next event-loop turn is coalesced away and produces no change).
            const QSize target = sizeHint();
            resize(target.width() + kGeometryNudgePx, target.height());
            QTimer::singleShot(kGeometryNudgeMs, this, [this, target] {
                if (awaitingPlacement_) {
                    resize(target);
                }
            });
            // Paint as soon as the compositor answers. It can answer more than
            // once — including *after* this composition — and there is no serial
            // in the channel to tell a stale answer from the settling one, so
            // waiting only delays the first frame without making it correct.
            armPlacementSettle();
            QTimer::singleShot(kPlacementTimeoutMs, this, [this] {
                if (!awaitingPlacement_) {
                    return;
                }
                awaitingPlacement_ = false;
                paintReady_ = true;
                update();
            });
        }
        update();
        return;
    }

    // A caret we can trust (or no caret at all): place it ourselves. Treeland
    // stops repositioning in this case because an X11 focus has no text-input
    // surface.
    if (managedWindow_) {
        setCompositorPlacement(false);
    }
    compositorPlaced_ = false;
    // Nothing else is going to place this window, so there is nothing to wait
    // for, and `paintReady_` gate is what makes the window visible at all: a
    // translucent window that paints nothing shows nothing. This is the only
    // path on X11 and XWayland, where leaving the flag at "waiting for the
    // compositor" hid the panel completely.
    awaitingPlacement_ = false;
    paintReady_ = true;
    selfMoving_ = true;
    placeAtCaret(*this, panel.anchor);
    stepAround(*this, avoid_);
    selfMoving_ = false;
    show();
    update();
}

QFont CandidateWindow::candidateFont() const {
    QFont candidate = font();
    candidate.setPointSizeF(kCandidatePointSize * scale_);
    // A candidate can be an emoji, and Qt's own fallback picks a monochrome
    // glyph for it even when a colour emoji font is installed. Naming the family
    // is what turns the outline into the yellow face.
    const QString emoji = emojiFamily();
    if (!emoji.isEmpty()) {
        QStringList families = candidate.families();
        families.removeAll(QString());
        if (families.isEmpty()) {
            families << (candidate.family().isEmpty()
                             ? QFontDatabase::systemFont(QFontDatabase::GeneralFont).family()
                             : candidate.family());
        }
        if (!families.contains(emoji)) {
            families << emoji;
            candidate.setFamilies(families);
        }
    }
    return candidate;
}

QSize CandidateWindow::sizeHint() const {
    const QFont metricsFont = candidateFont();
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
    if (!paintReady_) {
        // Nothing painted on a translucent window keeps it invisible while the
        // compositor is still deciding where it goes.
        return;
    }
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

    const QFont cellFont = candidateFont();
    painter.setFont(cellFont);
    const QFontMetrics metrics(cellFont);

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

void CandidateWindow::armPlacementSettle() {
    if (placementSettle_ == nullptr) {
        placementSettle_ = new QTimer(this);
        placementSettle_->setSingleShot(true);
        placementSettle_->setInterval(kSettleQuietMs);
        connect(placementSettle_, &QTimer::timeout, this, [this] {
            if (!awaitingPlacement_) {
                return;
            }
            awaitingPlacement_ = false;
            paintReady_ = true;
            update();
        });
    }
    placementSettle_->start();
}

void CandidateWindow::setPlatformSession(const Session &session) {
    session_ = session;
}

void CandidateWindow::setCompositorPlacement(bool managed) {
    managedWindow_ = managed;
    if (managed) {
        makeCompositorPlacedPanel(*this);
    } else {
        makeOverlay(*this);
    }
    // `setWindowFlags` hides the widget and drops the native window; the caller
    // re-shows it, and the property is written again on the next frame because
    // the new window has never been seen by the compositor.
    winId();
}

void CandidateWindow::moveEvent(QMoveEvent *event) {
    if (compositorPlaced_ && !selfMoving_) {
        // Not ours: the compositor placed us from the application's cursor
        // rectangle. That is the answer this composition was waiting for.
        lastCompositorPos_ = event->pos();
        haveCompositorPos_ = true;
        if (awaitingPlacement_) {
            // Another arrangement is probably on its way; wait for silence
            // instead of painting the first answer (which is the previous
            // caret's position).
            armPlacementSettle();
        }
    }
    QWidget::moveEvent(event);
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
