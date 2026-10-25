// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QColor>
#include <QPixmap>
#include <QPoint>
#include <QString>
#include <QWidget>

#include <functional>
#include <map>
#include <optional>
#include <string>

#include "superior/api/Commands.hpp"
#include "superior/api/Scene.hpp"

#include "LayoutRules.hpp"
#include "ModelDownload.hpp"
#include "SkinTheme.hpp"

class QComboBox;
class QEvent;
class QLabel;
class QProgressBar;
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
    /// True when clicking the microphone has an effect: start, stop, or retry.
    [[nodiscard]] bool micActionable() const;
    /// One line under the microphone. The panel owns this text because the state
    /// machine only knows about setup and failures.
    [[nodiscard]] QString hintText() const;
    QPixmap iconPixmap(const QString &resource, const QColor &color, int size);
    /// Resizes to the layout's preferred height for the current width and
    /// re-anchors. Called whenever a row appears or disappears.
    void relayout();
    void reposition();
    /// Cancel a custom position and go back to the anchor above the toolbar.
    void resetPosition();
    /// Skin colours for the model popup, which is a separate window.
    void applyPopupPalette();
    void updateActions();
    /// 60 Hz clock for the microphone ring. Stopped in every other phase, so a
    /// panel that is merely open costs nothing.
    void startAnimation();
    void stopAnimation();
    void tickAnimation();
    void beginDownload();
    void cancelDownload();
    void finishDownload();
    /// Samples the reply's counters on a timer instead of per chunk: a fast
    /// mirror delivers thousands of chunks per second, and touching widgets (or
    /// opening the file) that often is what made the UI stop responding.
    void updateDownloadProgress();
    void stopProgressTimer();
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
    /// Level after attack/release smoothing: what the ring actually draws.
    double smoothedLevel_ = 0.0;
    /// Animation phase in radians: drives the breathing halo.
    double pulsePhase_ = 0.0;
    QTimer *animationTimer_ = nullptr;

    /// Model the user just picked, until a scene confirms it. Scenes are
    /// snapshots: a frame that was already in flight carries the previous model
    /// and must not undo the click.
    std::optional<std::string> pendingModelChoice_;
    /// Position chosen by dragging; empty means "use the anchor".
    std::optional<Point> customPosition_;
    bool dragging_ = false;
    /// Grab point inside the panel, so the window does not jump on press.
    QPoint dragOffset_;

    QVBoxLayout *root_ = nullptr;
    /// Spacer the microphone is painted into. It also takes clicks, which is why
    /// it is not transparent to the mouse: the microphone *is* the button.
    QWidget *mic_ = nullptr;
    QLabel *title_ = nullptr;
    QLabel *message_ = nullptr;
    QComboBox *model_ = nullptr;
    PopupDelegate *popupDelegate_ = nullptr;
    QProgressBar *progress_ = nullptr;
    QPushButton *discard_ = nullptr;
    QPushButton *close_ = nullptr;
    QPushButton *download_ = nullptr;

    /// Off-thread transfer: the UI thread must never wait for TLS setup or for a
    /// 600 MB body.
    ModelDownloader downloader_;
    bool downloaded_ = false;
    bool cancelled_ = false;
    QTimer *progressTimer_ = nullptr;
    std::string downloadingId_;
    QString partPath_;
    QString finalPath_;
    qint64 received_ = 0;
    qint64 total_ = 0;
    /// `45% · 12 MiB / 123 MiB`, shown in the message line while a transfer runs.
    /// The bar itself is 9 px tall and therefore cannot carry text.
    QString downloadHint_;
    /// Rendered icons, keyed by resource + colour + size. Re-rasterising the SVG
    /// on every repaint would be wasteful while the level ring animates.
    std::map<std::string, QPixmap> iconCache_;
};

} // namespace superior::ui
