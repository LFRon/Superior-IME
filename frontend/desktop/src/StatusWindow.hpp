// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>

#include <functional>
#include <string>
#include <vector>

#include "superior/api/Scene.hpp"

#include "LayoutRules.hpp"
#include "SkinTheme.hpp"

namespace superior::ui {

/// Bottom-right status bar.
///
/// The window is override-redirect on X11: permanent stacking above normal
/// windows, no focus stealing. It never owns input state; it only paints the
/// last scene received from the agent and forwards clicks as semantic commands
/// (`ToggleStatusItem`), which the agent decides how to interpret.
class StatusWindow : public QWidget {
public:
    explicit StatusWindow(QWidget *parent = nullptr);

    void apply(const api::StatusScene &status, const std::string &mode,
               const std::string &program, const SkinTheme &theme);

    /// Screen anchored placement, from `settings.json` (`statusPlacement`).
    void setPlacement(Placement placement);
    /// `settings.json` `uiScale`; 1.0 is 100%.
    void setScale(double scale);
    /// `settings.json` `showStatusBar`: hiding is a settings decision, not a
    /// scene decision, so it survives the next scene frame.
    void setEnabled(bool enabled);

    /// Emitted with the clicked item id (`inline-mode` for the 中/英 badge).
    void setItemClickHandler(std::function<void(const std::string &)> handler);

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    [[nodiscard]] int scaled(int value) const;

    SkinTheme theme_ = SkinTheme::fallback();
    std::vector<api::StatusItem> items_;
    std::vector<QRect> itemRects_;
    QRect modeRect_;
    QString mode_;
    QString program_;
    Placement placement_ = Placement::BottomRight;
    double scale_ = 1.0;
    bool enabled_ = true;
    bool visible_ = false;
    std::function<void(const std::string &)> itemClick_;
};

} // namespace superior::ui
