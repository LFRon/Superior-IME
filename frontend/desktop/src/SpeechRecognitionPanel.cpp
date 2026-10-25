// SPDX-License-Identifier: GPL-3.0-or-later
#include "SpeechRecognitionPanel.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPainterPath>
#include <QPaintEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyledItemDelegate>
#include <QSignalBlocker>
#include <QTimer>
#include <QUrl>

#ifdef SUPERIOR_HAVE_QT_SVG
#include <QSvgRenderer>
#endif

#include <algorithm>
#include <cmath>

#include "Overlay.hpp"
#include "OverlayLayout.hpp"

#include "superior/config/Settings.hpp"
#include "superior/diagnostics/Log.hpp"
#include "superior/speech_recognition/Layout.hpp"
#include "superior/speech_recognition/Models.hpp"

namespace superior::ui {

namespace diag = ::superior::diagnostics;

namespace {

constexpr int kMargin = 14;
constexpr int kHeaderHeight = 30;
constexpr int kMicDiameter = 60;

QString styleFor(const SkinTheme &theme) {
    // The panel uses the same palette as the candidate window, so a skin change
    // is visible everywhere instead of only in one surface.
    return QStringLiteral(
               "QPushButton { background: rgba(255,255,255,18); color: %1;"
               " border: 1px solid rgba(255,255,255,40); border-radius: 8px;"
               " padding: 5px 12px; }"
               "QPushButton:hover { background: rgba(255,255,255,34); }"
               "QPushButton:disabled { color: %2; border-color: rgba(255,255,255,20); }"
               "QPushButton#primary { background: %3; color: %4; border: none; font-weight: 600; }"
               "QPushButton#primary:disabled { background: rgba(255,255,255,26); color: %2; }"
               "QPushButton#close { border: none; background: transparent; font-size: 16px; }"
               "QComboBox { background: rgba(255,255,255,14); color: %1;"
               " border: 1px solid rgba(255,255,255,40); border-radius: 6px; padding: 3px 8px; }"
               // Row colours are *not* set here. A stylesheet `color` on
               // QAbstractItemView does not reach the items: QComboBox draws
               // them with the view's palette, so the popup stayed black-on-dark.
               // The palette is set in applyPopupPalette() instead, where it
               // actually takes effect.
               "QComboBox::drop-down { border: none; width: 18px; }"
               // 9 px tall: a 6 px corner radius would round it into a pill.
               "QProgressBar { background: rgba(255,255,255,14); border: none;"
               " border-radius: 4px; }"
               "QProgressBar::chunk { background: %3; border-radius: 3px; }"
               "QLabel { color: %1; background: transparent; }")
        .arg(theme.text.name(QColor::HexArgb), theme.comment.name(QColor::HexArgb),
             theme.selection.name(QColor::HexArgb), theme.selectionText.name(QColor::HexArgb));
}

/// Last resort when Qt was built without SVG: the same microphone the toolbar
/// draws, plus a slash when listening is not possible.
void paintFallbackMic(QPainter &painter, const QPointF &center, const QColor &color, bool available,
                      bool active) {
    const double bodyWidth = 12.0;
    const double bodyHeight = 20.0;

    painter.save();
    painter.translate(center);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(-bodyWidth / 2.0, -bodyHeight / 2.0 - 2, bodyWidth, bodyHeight),
                            bodyWidth / 2.0, bodyWidth / 2.0);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(QRectF(-9, -6, 18, 16), 200 * 16, 140 * 16);
    painter.drawLine(QPointF(0, 10), QPointF(0, 14));
    painter.drawLine(QPointF(-4, 14), QPointF(4, 14));

    if (!available) {
        // Same slash as the Tabler icon, at the same angle.
        painter.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(-10, -10), QPointF(10, 10));
    }
    painter.restore();
    Q_UNUSED(active);
}

} // namespace

/// Draws the model list with the skin's colours.
///
/// The list is a window of its own, and neither a palette nor a stylesheet set
/// on it reached the rows: the system light style kept painting black text on
/// the dark popup. Colours are therefore forced at draw time, which is the one
/// place that cannot be overridden.
class PopupDelegate : public QStyledItemDelegate {
public:
    PopupDelegate(const SkinTheme &theme, QComboBox *combo)
        : QStyledItemDelegate(combo), theme_(theme), combo_(combo) {}

    void setTheme(const SkinTheme &theme) { theme_ = theme; }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        const bool selected = option.state & QStyle::State_Selected;
        painter->save();
        painter->fillRect(option.rect, theme_.surface);

        QRect row = option.rect.adjusted(2, 1, -2, -1);
        QPainterPath shape;
        shape.addRoundedRect(QRectF(row), 5, 5);
        if (selected) {
            painter->fillPath(shape, theme_.selection);
        }

        int textLeft = row.left() + 10;
        // The current row is marked here because the style's check indicator is
        // drawn with the system palette too, i.e. invisible on a dark popup.
        const bool current = combo_ != nullptr && combo_->model() != nullptr &&
                             index == combo_->model()->index(combo_->currentIndex(), 0);
        if (current) {
            painter->setPen(selected ? theme_.selectionText : theme_.text);
            QRect tick{textLeft, row.top(), 14, row.height()};
            painter->drawText(tick, Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("✓"));
        }
        textLeft += 18;

        painter->setPen(selected ? theme_.selectionText : theme_.text);
        const QRect textRect{textLeft, row.top(), std::max(0, row.right() - textLeft - 8),
                             row.height()};
        const QString text = option.fontMetrics.elidedText(
            index.data(Qt::DisplayRole).toString(), Qt::ElideRight, textRect.width());
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text);
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(std::max(size.height(), option.fontMetrics.height() + 12));
        return size;
    }

private:
    SkinTheme theme_;
    QComboBox *combo_ = nullptr;
};

SpeechRecognitionPanel::SpeechRecognitionPanel(QWidget *parent) : QWidget(parent) {
    makeOverlay(*this);
    setWindowTitle(QStringLiteral("Superior-IME Speech"));
    setStyleSheet(styleFor(theme_));

    title_ = new QLabel(QStringLiteral("语音输入"), this);
    QFont titleFont = title_->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1.0);
    titleFont.setBold(true);
    title_->setFont(titleFont);

    message_ = new QLabel(this);
    message_->setWordWrap(true);
    message_->setAlignment(Qt::AlignHCenter | Qt::AlignTop);

    model_ = new QComboBox(this);
    for (const auto &entry : speech_recognition::modelCatalog()) {
        model_->addItem(QString::fromStdString(entry.displayName),
                        QString::fromStdString(entry.id));
    }
    popupDelegate_ = new PopupDelegate(theme_, model_);
    model_->setItemDelegate(popupDelegate_);
    model_->view()->installEventFilter(this);
    applyPopupPalette();

    discard_ = new QPushButton(QStringLiteral("丢弃"), this);
    close_ = new QPushButton(QStringLiteral("✕"), this);
    close_->setObjectName(QStringLiteral("close"));
    download_ = new QPushButton(QStringLiteral("下载模型"), this);

    connect(close_, &QPushButton::clicked, this, [this] {
        if (command_) {
            command_(api::ToggleStatusItem{"speech"});
        }
    });
    connect(discard_, &QPushButton::clicked, this, [this] {
        if (command_) {
            command_(api::SpeechCancel{session_});
        }
    });
    connect(model_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0 || downloader_.running()) {
            return;
        }
        const std::string id = model_->currentData().toString().toStdString();
        if (id.empty() || id == scene_.modelId) {
            return;
        }
        saveModelChoice(id);
    });
    connect(download_, &QPushButton::clicked, this, [this] {
        if (downloader_.running()) {
            cancelDownload();
        } else {
            beginDownload();
        }
    });

    // Hops back from the worker thread. The counters are only stored here; the
    // bar is repainted by `progressTimer_`, so a fast mirror cannot turn into a
    // repaint storm.
    downloader_.setProgressHandler([this](qint64 received, qint64 total) {
        received_ = received;
        total_ = total;
    });
    downloader_.setFinishedHandler([this](ModelDownloader::Status status) {
        downloaded_ = status == ModelDownloader::Status::Ok;
        cancelled_ = status == ModelDownloader::Status::Cancelled;
        finishDownload();
    });

    // Real layouts, not hand-computed geometry: a translated sentence wraps to a
    // different number of lines, and a missing row must collapse instead of
    // leaving a hole. Both are ordinary layout work and were exactly what broke
    // when the positions were computed by hand.
    mic_ = new QWidget(this);
    mic_->setFixedHeight(kMicDiameter + 12);
    // Deliberately *not* transparent to the mouse: the microphone replaced the
    // "开始说话" button, so it must receive the click. The rest of the panel still
    // drags the window; a button that also drags is a surprise, not a feature.
    mic_->installEventFilter(this);

    auto *header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->addWidget(title_, 1);
    header->addWidget(close_, 0);

    auto *setupRow = new QHBoxLayout();
    setupRow->setContentsMargins(0, 0, 0, 0);
    setupRow->addWidget(model_, 1);
    setupRow->addWidget(download_, 0);

    progress_ = new QProgressBar(this);
    progress_->setRange(0, 100);
    progress_->setValue(0);
    // A hairline, not a block: the bar only answers "how far along is it", and
    // the numbers moved into the message line above it (`downloadHint_`) because
    // 9 px cannot hold a caption.
    progress_->setTextVisible(false);
    progress_->setFixedHeight(9);
    progress_->hide();

    root_ = new QVBoxLayout(this);
    root_->setContentsMargins(kMargin, 10, kMargin, kMargin);
    root_->setSpacing(8);
    root_->addLayout(header);
    root_->addWidget(mic_);
    root_->addWidget(message_);
    root_->addLayout(setupRow);
    // Directly under the model choice, spanning the panel: the bar reports on the
    // row above it and nothing else.
    root_->addWidget(progress_);
    root_->addWidget(discard_, 0, Qt::AlignLeft);

    progressTimer_ = new QTimer(this);
    progressTimer_->setInterval(120);
    connect(progressTimer_, &QTimer::timeout, this, [this] { updateDownloadProgress(); });

    animationTimer_ = new QTimer(this);
    animationTimer_->setInterval(16); // 60 fps
    connect(animationTimer_, &QTimer::timeout, this, [this] { tickAnimation(); });

    setFixedWidth(speechPanelWidth(1.0));
    updateActions();
    relayout();
}

void SpeechRecognitionPanel::setCommandHandler(CommandHandler handler) {
    command_ = std::move(handler);
}

void SpeechRecognitionPanel::setScale(double scale) {
    scale_ = clampScale(scale);
    setFixedWidth(speechPanelWidth(scale_));
    relayout();
}

void SpeechRecognitionPanel::setScreen(const Rect &available) {
    screen_ = available;
    reposition();
}

void SpeechRecognitionPanel::setStatusBarRect(const Rect &rect) {
    statusBar_ = rect;
    reposition();
}

bool SpeechRecognitionPanel::setupPhase() const {
    return scene_.phase == api::SpeechPhase::NeedsSetup;
}

QSize SpeechRecognitionPanel::sizeHint() const {
    const int height = root_ != nullptr ? root_->sizeHint().height() : 0;
    return QSize(speechPanelWidth(scale_), height);
}

void SpeechRecognitionPanel::relayout() {
    if (root_ == nullptr) {
        return;
    }
    // `activate()` first: a row that just became visible only contributes its
    // real height after the layout has seen it.
    root_->activate();
    const int height = root_->sizeHint().height();
    if (height != this->height()) {
        resize(width(), height);
    }
    reposition();
}

Rect SpeechRecognitionPanel::occupiedRect() const {
    if (!scene_.visible) {
        return Rect{};
    }
    return Rect{x(), y(), width(), height()};
}

void SpeechRecognitionPanel::reposition() {
    if (screen_.width <= 0) {
        return;
    }
    // The actual size, not a predicted one: the panel's height depends on how
    // many lines the current message needs.
    const Size size{width(), height()};
    // A dragged position is a user decision, so it outranks the anchor. Only
    // clamping still applies: the window must stay reachable.
    const Rect placed = customPosition_ ? clampToScreen(screen_, size, *customPosition_, kOverlayMargin)
                                        : placeSpeechPanel(screen_, statusBar_, size, kOverlayMargin);
    move(placed.left, placed.top);
    if (customPosition_) {
        customPosition_ = Point{placed.left, placed.top};
    }
}

void SpeechRecognitionPanel::resetPosition() {
    customPosition_.reset();
    reposition();
}

void SpeechRecognitionPanel::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    dragging_ = true;
    dragOffset_ = event->position().toPoint();
    event->accept();
}

void SpeechRecognitionPanel::mouseMoveEvent(QMouseEvent *event) {
    if (!dragging_ || !(event->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const QPoint target = event->globalPosition().toPoint() - dragOffset_;
    customPosition_ = Point{target.x(), target.y()};
    reposition();
    event->accept();
}

void SpeechRecognitionPanel::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

/// Double-click resets: without it, a panel dragged into a corner would have no
/// way back to "above the toolbar" short of restarting the input method.
void SpeechRecognitionPanel::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    resetPosition();
    event->accept();
}

/// The popup is a top-level window with the system palette, so its *frame* was
/// light even after the rows were fixed. It does not exist until the list is
/// opened, hence a fix-up on every show rather than one in the constructor.
bool SpeechRecognitionPanel::eventFilter(QObject *watched, QEvent *event) {
    // The microphone is the button. Clicks are taken here instead of by the
    // panel so they cannot start a window drag on the way.
    if (mic_ != nullptr && watched == mic_) {
        if (event->type() == QEvent::MouseButtonRelease) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton && micActionable() &&
                mic_->rect().contains(mouse->position().toPoint()) && command_) {
                if (scene_.phase == api::SpeechPhase::Recording) {
                    command_(api::SpeechStop{session_, true});
                } else {
                    command_(api::SpeechStart{session_, std::string{}, std::string{}});
                }
                return true;
            }
        }
        if (event->type() == QEvent::MouseButtonPress) {
            // Swallow the press as well, otherwise the panel would see it and
            // treat a click on the microphone as the start of a drag.
            return true;
        }
    }
    if (event->type() == QEvent::Show && model_ != nullptr && watched == model_->view()) {
        QWidget *popup = model_->view()->window();
        if (popup != nullptr && popup != this) {
            QPalette palette = popup->palette();
            for (const QPalette::ColorRole role :
                 {QPalette::Window, QPalette::Base, QPalette::Button, QPalette::ToolTipBase}) {
                palette.setColor(role, theme_.surface);
            }
            for (const QPalette::ColorRole role :
                 {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
                palette.setColor(role, theme_.text);
            }
            for (const QPalette::ColorRole role : {QPalette::Light, QPalette::Midlight,
                                                   QPalette::Dark, QPalette::Mid, QPalette::Shadow}) {
                palette.setColor(role, theme_.border);
            }
            popup->setPalette(palette);
        }
    }
    return QWidget::eventFilter(watched, event);
}

/// Paints the model list with the skin's colours.
///
/// The list lives in its own window with its own palette, so the panel's
/// stylesheet does not colour it; without this the popup was the system light
/// palette: white window, black items, and unreadable selected rows.
void SpeechRecognitionPanel::applyPopupPalette() {
    QAbstractItemView *view = model_ != nullptr ? model_->view() : nullptr;
    if (view == nullptr) {
        return;
    }
    QPalette palette = view->palette();
    palette.setColor(QPalette::Base, theme_.surface);
    palette.setColor(QPalette::Window, theme_.surface);
    palette.setColor(QPalette::Button, theme_.surface);
    palette.setColor(QPalette::Text, theme_.text);
    palette.setColor(QPalette::WindowText, theme_.text);
    palette.setColor(QPalette::ButtonText, theme_.text);
    palette.setColor(QPalette::Highlight, theme_.selection);
    palette.setColor(QPalette::HighlightedText, theme_.selectionText);
    view->setPalette(palette);
    view->viewport()->setPalette(palette);
    view->setAutoFillBackground(true);
}

void SpeechRecognitionPanel::saveModelChoice(const std::string &id) {
    const std::filesystem::path path = superior::config::defaultSettingsPath();
    auto loaded = superior::config::loadSettings(path);
    loaded.settings.speechRecognitionModelId = id;
    std::string error;
    if (!superior::config::saveSettings(path, loaded.settings, error)) {
        SUPERIOR_WARN(diag::categories::SpeechRecognition)
            .field("error", error)
            << "cannot save the speech model choice";
        return;
    }
    // Optimistic on purpose: the combo already shows the new entry, and the
    // command below makes the agent re-read the file and answer with the same
    // id. Without this the answer to "which model?" was still the old one for
    // one round trip, so the first click looked like it did nothing.
    pendingModelChoice_ = id;
    if (command_) {
        command_(api::SpeechReload{});
    }
}

void SpeechRecognitionPanel::apply(const api::SpeechScene &scene,
                                   const api::SessionKey &session, const SkinTheme &theme) {
    const bool wasSetup = setupPhase();
    scene_ = scene;
    session_ = session;
    if (theme.text != theme_.text || theme_.selection != theme.selection) {
        theme_ = theme;
        setStyleSheet(styleFor(theme_));
        if (popupDelegate_ != nullptr) {
            popupDelegate_->setTheme(theme_);
        }
        applyPopupPalette();
    }

    if (!scene.visible) {
        pendingModelChoice_.reset();
        stopAnimation();
        hide();
        return;
    }

    title_->setText(scene.modelName.empty()
                        ? QStringLiteral("语音输入")
                        : QStringLiteral("语音输入 · %1").arg(QString::fromStdString(scene.modelName)));
    level_ = scene.phase == api::SpeechPhase::Recording ? scene.level : 0.0;
    // Follow the scene, but not *against* the user: a frame can still be in
    // flight from before the choice was saved. The pending id is dropped as
    // soon as the agent echoes it back, and on close, so a choice the agent
    // refused cannot stick forever.
    if (pendingModelChoice_.has_value() && !scene.modelId.empty() &&
        scene.modelId == *pendingModelChoice_) {
        pendingModelChoice_.reset();
    }
    if (!scene.modelId.empty() && !pendingModelChoice_.has_value()) {
        const int index = model_->findData(QString::fromStdString(scene.modelId));
        if (index >= 0 && index != model_->currentIndex() && !downloader_.running()) {
            QSignalBlocker blocker(model_);
            model_->setCurrentIndex(index);
        }
    }

    updateActions();
    Q_UNUSED(wasSetup);
    // The animation exists only while the microphone is live: an idle panel must
    // not hold a 60 Hz timer.
    if (scene.phase == api::SpeechPhase::Recording) {
        startAnimation();
    } else {
        stopAnimation();
    }
    relayout();
    update();
    if (!isVisible()) {
        show();
        raise();
    }
}

void SpeechRecognitionPanel::updateActions() {
    const bool setup = setupPhase();
    const bool recording = scene_.phase == api::SpeechPhase::Recording;
    const bool transcribing = scene_.phase == api::SpeechPhase::Transcribing;

    // Switching the model is not a setup-only action: a user with 0.6B installed
    // must be able to move to 1.7B without deleting files. Picking a model that
    // is not downloaded simply lands back in NeedsSetup, with the download
    // button already visible.
    model_->setVisible(setup || scene_.phase == api::SpeechPhase::Ready);
    download_->setVisible(setup);
    download_->setEnabled(true);
    download_->setText(downloader_.running() ? QStringLiteral("取消下载") : QStringLiteral("下载模型"));
    progress_->setVisible(downloader_.running());
    message_->setVisible(!hintText().isEmpty());
    message_->setText(hintText());

    // The microphone is the button. The cursor says so before the click does.
    if (mic_ != nullptr) {
        mic_->setCursor(micActionable() ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
    discard_->setVisible(recording || transcribing);
    discard_->setEnabled(recording || transcribing);
}

bool SpeechRecognitionPanel::micActionable() const {
    switch (scene_.phase) {
    case api::SpeechPhase::Ready:
    case api::SpeechPhase::Failed:
    case api::SpeechPhase::Recording:
        // Recording counts: the same spot stops it. Everywhere else the toggle
        // would either be a no-op or race the capability process.
        return !downloader_.running();
    case api::SpeechPhase::NeedsSetup:
    case api::SpeechPhase::Transcribing:
    case api::SpeechPhase::Cancelling:
    case api::SpeechPhase::Hidden:
        return false;
    }
    return false;
}

QString SpeechRecognitionPanel::hintText() const {
    // While a model is downloading, this line is the progress read-out: the bar
    // is deliberately a hairline with no room for text.
    if (downloader_.running() && !downloadHint_.isEmpty()) {
        return downloadHint_;
    }
    // The state machine writes the setup and failure sentences; the rest is pure
    // presentation and stays here, so "which words does the panel use" has one
    // answer per state.
    if (!scene_.message.empty()) {
        return QString::fromStdString(scene_.message);
    }
    switch (scene_.phase) {
    case api::SpeechPhase::Ready:
        return QStringLiteral("点击麦克风开始说话");
    case api::SpeechPhase::Recording:
        // A cap that arrives unannounced is a bug from the user's point of view,
        // so the last seconds are counted down in the same line that explains how
        // to stop. Above ten seconds the line stays quiet.
        if (scene_.remainingMs >= 0 && scene_.remainingMs <= 10000) {
            const int seconds = static_cast<int>((scene_.remainingMs + 999) / 1000);
            return QStringLiteral("正在聆听…还可说 %1 秒，之后自动停止（再点麦克风可提前结束）")
                .arg(seconds);
        }
        return QStringLiteral("正在聆听…再次点击麦克风结束");
    case api::SpeechPhase::Transcribing:
        return QStringLiteral("正在识别…");
    case api::SpeechPhase::Cancelling:
        return QStringLiteral("正在取消…");
    case api::SpeechPhase::Failed:
    case api::SpeechPhase::NeedsSetup:
    case api::SpeechPhase::Hidden:
        return {};
    }
    return {};
}

void SpeechRecognitionPanel::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF card(0.5, 0.5, width() - 1.0, height() - 1.0);
    QPainterPath path;
    path.addRoundedRect(card, theme_.radius + 2, theme_.radius + 2);
    painter.fillPath(path, theme_.surface);
    painter.setPen(QPen(theme_.border, 1.0));
    painter.drawPath(path);

    const QRectF band = mic_ != nullptr ? QRectF(mic_->geometry()) : QRectF();
    const QPointF center(band.center());
    const bool active = scene_.phase == api::SpeechPhase::Recording;
    const bool available = micActionable();
    QColor micColor = theme_.text;
    if (!available) {
        micColor = theme_.comment;
    } else if (active) {
        micColor = theme_.firstText;
    }

    painter.save();
    painter.translate(center);
    painter.setPen(Qt::NoPen);
    // While listening the disc brightens with the voice, which is what makes the
    // animation read as "hearing you" rather than as a static badge.
    const int discAlpha = active ? static_cast<int>(34 + 52 * smoothedLevel_) : 28;
    painter.setBrush(QColor(micColor.red(), micColor.green(), micColor.blue(), discAlpha));
    painter.drawEllipse(QPointF(0, 0), kMicDiameter / 2.0, kMicDiameter / 2.0);
    painter.restore();

    // The microphone itself comes from the two SVGs the product ships
    // (`microphone.svg`, and `tabler-microphone-off-outline.svg` for "cannot
    // listen"). Drawing them here as well keeps the panel working on a Qt build
    // without Qt6Svg, which is a supported configuration.
    const int iconSize = static_cast<int>(kMicDiameter - 18);
    const QPixmap icon = available
                             ? iconPixmap(QStringLiteral(":/icons/microphone.svg"), micColor, iconSize)
                             : iconPixmap(QStringLiteral(":/icons/tabler-microphone-off-outline.svg"),
                                          micColor, iconSize);
    if (!icon.isNull()) {
        painter.drawPixmap(QPointF(center.x() - iconSize / 2.0, center.y() - iconSize / 2.0), icon);
    } else {
        paintFallbackMic(painter, center, micColor, available, active);
    }

    if (active) {
        // Listening animation, three cheap layers:
        //   1. a full track, so the ring exists even in complete silence;
        //   2. a halo that breathes at a fixed cadence — life that does not
        //      depend on the microphone, so the panel never looks frozen when the
        //      room is quiet;
        //   3. the level arc, smoothed (see tickAnimation) so it follows
        //      syllables instead of flickering on every RMS sample.
        const double radius = kMicDiameter / 2.0 + 7.0;
        const double breath = 1.0 + 0.04 * std::sin(pulsePhase_);
        const QRectF ring(center.x() - radius * breath, center.y() - radius * breath,
                          radius * breath * 2.0, radius * breath * 2.0);

        painter.setBrush(Qt::NoBrush);
        QColor track = micColor;
        track.setAlpha(46);
        painter.setPen(QPen(track, 3.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawEllipse(ring);

        QColor halo = micColor;
        halo.setAlpha(static_cast<int>(16 + 44 * smoothedLevel_));
        painter.setPen(QPen(halo, 7.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawEllipse(ring);

        // The arc is *only* the measured level: drawn from twelve o'clock
        // clockwise. Below the threshold it is not painted at all — a rounded cap
        // at 2 degrees reads as a stray dot on the ring, which is noise, not
        // feedback. Silence still looks alive because the halo keeps breathing.
        // Speech RMS sits around 0.02–0.25, so a linear arc would barely move at
        // normal volume. The 0.6 power curve spreads that range over the whole
        // ring, which is what makes the arc readable as loudness.
        constexpr double kAudible = 0.02;
        if (smoothedLevel_ > kAudible) {
            const double span = 330.0 * std::pow(smoothedLevel_, 0.6);
            QColor arc = theme_.firstText;
            arc.setAlpha(235);
            painter.setPen(QPen(arc, 3.0, Qt::SolidLine, Qt::RoundCap));
            painter.drawArc(ring, 90 * 16, -static_cast<int>(span * 16));
        }
    }
}

QPixmap SpeechRecognitionPanel::iconPixmap(const QString &resource, const QColor &color, int size) {
    if (size <= 0) {
        return {};
    }
    const std::string key =
        resource.toStdString() + '|' + color.name(QColor::HexArgb).toStdString() + '|' +
        std::to_string(size);
    if (const auto it = iconCache_.find(key); it != iconCache_.end()) {
        return it->second;
    }

    QPixmap pixmap;
#ifdef SUPERIOR_HAVE_QT_SVG
    QSvgRenderer renderer(resource);
    if (renderer.isValid()) {
        // Tinted through a mask so the SVG's own `currentColor` never matters.
        QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter paint(&image);
        paint.setRenderHint(QPainter::Antialiasing, true);
        renderer.render(&paint);
        paint.setCompositionMode(QPainter::CompositionMode_SourceIn);
        paint.fillRect(image.rect(), color);
        paint.end();
        pixmap = QPixmap::fromImage(image);
    }
#endif
    iconCache_.emplace(key, pixmap);
    return pixmap;
}

void SpeechRecognitionPanel::startAnimation() {
    if (animationTimer_ == nullptr || animationTimer_->isActive()) {
        return;
    }
    smoothedLevel_ = 0.0;
    pulsePhase_ = 0.0;
    animationTimer_->start();
}

void SpeechRecognitionPanel::stopAnimation() {
    if (animationTimer_ == nullptr || !animationTimer_->isActive()) {
        return;
    }
    animationTimer_->stop();
    smoothedLevel_ = 0.0;
    update();
}

void SpeechRecognitionPanel::tickAnimation() {
    constexpr double kTwoPi = 6.28318530717958647692;
    if (scene_.phase == api::SpeechPhase::Recording) {
        // Attack is quick and release slow: the ring snaps to a syllable and eases
        // back. A symmetric filter would either lag behind speech or flicker.
        const double target = std::clamp(level_, 0.0, 1.0);
        const double rate = target > smoothedLevel_ ? 0.42 : 0.09;
        smoothedLevel_ += (target - smoothedLevel_) * rate;
    } else {
        smoothedLevel_ = 0.0;
    }

    // One revolution in 1.33 s: used as the breathing phase while recording and
    // as the spinner angle while the recogniser works.
    pulsePhase_ += kTwoPi * 0.75 / 60.0;
    if (pulsePhase_ > kTwoPi) {
        pulsePhase_ -= kTwoPi;
    }

    // Only the microphone area changes, so only that area is repainted. The whole
    // panel at 60 Hz would be visible in the X server's CPU use for no reason.
    if (mic_ != nullptr) {
        update(mic_->geometry().adjusted(-18, -18, 18, 18));
    } else {
        update();
    }
}

void SpeechRecognitionPanel::beginDownload() {
    if (downloader_.running()) {
        return;
    }
    const std::string id = model_->currentData().toString().toStdString();
    const speech_recognition::ModelEntry *entry = speech_recognition::findModel(id);
    if (entry == nullptr) {
        return;
    }
    const std::filesystem::path target = speech_recognition::modelPath(entry->id);
    if (target.empty()) {
        return;
    }

    QDir().mkpath(QString::fromStdString(target.parent_path().string()));
    finalPath_ = QString::fromStdString(target.string());
    partPath_ = finalPath_ + QStringLiteral(".part");
    QFile::remove(partPath_);
    downloadingId_ = entry->id;
    received_ = 0;
    total_ = 0;

    progress_->setRange(0, 0); // indeterminate until the size is known
    progress_->setValue(0);
    downloadHint_ = QStringLiteral("正在下载…");
    progressTimer_->start();
    // The transfer runs on a worker thread. See ModelDownload.hpp: Qt's TLS
    // setup is synchronous inside `get()` and walks the CA directory, which took
    // 40+ seconds here — long enough to look like a dead input method.
    downloader_.start(QUrl(QStringLiteral("https://hf-mirror.com/%1/resolve/main/%2")
                               .arg(QString::fromStdString(entry->repo),
                                    QString::fromStdString(entry->file))),
                      partPath_);
    updateActions();
    relayout();
}

void SpeechRecognitionPanel::updateDownloadProgress() {
    if (!downloader_.running()) {
        stopProgressTimer();
        return;
    }
    if (total_ > 0) {
        if (progress_->maximum() != 100) {
            progress_->setRange(0, 100);
        }
        const int percent = static_cast<int>(received_ * 100 / total_);
        progress_->setValue(percent);
        const QString caption = QStringLiteral("正在下载 %1% · %2 / %3")
                                    .arg(QString::number(percent),
                                         QLocale().formattedDataSize(received_, 1),
                                         QLocale().formattedDataSize(total_, 1));
        if (caption != downloadHint_) {
            downloadHint_ = caption;
            updateActions();
        }
    } else {
        progress_->setRange(0, 0);
        if (downloadHint_ != QStringLiteral("正在下载…")) {
            downloadHint_ = QStringLiteral("正在下载…");
            updateActions();
        }
    }
}

void SpeechRecognitionPanel::stopProgressTimer() {
    progressTimer_->stop();
    downloadHint_.clear();
    progress_->setRange(0, 100);
    progress_->setValue(0);
}

void SpeechRecognitionPanel::cancelDownload() {
    downloader_.cancel();
}

void SpeechRecognitionPanel::finishDownload() {
    stopProgressTimer();

    if (cancelled_) {
        QFile::remove(partPath_);
        message_->setText(QStringLiteral("已取消下载"));
        updateActions();
        relayout();
        return;
    }

    if (!downloaded_) {
        QFile::remove(partPath_);
        message_->setText(QStringLiteral("下载失败，请重试"));
        updateActions();
        relayout();
        return;
    }

    QFile::remove(finalPath_);
    if (!QFile::rename(partPath_, finalPath_)) {
        message_->setText(QStringLiteral("无法保存模型文件"));
        updateActions();
        relayout();
        return;
    }

    // A mirror answering with an error page or a half-written file is common
    // enough to check for: without this the failure surfaces later as an engine
    // crash that says nothing about the download.
    const speech_recognition::ModelEntry *entry =
        speech_recognition::findModel(downloadingId_);
    const auto check = speech_recognition::checkModelFile(
        std::filesystem::path(finalPath_.toStdString()), entry != nullptr ? entry->bytes : 0);
    if (check != speech_recognition::ModelFileCheck::Ok) {
        QFile::remove(finalPath_);
        message_->setText(check == speech_recognition::ModelFileCheck::TooSmall
                              ? QStringLiteral("下载不完整，请重试")
                              : QStringLiteral("下载到的不是模型文件，请换个镜像重试"));
        updateActions();
        relayout();
        return;
    }

    message_->setText(QStringLiteral("模型已就绪"));
    updateActions();
    relayout();
    if (command_) {
        command_(api::SpeechReload{});
    }
}

} // namespace superior::ui
