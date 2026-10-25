// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QString>
#include <QWidget>

#include <functional>
#include <optional>
#include <string>

#include "superior/api/Commands.hpp"
#include "superior/api/Scene.hpp"

#include "LayoutRules.hpp"
#include "SkinTheme.hpp"

class QComboBox;
class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;
class QTimer;

namespace superior::ui {

/// The speech-recognition panel: a persistent, always-on-top surface.
///
/// It is deliberately *not* a dialog. Sogou and iFlytek both keep a small panel
/// on screen while dictating; closing it is an explicit action, not a side
/// effect of finishing a sentence. The panel owns no semantics: it renders a
/// `SpeechScene` and sends commands, exactly like the candidate window.
class SpeechRecognitionPanel : public QWidget {
public:
    explicit SpeechRecognitionPanel(QWidget *parent = nullptr);

    using CommandHandler = std::function<void(const api::UiCommand &)>;
    void setCommandHandler(CommandHandler handler);

    void apply(const api::SpeechScene &scene, const api::SessionKey &session,
               const SkinTheme &theme);
    void setScale(double scale);

    /// Screen area the panel must stay inside; provided by the host so the pure
    /// layout function does the arithmetic.
    void setScreen(const Rect &available);
    /// Current status-bar rect, so the panel can step around it.
    void setStatusBarRect(const Rect &rect);

    /// Rect the panel occupies right now, or an empty rect when hidden. The host
    /// uses it to keep the candidate window from covering the panel.
    [[nodiscard]] Rect occupiedRect() const;

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    [[nodiscard]] bool setupPhase() const;
    void reposition();
    void updateActions();
    void beginDownload();
    void cancelDownload();
    void finishDownload();
    void saveModelChoice(const std::string &id);

    CommandHandler command_;
    api::SpeechScene scene_{};
    api::SessionKey session_{};
    SkinTheme theme_ = SkinTheme::fallback();
    double scale_ = 1.0;
    Rect screen_{};
    Rect statusBar_{};
    /// Live input level from the capability process, `[0, 1]`.
    double level_ = 0.0;

    QLabel *title_ = nullptr;
    QLabel *message_ = nullptr;
    QLabel *partial_ = nullptr;
    QComboBox *model_ = nullptr;
    QPushButton *primary_ = nullptr;
    QPushButton *discard_ = nullptr;
    QPushButton *close_ = nullptr;
    QPushButton *download_ = nullptr;

    QNetworkAccessManager *network_ = nullptr;
    QNetworkReply *reply_ = nullptr;
    std::optional<QByteArray> downloadError_;
    std::string downloadingId_;
    QString partPath_;
    QString finalPath_;
    qint64 received_ = 0;
    qint64 total_ = 0;
};

} // namespace superior::ui
