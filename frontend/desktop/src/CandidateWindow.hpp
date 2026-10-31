// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QFont>
#include <QSize>
#include <QString>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "superior/api/Scene.hpp"
#include "LayoutRules.hpp"
#include "platform/Session.hpp"
#include "SkinTheme.hpp"

namespace superior::ui {

/// Candidate/preedit panel.
///
/// On X11 it is another override-redirect overlay, so it cannot steal focus
/// from the text client. On Wayland this class is intentionally not used for
/// the candidate panel: the compositor owns input-popup placement there.
class CandidateWindow : public QWidget {
public:
    explicit CandidateWindow(QWidget *parent = nullptr);

    void apply(const api::PanelScene &panel, const SkinTheme &theme);

    /// `settings.json` `uiScale`; 1.0 is 100%. Re-renders the last scene so the
    /// panel matches immediately instead of on the next keystroke.
    void setScale(double scale);

    /// Clicking a candidate sends its id; the agent decides what that means.
    void setActivateHandler(std::function<void(api::CandidateId)> handler);
    /// Mouse wheel paging: +1 next page, -1 previous page.
    void setPageHandler(std::function<void(int)> handler);

    /// Area another overlay currently occupies (the speech panel). The candidate
    /// panel is lifted above it instead of covering it; an empty rect disables
    /// the rule. Set by the host, because only the host sees all three windows.
    void setAvoidRect(const Rect &rect);

    /// Which platform implementation answers "where does this panel go".
    ///
    /// Set once by the host after it classified the session; the window itself
    /// never reads the environment or a platform API.
    void setPlatformSession(const Session &session);

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void moveEvent(QMoveEvent *event) override;

    /// Switches between "we place it" and "the compositor places it".
    ///
    /// The two need different window flags, and changing flags recreates the
    /// native window — so this only runs when the mode actually changes (mixed
    /// X11/Wayland focus, which is rare) and the caller re-shows afterwards.
    void setCompositorPlacement(bool managed);
    /// (Re)starts the "compositor has settled" timer; see `placementSettle_`.
    void armPlacementSettle();

private:
    [[nodiscard]] int scaled(int value) const;

    /// The font a candidate cell is measured and painted with.
    ///
    /// One function for both, because a cell measured without the emoji fallback
    /// clips the emoji it then draws.
    [[nodiscard]] QFont candidateFont() const;

    /// Painted cell, kept so a click can be mapped back to a candidate id
    /// without recomputing the layout (fonts may have changed since).
    struct Cell {
        QRect rect;
        api::CandidateId id = 0;
    };

    Session session_{};
    SkinTheme theme_ = SkinTheme::fallback();
    api::Anchor lastAnchor_;
    /// The compositor owns this window's position (Treeland candidate panel).
    ///
    /// Treeland adopts a window per *mapped surface*, so this can be true and
    /// then false again when the panel is unmapped. While it is true we do not
    /// move the window at all: the compositor places it from the application's
    /// own `set_cursor_rectangle`, which we cannot see on Wayland.
    bool compositorPlaced_ = false;
    /// Last position the compositor chose, reused as the provisional spot for the
    /// next composition. The window has to be mapped before it can be adopted, so
    /// the very first frame of a session is necessarily at our own guess; reusing
    /// the previous answer makes every later composition correct from frame one.
    QPoint lastCompositorPos_;
    bool haveCompositorPos_ = false;
    /// True while we are moving the window ourselves, so a `QMoveEvent` can tell
    /// "our own move" from "the compositor placed us".
    bool selfMoving_ = false;
    /// Current window policy; see `setCompositorPlacement`.
    bool managedWindow_ = false;
    /// True while a composition is waiting for the compositor to place the panel.
    ///
    /// Painting before that shows the panel at the previous composition's caret
    /// for one frame, which is what "it is not at the caret at first" looked
    /// like. The compositor repositions on every geometry change, so the resize
    /// that starts a composition always produces the answer we wait for.
    bool awaitingPlacement_ = false;
    /// Gates `paintEvent`: a mapped window with nothing painted stays invisible
    /// (translucent background), which is how the panel can be adopted *before*
    /// the first composition without flashing anywhere.
    bool paintReady_ = false;
    /// Fires once the compositor has stopped repositioning us.
    ///
    /// Treeland answers a geometry change with *several* arrangements in quick
    /// succession (measured: two positions 1 ms apart, then the settled one
    /// ~85 ms later). Painting on the first answer shows the previous field's
    /// caret, so the first frame waits for the stream to go quiet.
    QTimer *placementSettle_ = nullptr;
    /// Native window the candidate-panel property was last written to, so it is
    /// written once per window instead of once per keystroke.
    std::uintptr_t candidatePanelWindow_ = 0;
    Rect avoid_{};
    double scale_ = 1.0;
    bool panelVisible_ = false;
    QString preedit_;
    std::size_t preeditCursor_ = 0;
    std::vector<api::Candidate> candidates_;
    api::CandidateId selected_ = 0;
    QString pageInfo_;
    std::vector<Cell> cells_;
    int hoverIndex_ = -1;
    std::function<void(api::CandidateId)> activate_;
    std::function<void(int)> page_;
};

} // namespace superior::ui
