// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QWidget>

#include <functional>
#include <vector>

#include "superior/config/Fcitx5Config.hpp"
#include "superior/config/Settings.hpp"
#include "superior/skin/Skin.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSlider;
class QStackedWidget;
class QVBoxLayout;

namespace superior::ui {

/// Toggle switch drawn by hand.
///
/// A plain QWidget with a callback instead of a signal keeps the class free of
/// moc while still behaving like a normal widget (mouse and keyboard).
class ToggleSwitch : public QWidget {
public:
    explicit ToggleSwitch(bool checked, QWidget *parent = nullptr);

    [[nodiscard]] bool isChecked() const noexcept { return checked_; }
    void setChecked(bool checked);
    void setOnChange(std::function<void(bool)> callback);

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    bool checked_ = false;
    std::function<void(bool)> onChange_;
};

/// Native Qt settings window.
///
/// Layout: category sidebar with search on the left, grouped cards on the
/// right. Compared with the reference dialog (Sogou), this version:
///   - drops upgrade / account / ads sections entirely;
///   - uses switches for booleans and a slider for ranges;
///   - keeps the effect next to each control instead of one shared preview;
///   - makes every write reversible and reports what changed.
///
/// Architecture: this frontend includes only `superior/api`, `superior/skin`,
/// `superior/config` and `superior/theme`. It never touches `superior/core`,
/// any Fcitx5 header.
class SettingsWindow : public QWidget {
public:
    explicit SettingsWindow(config::Paths paths, QWidget *parent = nullptr);

private:
    void buildUi();
    void loadFromDisk();
    void syncWidgets();
    void reloadFcitxState();
    void applyChanges();
    void refreshSkinList();
    void setStatus(const QString &text, bool error = false);

    [[nodiscard]] config::Settings currentSettings() const;

    QWidget *buildAppearancePage();
    QWidget *buildInputPage();
    QWidget *buildStatusPage();
    QWidget *buildVoicePage();
    QWidget *buildAdvancedPage();
    QWidget *buildAboutPage();

    /// Creates a card and returns its content layout so callers add rows only.
    QVBoxLayout *addCard(QVBoxLayout *pageLayout, const QString &title, const QString &subtitle);

    config::Paths paths_;
    config::Settings settings_;
    config::FcitxStatus fcitxState_;
    std::vector<skin::SkinSummary> skins_;

    QLineEdit *search_ = nullptr;
    QListWidget *nav_ = nullptr;
    QStackedWidget *pages_ = nullptr;
    QLabel *pageTitle_ = nullptr;
    QLabel *pageSubtitle_ = nullptr;
    QLabel *status_ = nullptr;

    ToggleSwitch *shiftToggle_ = nullptr;
    ToggleSwitch *shiftCommitToggle_ = nullptr;
    ToggleSwitch *statusBarToggle_ = nullptr;
    ToggleSwitch *voiceToggle_ = nullptr;
    ToggleSwitch *cloudToggle_ = nullptr;
    QComboBox *cloudBackendCombo_ = nullptr;
    QComboBox *skinCombo_ = nullptr;
    QComboBox *placementCombo_ = nullptr;
    QComboBox *logLevelCombo_ = nullptr;
    QSlider *scaleSlider_ = nullptr;
    QLabel *scaleValue_ = nullptr;
    QLabel *themeState_ = nullptr;
    QLabel *triggerState_ = nullptr;
};

} // namespace superior::ui
