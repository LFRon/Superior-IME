// SPDX-License-Identifier: GPL-3.0-or-later
#include "SpeechRecognitionPanel.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDir>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QLabel>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPalette>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyledItemDelegate>
#include <QSignalBlocker>
#include <QUrl>

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
               "QLabel { color: %1; background: transparent; }")
        .arg(theme.text.name(QColor::HexArgb), theme.comment.name(QColor::HexArgb),
             theme.selection.name(QColor::HexArgb), theme.selectionText.name(QColor::HexArgb));
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

    partial_ = new QLabel(this);
    partial_->setAlignment(Qt::AlignHCenter);
    partial_->setWordWrap(true);

    model_ = new QComboBox(this);
    for (const auto &entry : speech_recognition::modelCatalog()) {
        model_->addItem(QString::fromStdString(entry.displayName),
                        QString::fromStdString(entry.id));
    }
    popupDelegate_ = new PopupDelegate(theme_, model_);
    model_->setItemDelegate(popupDelegate_);
    model_->view()->installEventFilter(this);
    applyPopupPalette();

    primary_ = new QPushButton(QStringLiteral("开始说话"), this);
    primary_->setObjectName(QStringLiteral("primary"));
    discard_ = new QPushButton(QStringLiteral("丢弃"), this);
    close_ = new QPushButton(QStringLiteral("✕"), this);
    close_->setObjectName(QStringLiteral("close"));
    download_ = new QPushButton(QStringLiteral("下载模型"), this);

    connect(close_, &QPushButton::clicked, this, [this] {
        if (command_) {
            command_(api::ToggleStatusItem{"speech"});
        }
    });
    connect(primary_, &QPushButton::clicked, this, [this] {
        if (!command_) {
            return;
        }
        if (scene_.phase == api::SpeechPhase::Recording) {
            command_(api::SpeechStop{session_, true});
        } else {
            command_(api::SpeechStart{session_, std::string{}, std::string{}});
        }
    });
    connect(discard_, &QPushButton::clicked, this, [this] {
        if (command_) {
            command_(api::SpeechCancel{session_});
        }
    });
    connect(model_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0 || reply_ != nullptr) {
            return;
        }
        const std::string id = model_->currentData().toString().toStdString();
        if (id.empty() || id == scene_.modelId) {
            return;
        }
        saveModelChoice(id);
    });
    connect(download_, &QPushButton::clicked, this, [this] {
        if (reply_ != nullptr) {
            cancelDownload();
        } else {
            beginDownload();
        }
    });

    // Real layouts, not hand-computed geometry: a translated sentence wraps to a
    // different number of lines, and a missing row must collapse instead of
    // leaving a hole. Both are ordinary layout work and were exactly what broke
    // when the positions were computed by hand.
    mic_ = new QWidget(this);
    mic_->setFixedHeight(kMicDiameter + 12);
    mic_->setAttribute(Qt::WA_TransparentForMouseEvents);

    auto *header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->addWidget(title_, 1);
    header->addWidget(close_, 0);

    auto *setupRow = new QHBoxLayout();
    setupRow->setContentsMargins(0, 0, 0, 0);
    setupRow->addWidget(model_, 1);
    setupRow->addWidget(download_, 0);

    auto *actionRow = new QHBoxLayout();
    actionRow->setContentsMargins(0, 0, 0, 0);
    actionRow->addWidget(discard_, 0);
    actionRow->addStretch(1);
    actionRow->addWidget(primary_, 0);

    root_ = new QVBoxLayout(this);
    root_->setContentsMargins(kMargin, 10, kMargin, kMargin);
    root_->setSpacing(8);
    root_->addLayout(header);
    root_->addWidget(mic_);
    root_->addWidget(message_);
    root_->addWidget(partial_);
    root_->addLayout(setupRow);
    root_->addLayout(actionRow);

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
        hide();
        return;
    }

    title_->setText(scene.modelName.empty()
                        ? QStringLiteral("语音输入")
                        : QStringLiteral("语音输入 · %1").arg(QString::fromStdString(scene.modelName)));
    message_->setText(QString::fromStdString(scene.message));
    level_ = scene.phase == api::SpeechPhase::Recording ? scene.level : 0.0;
    // `partial` is the recogniser's draft: it may be revised downwards while the
    // user keeps talking, so it is replaced wholesale rather than appended.
    partial_->setText(QString::fromStdString(scene.partial));
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
        if (index >= 0 && index != model_->currentIndex() && reply_ == nullptr) {
            QSignalBlocker blocker(model_);
            model_->setCurrentIndex(index);
        }
    }

    updateActions();
    Q_UNUSED(wasSetup);
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
    message_->setVisible(setup || scene_.phase == api::SpeechPhase::Failed ||
                         !scene_.message.empty());
    partial_->setVisible(recording || transcribing);

    switch (scene_.phase) {
    case api::SpeechPhase::NeedsSetup:
        // Requested behaviour: the big button stays disabled until the model is
        // there; the download row below is the only action that can help.
        primary_->setText(QStringLiteral("开始说话"));
        primary_->setEnabled(false);
        download_->setEnabled(true);
        download_->setText(reply_ != nullptr ? QStringLiteral("取消下载")
                                            : QStringLiteral("下载模型"));
        break;
    case api::SpeechPhase::Ready:
        primary_->setText(QStringLiteral("开始说话"));
        primary_->setEnabled(true);
        break;
    case api::SpeechPhase::Recording:
        primary_->setText(QStringLiteral("✓ 完成"));
        primary_->setEnabled(true);
        break;
    case api::SpeechPhase::Transcribing:
        primary_->setText(QStringLiteral("识别中…"));
        primary_->setEnabled(false);
        break;
    case api::SpeechPhase::Cancelling:
        primary_->setText(QStringLiteral("正在取消…"));
        primary_->setEnabled(false);
        break;
    case api::SpeechPhase::Failed:
        primary_->setText(QStringLiteral("重试"));
        primary_->setEnabled(true);
        break;
    case api::SpeechPhase::Hidden:
        break;
    }

    discard_->setVisible(recording || transcribing);
    discard_->setEnabled(recording || transcribing);
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

    // Microphone: a filled capsule, an arc handle and a stem. Drawn instead of
    // loaded so the panel needs no SVG dependency and stays crisp at any scale.
    const QRectF band = mic_ != nullptr ? QRectF(mic_->geometry()) : QRectF();
    const QPointF center(band.center());
    const double bodyWidth = 12.0;
    const double bodyHeight = 20.0;
    const bool active = scene_.phase == api::SpeechPhase::Recording;
    QColor micColor = theme_.text;
    if (setupPhase()) {
        micColor = theme_.comment;
    } else if (active) {
        micColor = theme_.firstText;
    }

    painter.save();
    painter.translate(center);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(micColor.red(), micColor.green(), micColor.blue(), active ? 46 : 28));
    painter.drawEllipse(QPointF(0, 0), kMicDiameter / 2.0, kMicDiameter / 2.0);

    painter.setBrush(micColor);
    painter.drawRoundedRect(QRectF(-bodyWidth / 2.0, -bodyHeight / 2.0 - 2, bodyWidth, bodyHeight),
                            bodyWidth / 2.0, bodyWidth / 2.0);

    painter.setBrush(Qt::NoBrush);
    QPen pen(micColor, 2.0, Qt::SolidLine, Qt::RoundCap);
    painter.setPen(pen);
    painter.drawArc(QRectF(-9, -6, 18, 16), 200 * 16, 140 * 16);
    painter.drawLine(QPointF(0, 10), QPointF(0, 14));
    painter.drawLine(QPointF(-4, 14), QPointF(4, 14));

    painter.restore();

    if (scene_.phase == api::SpeechPhase::Recording) {
        // The ring is the only feedback that the microphone is actually being
        // read, so its arc length tracks the measured level instead of being a
        // decorative constant. A floor keeps it visible during quiet passages.
        const double level = std::clamp(level_, 0.0, 1.0);
        const double grow = 2.0 + 6.0 * level;
        const QRectF ring(center.x() - kMicDiameter / 2.0 - grow,
                          center.y() - kMicDiameter / 2.0 - grow, kMicDiameter + 2.0 * grow,
                          kMicDiameter + 2.0 * grow);
        const int span = static_cast<int>((30.0 + 300.0 * level) * 16);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(micColor.red(), micColor.green(), micColor.blue(), 60), 2.0));
        painter.drawEllipse(ring);
        painter.setPen(QPen(theme_.firstText, 2.0));
        painter.drawArc(ring, 90 * 16, span);
    }
}

void SpeechRecognitionPanel::beginDownload() {
    if (reply_ != nullptr) {
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

    const QString url = QStringLiteral("https://hf-mirror.com/%1/resolve/main/%2")
                            .arg(QString::fromStdString(entry->repo),
                                 QString::fromStdString(entry->file));
    QNetworkRequest request{QUrl(url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    network_ = new QNetworkAccessManager(this);
    reply_ = network_->get(request);
    received_ = 0;
    total_ = 0;
    downloadingId_ = entry->id;

    connect(reply_, &QNetworkReply::readyRead, this, [this] {
        if (reply_ == nullptr) {
            return;
        }
        const QByteArray chunk = reply_->readAll();
        QFile file(partPath_);
        if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            file.write(chunk);
        }
        received_ += chunk.size();
        if (total_ <= 0) {
            total_ = reply_->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        }
        message_->setText(total_ > 0
                              ? QStringLiteral("正在下载模型… %1%")
                                    .arg(static_cast<int>(received_ * 100 / total_))
                              : QStringLiteral("正在下载模型…"));
        updateActions();
    });
    connect(reply_, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                received_ = received;
                total_ = total;
            });
    connect(reply_, &QNetworkReply::finished, this, [this] { finishDownload(); });
}

void SpeechRecognitionPanel::cancelDownload() {
    if (reply_ == nullptr) {
        return;
    }
    reply_->abort();
}

void SpeechRecognitionPanel::finishDownload() {
    if (reply_ == nullptr) {
        return;
    }
    const QNetworkReply::NetworkError error = reply_->error();
    const bool ok = error == QNetworkReply::NoError;
    const bool cancelled = error == QNetworkReply::OperationCanceledError;
    reply_->deleteLater();
    reply_ = nullptr;

    if (!ok) {
        QFile::remove(partPath_);
        message_->setText(cancelled ? QStringLiteral("已取消下载。")
                                    : QStringLiteral("下载失败，请重试。"));
        updateActions();
        return;
    }

    QFile::remove(finalPath_);
    if (!QFile::rename(partPath_, finalPath_)) {
        message_->setText(QStringLiteral("无法保存模型文件。"));
        updateActions();
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
                              ? QStringLiteral("下载不完整，请重试。")
                              : QStringLiteral("下载到的不是模型文件，请换个镜像重试。"));
        updateActions();
        return;
    }

    message_->setText(QStringLiteral("模型已就绪。"));
    updateActions();
    if (command_) {
        command_(api::SpeechReload{});
    }
}

} // namespace superior::ui
