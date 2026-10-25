// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QPoint>
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
class QEvent;
class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;
class QTimer;
class QVBoxLayout;

namespace superior::ui {

class PopupDelegate;

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
    // Dragging: the anchor is only the *default* place. Once the user moves the
    // panel, that position is the truth until the screen shrinks or the panel is
    // re-opened in a new session.
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    /// Repaints the model popup's frame, which is a separate window.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    [[nodiscard]] bool setupPhase() const;
    /// Resizes to the layout's preferred height for the current width and
    /// re-anchors. Called whenever a row appears or disappears.
    void relayout();
    void reposition();
    /// Cancel a custom position and go back to the anchor above the toolbar.
    void resetPosition();
    /// Skin colours for the model popup, which is a separate window.
    void applyPopupPalette();
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

    /// Position chosen by dragging; empty means "use the anchor".
    std::optional<Point> customPosition_;
    bool dragging_ = false;
    /// Grab point inside the panel, so the window does not jump on press.
    QPoint dragOffset_;

    QVBoxLayout *root_ = nullptr;
    /// Empty spacer the microphone is painted into; it exists so the layout
    /// reserves exactly the space the drawing needs.
    QWidget *mic_ = nullptr;
    QLabel *title_ = nullptr;
    QLabel *message_ = nullptr;
    QLabel *partial_ = nullptr;
    QComboBox *model_ = nullptr;
    PopupDelegate *popupDelegate_ = nullptr;
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
