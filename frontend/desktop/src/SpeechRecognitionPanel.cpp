// SPDX-License-Identifier: GPL-3.0-or-later
#include "SpeechRecognitionPanel.hpp"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPushButton>
#include <QResizeEvent>
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
               "QComboBox QAbstractItemView { background: %5; color: %1; }"
               "QLabel { color: %1; background: transparent; }")
        .arg(theme.text.name(QColor::HexArgb), theme.comment.name(QColor::HexArgb),
             theme.selection.name(QColor::HexArgb), theme.selectionText.name(QColor::HexArgb),
             theme.surface.name(QColor::HexArgb));
}

} // namespace

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

    const Size initial = speechPanelSize(true, 1.0);
    resize(initial.width, initial.height);
    updateActions();
}

void SpeechRecognitionPanel::setCommandHandler(CommandHandler handler) {
    command_ = std::move(handler);
}

void SpeechRecognitionPanel::setScale(double scale) {
    scale_ = clampScale(scale);
    const Size size = speechPanelSize(setupPhase(), scale_);
    resize(size.width, size.height);
    reposition();
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
    const Size size = speechPanelSize(setupPhase(), scale_);
    return QSize(size.width, size.height);
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
    const Rect placed =
        placeSpeechPanel(screen_, statusBar_, speechPanelSize(setupPhase(), scale_), kOverlayMargin);
    move(placed.left, placed.top);
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
    }

    if (!scene.visible) {
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
    if (!scene.modelId.empty()) {
        const int index = model_->findData(QString::fromStdString(scene.modelId));
        if (index >= 0 && index != model_->currentIndex() && reply_ == nullptr) {
            QSignalBlocker blocker(model_);
            model_->setCurrentIndex(index);
        }
    }

    updateActions();
    if (wasSetup != setupPhase()) {
        const Size size = speechPanelSize(setupPhase(), scale_);
        resize(size.width, size.height);
    }
    reposition();
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

    model_->setVisible(setup);
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

void SpeechRecognitionPanel::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    const int width = this->width();
    const int height = this->height();

    close_->setGeometry(width - 34, 8, 26, 24);
    title_->setGeometry(kMargin, 6, width - 2 * kMargin - 30, kHeaderHeight - 6);

    const int micTop = kHeaderHeight + 6;
    const int controlsTop = height - 44;

    if (setupPhase()) {
        message_->setGeometry(kMargin, micTop + kMicDiameter + 6, width - 2 * kMargin, 44);
        model_->setGeometry(kMargin, message_->geometry().bottom() + 6, width - 2 * kMargin, 28);
        download_->setGeometry(kMargin, height - 42, width - 2 * kMargin, 30);
        primary_->setVisible(false);
        discard_->setVisible(false);
        partial_->setVisible(false);
    } else {
        message_->setGeometry(kMargin, micTop + kMicDiameter + 4, width - 2 * kMargin, 22);
        partial_->setGeometry(kMargin, message_->geometry().bottom() + 2, width - 2 * kMargin, 40);
        primary_->setGeometry(width - kMargin - 110, controlsTop, 110, 32);
        discard_->setGeometry(kMargin, controlsTop, 70, 32);
        primary_->setVisible(true);
        model_->setVisible(false);
        download_->setVisible(false);
    }
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
    const QPointF center(width() / 2.0, kHeaderHeight + 6 + kMicDiameter / 2.0);
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
