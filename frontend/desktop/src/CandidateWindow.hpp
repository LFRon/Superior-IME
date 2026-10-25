// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QSize>
#include <QString>
#include <QWidget>

#include <cstddef>
#include <functional>
#include <vector>

#include "superior/api/Scene.hpp"
#include "LayoutRules.hpp"
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

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    [[nodiscard]] int scaled(int value) const;

    /// Painted cell, kept so a click can be mapped back to a candidate id
    /// without recomputing the layout (fonts may have changed since).
    struct Cell {
        QRect rect;
        api::CandidateId id = 0;
    };

    SkinTheme theme_ = SkinTheme::fallback();
    api::Anchor lastAnchor_;
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
