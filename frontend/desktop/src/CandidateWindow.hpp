// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QSize>
#include <QString>
#include <QWidget>

#include <vector>

#include "superior/api/Scene.hpp"
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

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    [[nodiscard]] int scaled(int value) const;

    SkinTheme theme_ = SkinTheme::fallback();
    api::Anchor lastAnchor_;
    double scale_ = 1.0;
    bool panelVisible_ = false;
    QString preedit_;
    std::vector<api::Candidate> candidates_;
    api::CandidateId selected_ = 0;
    QString pageInfo_;
};

} // namespace superior::ui
