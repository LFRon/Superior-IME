// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>

#include <functional>
#include <optional>
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

    /// Restores a dragged position (`statusbar.json`). Empty means "use
    /// `statusPlacement` again", which is what picking another corner in the
    /// settings page does.
    void setSavedPosition(std::optional<Point> position);
    [[nodiscard]] std::optional<Point> savedPosition() const { return customPosition_; }

    /// Called once per finished drag with the final top-left corner.
    void setPositionChangeHandler(std::function<void(int, int)> handler);

    /// Emitted with the clicked item id (`inline-mode` for the 中/英 badge).
    void setItemClickHandler(std::function<void(const std::string &)> handler);

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    [[nodiscard]] int scaled(int value) const;
    /// Applies either the dragged position or the configured corner.
    void reposition();
    /// Screen under `global` (falling back to the primary screen).
    [[nodiscard]] Rect availableRectAt(const QPoint &global) const;

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
    bool dragging_ = false;
    bool handleHover_ = false;
    QPoint dragOffset_;
    std::optional<Point> customPosition_;
    std::function<void(const std::string &)> itemClick_;
    std::function<void(int, int)> positionChange_;
};

} // namespace superior::ui
