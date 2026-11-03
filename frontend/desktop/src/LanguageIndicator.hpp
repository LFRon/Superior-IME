// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QSize>
#include <QString>
#include <QWidget>

#include "superior/api/Scene.hpp"

#include "SkinTheme.hpp"

namespace superior::ui {

/// The short-lived language badge next to the caret.
///
/// Shown by the runtime when the status bar is hidden and Shift flips 中/英:
/// the box says 「拼」 in Chinese mode and 「A」 in English mode. Like the other
/// overlays it never takes focus or input; it is a pure effect of the scene.
class LanguageIndicator : public QWidget {
public:
    explicit LanguageIndicator(QWidget *parent = nullptr);

    /// Applies one complete scene frame. A hidden or empty indicator unmaps the
    /// window; the runtime owns the timing, so no local timer is involved.
    void apply(const api::IndicatorScene &indicator, const SkinTheme &theme);

    /// `settings.json` `uiScale`; 1.0 is 100%.
    void setScale(double scale);

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    [[nodiscard]] int scaled(int value) const;

    SkinTheme theme_ = SkinTheme::fallback();
    api::Anchor anchor_;
    QString text_;
    double scale_ = 1.0;
    bool visible_ = false;
};

} // namespace superior::ui
