// SPDX-License-Identifier: GPL-3.0-or-later
#include "SettingsWindow.hpp"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QStackedWidget>
#include <QStringList>
#include <QStyle>

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>

#include "superior/diagnostics/Log.hpp"
#include "superior/theme/ClassicUiTheme.hpp"

namespace superior::ui {

namespace {

namespace fs = std::filesystem;
namespace diag = ::superior::diagnostics;

constexpr int kMinScale = 75;
constexpr int kMaxScale = 300;

QString darkStyleSheet() {
    return QStringLiteral(R"(
QWidget {
    background: #15181C;
    color: #E8EAED;
    font-family: "Noto Sans CJK SC", "Source Han Sans SC", sans-serif;
    font-size: 13px;
}
#sidebar { background: #101317; border-right: 1px solid rgba(255,255,255,0.06); }
#brand { font-size: 15px; font-weight: 600; }
#brandDot { color: #7CC4FF; font-size: 16px; }
#version { color: #6F767D; font-size: 11px; }
#pageTitle { font-size: 20px; font-weight: 600; }
#pageSubtitle { color: #9AA0A6; font-size: 12px; }
#card {
    background: #1A1D22;
    border: 1px solid rgba(255,255,255,0.07);
    border-radius: 12px;
}
#cardTitle { font-size: 14px; font-weight: 600; background: transparent; }
#cardHint { color: #9AA0A6; font-size: 11px; background: transparent; }
#rowLabel { background: transparent; }
#rowHint { color: #8A9096; font-size: 11px; background: transparent; }
#status { color: #9AA0A6; font-size: 12px; }
#statusError { color: #FF8A80; font-size: 12px; }
QLineEdit {
    background: #1E2126;
    border: 1px solid rgba(255,255,255,0.08);
    border-radius: 8px;
    padding: 6px 10px;
    selection-background-color: #2A6FDB;
}
QListWidget { background: transparent; border: none; outline: none; }
QListWidget::item { border-radius: 8px; padding: 9px 12px; margin: 2px 0; color: #C7CBD1; }
QListWidget::item:hover { background: rgba(255,255,255,0.05); }
QListWidget::item:selected { background: #2A6FDB; color: #FFFFFF; }
QComboBox {
    background: #1E2126;
    border: 1px solid rgba(255,255,255,0.08);
    border-radius: 8px;
    padding: 6px 10px;
    min-width: 170px;
}
QComboBox::drop-down { border: none; width: 22px; }
QComboBox QAbstractItemView {
    background: #1E2126;
    border: 1px solid rgba(255,255,255,0.1);
    selection-background-color: #2A6FDB;
}
QPushButton {
    background: #24282F;
    border: 1px solid rgba(255,255,255,0.08);
    border-radius: 8px;
    padding: 8px 14px;
}
QPushButton:hover { background: #2C3038; }
QPushButton#primary { background: #2A6FDB; border: none; color: #FFFFFF; font-weight: 600; }
QPushButton#primary:hover { background: #3B7FE8; }
QSlider::groove:horizontal { height: 4px; background: #2A2E35; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #2A6FDB; border-radius: 2px; }
QSlider::handle:horizontal {
    background: #E8EAED; width: 14px; margin: -6px 0; border-radius: 7px;
}
QScrollArea { border: none; background: transparent; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 4px; }
QScrollBar::handle:vertical {
    background: rgba(255,255,255,0.15); border-radius: 5px; min-height: 32px;
}
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
)");
}

QLabel *makeLabel(const QString &text, const char *objectName = nullptr) {
    auto *result = new QLabel(text);
    if (objectName != nullptr) {
        result->setObjectName(QString::fromLatin1(objectName));
    }
    result->setWordWrap(true);
    return result;
}

/// One setting: text on the left, control on the right.
QWidget *makeRow(const QString &title, const QString &hint, QWidget *control) {
    auto *container = new QWidget;
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);

    auto *text = new QVBoxLayout;
    text->setContentsMargins(0, 0, 0, 0);
    text->setSpacing(2);
    text->addWidget(makeLabel(title, "rowLabel"));
    if (!hint.isEmpty()) {
        text->addWidget(makeLabel(hint, "rowHint"));
    }
    layout->addLayout(text, 1);

    control->setParent(container);
    layout->addWidget(control, 0, Qt::AlignVCenter);
    return container;
}

QScrollArea *scrolled(QWidget *content) {
    auto *area = new QScrollArea;
    area->setWidgetResizable(true);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->setWidget(content);
    return area;
}

struct Placement {
    const char *value;
    const char *label;
};

struct CloudBackendChoice {
    const char *value;
    const char *label;
};

/// Mirrors `superior/cloudpinyin/Cloud.hpp`. The settings window only reads
/// `Settings`, so the list is restated here rather than pulling the networking
/// module into the UI process.
constexpr CloudBackendChoice kCloudBackends[] = {
    {"googlecn", "Google (google.cn)"},
    {"baidu", "百度（接口失效）"},
    {"sogou", "搜狗（暂不可用）"},
};

constexpr Placement kPlacements[] = {
    {"bottomRight", "右下角"},
    {"bottomLeft", "左下角"},
    {"topRight", "右上角"},
    {"topLeft", "左上角"},
};

QString placementLabel(const std::string &value) {
    for (const auto &placement : kPlacements) {
        if (value == placement.value) {
            return QString::fromUtf8(placement.label);
        }
    }
    return QString::fromUtf8(kPlacements[0].label);
}

std::string placementValue(const QString &label) {
    for (const auto &placement : kPlacements) {
        if (label == QString::fromUtf8(placement.label)) {
            return placement.value;
        }
    }
    return kPlacements[0].value;
}

} // namespace

// ---------------------------------------------------------------- ToggleSwitch

ToggleSwitch::ToggleSwitch(bool checked, QWidget *parent) : QWidget(parent), checked_(checked) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setFixedSize(44, 24);
}

void ToggleSwitch::setChecked(bool checked) {
    if (checked_ == checked) {
        return;
    }
    checked_ = checked;
    update();
}

void ToggleSwitch::setOnChange(std::function<void(bool)> callback) {
    onChange_ = std::move(callback);
}

QSize ToggleSwitch::sizeHint() const {
    return QSize(44, 24);
}

void ToggleSwitch::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF track(0.5, 2.5, width() - 1.0, height() - 5.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(checked_ ? QColor(42, 111, 219) : QColor(52, 57, 64));
    painter.drawRoundedRect(track, track.height() / 2.0, track.height() / 2.0);

    const double knobSize = track.height() - 4.0;
    const double knobX = checked_ ? track.right() - knobSize - 2.0 : track.left() + 2.0;
    painter.setBrush(QColor(232, 234, 237));
    painter.drawEllipse(QRectF(knobX, track.top() + 2.0, knobSize, knobSize));

    if (hasFocus()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(124, 196, 255, 180), 1.0));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);
    }
}

void ToggleSwitch::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    checked_ = !checked_;
    update();
    if (onChange_) {
        onChange_(checked_);
    }
}

void ToggleSwitch::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return) {
        checked_ = !checked_;
        update();
        if (onChange_) {
            onChange_(checked_);
        }
        return;
    }
    QWidget::keyPressEvent(event);
}

// --------------------------------------------------------------- SettingsWindow

SettingsWindow::SettingsWindow(config::Paths paths, QWidget *parent)
    : QWidget(parent), paths_(std::move(paths)) {
    setWindowTitle(tr("先！进！输入法 · 设置"));
    resize(940, 660);
    setStyleSheet(darkStyleSheet());
    buildUi();
    loadFromDisk();
    syncWidgets();
    reloadFcitxState();
}

QVBoxLayout *SettingsWindow::addCard(QVBoxLayout *pageLayout, const QString &title,
                                     const QString &subtitle) {
    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("card"));
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(18, 16, 18, 16);
    cardLayout->setSpacing(12);
    cardLayout->addWidget(makeLabel(title, "cardTitle"));
    if (!subtitle.isEmpty()) {
        cardLayout->addWidget(makeLabel(subtitle, "cardHint"));
    }
    pageLayout->addWidget(card);
    pageLayout->addSpacing(4);
    return cardLayout;
}

void SettingsWindow::buildUi() {
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- sidebar -----------------------------------------------------------
    auto *sidebar = new QWidget;
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setFixedWidth(248);

    auto *sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(18, 20, 18, 16);
    sidebarLayout->setSpacing(12);

    auto *brandRow = new QHBoxLayout;
    brandRow->setSpacing(8);
    brandRow->addWidget(makeLabel(QStringLiteral("◆"), "brandDot"));
    brandRow->addWidget(makeLabel(tr("先！进！输入法"), "brand"), 1);
    sidebarLayout->addLayout(brandRow);

    search_ = new QLineEdit;
    search_->setPlaceholderText(tr("搜索设置项"));
    search_->setClearButtonEnabled(true);
    sidebarLayout->addWidget(search_);

    nav_ = new QListWidget;
    nav_->setFrameShape(QFrame::NoFrame);
    const QStringList categories{tr("外观"), tr("输入"), tr("状态栏"), tr("语音"), tr("高级"),
                                 tr("关于")};
    for (const auto &category : categories) {
        auto *item = new QListWidgetItem(category);
        item->setSizeHint(QSize(0, 38));
        nav_->addItem(item);
    }
    nav_->setCurrentRow(0);
    sidebarLayout->addWidget(nav_, 1);
    sidebarLayout->addWidget(makeLabel(tr("版本 0.1.0 · GPL-3.0"), "version"));

    root->addWidget(sidebar);

    // ---- content -----------------------------------------------------------
    auto *content = new QWidget;
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(28, 22, 28, 16);
    contentLayout->setSpacing(12);

    pageTitle_ = makeLabel(QString(), "pageTitle");
    pageSubtitle_ = makeLabel(QString(), "pageSubtitle");
    contentLayout->addWidget(pageTitle_);
    contentLayout->addWidget(pageSubtitle_);

    pages_ = new QStackedWidget;
    pages_->addWidget(buildAppearancePage());
    pages_->addWidget(buildInputPage());
    pages_->addWidget(buildStatusPage());
    pages_->addWidget(buildVoicePage());
    pages_->addWidget(buildAdvancedPage());
    pages_->addWidget(buildAboutPage());
    contentLayout->addWidget(pages_, 1);

    auto *footer = new QHBoxLayout;
    footer->setSpacing(12);
    status_ = makeLabel(QString(), "status");
    footer->addWidget(status_, 1);

    auto *applyButton = new QPushButton(tr("应用"));
    applyButton->setObjectName(QStringLiteral("primary"));
    applyButton->setCursor(Qt::PointingHandCursor);
    footer->addWidget(applyButton);
    contentLayout->addLayout(footer);

    root->addWidget(content, 1);

    connect(applyButton, &QPushButton::clicked, this, [this] { applyChanges(); });

    connect(nav_, &QListWidget::currentRowChanged, this, [this](int index) {
        if (index < 0 || pages_ == nullptr) {
            return;
        }
        pages_->setCurrentIndex(index);
        static const char *const titles[] = {"外观", "输入", "状态栏", "语音", "高级", "关于"};
        static const char *const subtitles[] = {
            "皮肤、缩放与主题导出",       "中英文切换与候选行为",
            "右下角状态栏的显隐与位置",   "语音输入的开关（引擎尚未接入）",
            "日志、配置文件与重置",       "版本、架构与许可",
        };
        const int bounded = std::clamp(index, 0, 5);
        pageTitle_->setText(tr(titles[bounded]));
        pageSubtitle_->setText(tr(subtitles[bounded]));
    });

    connect(search_, &QLineEdit::textChanged, this, [this](const QString &text) {
        const QString query = text.trimmed();
        int firstVisible = -1;
        for (int i = 0; i < nav_->count(); ++i) {
            const bool match =
                query.isEmpty() || nav_->item(i)->text().contains(query, Qt::CaseInsensitive);
            nav_->item(i)->setHidden(!match);
            if (match && firstVisible < 0) {
                firstVisible = i;
            }
        }
        if (!query.isEmpty() && firstVisible >= 0 &&
            nav_->item(nav_->currentRow())->isHidden()) {
            nav_->setCurrentRow(firstVisible);
        }
    });

    // Explicit initial selection: setCurrentRow(0) alone is a no-op when the
    // row is already 0, so the page stack and header must be set directly.
    nav_->setCurrentRow(0);
    pages_->setCurrentIndex(0);
    pageTitle_->setText(tr("外观"));
    pageSubtitle_->setText(tr("皮肤、缩放与主题导出"));
}

QWidget *SettingsWindow::buildAppearancePage() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(6);

    QVBoxLayout *skinCard =
        addCard(layout, tr("皮肤"), tr("皮肤决定候选栏、状态栏与语音面板的外观。"));
    skinCombo_ = new QComboBox;
    skinCard->addWidget(makeRow(tr("当前皮肤"), tr("从已安装的皮肤中选择"), skinCombo_));

    skinCard->addWidget(makeLabel(
        tr("皮肤只作用于先！进！输入法自己的候选窗与状态栏；其他输入法保持 Fcitx5 原有外观。"),
        "rowHint"));
    themeState_ = makeLabel(QString(), "rowHint");
    skinCard->addWidget(themeState_);

    QVBoxLayout *scaleCard = addCard(layout, tr("缩放"), tr("界面缩放会同时影响状态栏与候选栏。"));
    auto *scaleRow = new QWidget;
    auto *scaleLayout = new QHBoxLayout(scaleRow);
    scaleLayout->setContentsMargins(0, 0, 0, 0);
    scaleLayout->setSpacing(12);
    scaleSlider_ = new QSlider(Qt::Horizontal);
    scaleSlider_->setRange(kMinScale, kMaxScale);
    scaleSlider_->setSingleStep(5);
    scaleSlider_->setPageStep(25);
    scaleValue_ = makeLabel(QStringLiteral("100%"), "rowLabel");
    scaleValue_->setMinimumWidth(56);
    scaleValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    scaleLayout->addWidget(scaleSlider_, 1);
    scaleLayout->addWidget(scaleValue_);
    scaleCard->addWidget(makeRow(tr("界面比例"), tr("75% – 300%"), scaleRow));
    connect(scaleSlider_, &QSlider::valueChanged, this, [this](int value) {
        scaleValue_->setText(QString::number(value) + QStringLiteral("%"));
    });

    layout->addStretch();
    return scrolled(page);
}

QWidget *SettingsWindow::buildInputPage() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(6);

    QVBoxLayout *card = addCard(
        layout, tr("中英文切换"),
        tr("轻按 Shift 在输入法内部切换中英文，不切换 Fcitx5 输入法，也不需要额外的英文键盘布局。"
           "长按 Shift 输入大写字母不会触发切换。"));
    shiftToggle_ = new ToggleSwitch(true);
    card->addWidget(makeRow(tr("Shift 切换中英文"), tr("输入法内生效；按 Esc 回到中文"),
                            shiftToggle_));
    triggerState_ = makeLabel(QString(), "rowHint");
    card->addWidget(triggerState_);
    card->addWidget(makeLabel(
        tr("以英文模式输入的字母会直接上屏，不经过拼音候选。这与搜狗输入法的行为一致。"),
        "rowHint"));

    addCard(layout, tr("候选窗口"),
            tr("候选窗由 Superior 自己的浮层绘制，不受 Fcitx5 全局主题影响；"
               "其他输入法仍使用它们原来的外观。Wayland 会话下浮层自动走 XWayland，"
               "保持永久置顶且不抢焦点。"));

    QVBoxLayout *cloudCard = addCard(
        layout, tr("云拼音"),
        tr("把当前拼音发送到在线服务，用返回的词补一个候选。拼音本身会离开本机，"
           "所以默认关闭；密码框与英文模式永远不会发送。"));
    cloudToggle_ = new ToggleSwitch(false);
    cloudCard->addWidget(makeRow(tr("启用云拼音"), tr("输入两个字母以上才会上网查询"),
                                 cloudToggle_));
    cloudBackendCombo_ = new QComboBox;
    for (const auto &backend : kCloudBackends) {
        cloudBackendCombo_->addItem(QString::fromUtf8(backend.label),
                                    QString::fromUtf8(backend.value));
    }
    cloudCard->addWidget(
        makeRow(tr("服务"), tr("百度接口目前失效，选择它会一直查不到结果"),
                cloudBackendCombo_));
    cloudCard->addWidget(makeLabel(
        tr("连续失败三次后会自动暂停一分钟，避免每次打字都去撞一个连不上的服务。"
           "查到的结果会缓存，同一个词再打一次不再联网。"),
        "rowHint"));

    layout->addStretch();
    return scrolled(page);
}

QWidget *SettingsWindow::buildStatusPage() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(6);

    QVBoxLayout *card =
        addCard(layout, tr("状态栏"), tr("状态栏是屏幕锚定的浮层，永久置顶且不抢焦点。"));
    statusBarToggle_ = new ToggleSwitch(true);
    card->addWidget(makeRow(tr("显示状态栏"), tr("常驻显示当前输入法状态"), statusBarToggle_));

    placementCombo_ = new QComboBox;
    for (const auto &placement : kPlacements) {
        placementCombo_->addItem(QString::fromUtf8(placement.label));
    }
    card->addWidget(makeRow(tr("位置"), tr("相对屏幕可用区域"), placementCombo_));
    card->addWidget(makeLabel(
        tr("直接拖动状态栏左侧的圆点也能移动它，位置会立刻记住；"
           "在这里重新选择位置会丢弃拖动过的坐标。"),
        "rowHint"));

    layout->addStretch();
    return scrolled(page);
}

QWidget *SettingsWindow::buildVoicePage() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(6);

    QVBoxLayout *card = addCard(
        layout, tr("语音输入"),
        tr("界面已预留，识别引擎尚未接入。启用后仍不会上传音频：未来会优先使用本地模型。"));
    voiceToggle_ = new ToggleSwitch(false);
    card->addWidget(makeRow(tr("启用语音面板"), tr("在状态栏显示麦克风按钮"), voiceToggle_));

    layout->addStretch();
    return scrolled(page);
}

QWidget *SettingsWindow::buildAdvancedPage() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(6);

    QVBoxLayout *logCard =
        addCard(layout, tr("日志"), tr("日志写入标准错误，或由 SUPERIOR_LOG_FILE 写入文件。"));
    logLevelCombo_ = new QComboBox;
    logLevelCombo_->addItems({"trace", "debug", "info", "warning", "error", "off"});
    logCard->addWidget(
        makeRow(tr("日志级别"), tr("这里只保存偏好；实际级别用环境变量控制"), logLevelCombo_));

    QVBoxLayout *pathsCard =
        addCard(layout, tr("文件位置"), tr("这些路径由 XDG 变量决定。"));
    const auto addPath = [this, pathsCard](const char *title, const fs::path &path) {
        pathsCard->addWidget(makeLabel(tr(title), "rowLabel"));
        pathsCard->addWidget(makeLabel(QString::fromStdString(path.string()), "rowHint"));
    };
    addPath("本程序设置", config::defaultSettingsPath());
    addPath("Fcitx5 经典界面", paths_.classicUiConf());
    addPath("Fcitx5 主题目录", paths_.userThemeDir());

    QVBoxLayout *resetCard =
        addCard(layout, tr("重置"), tr("只重置本程序的设置，不触碰 Fcitx5 输入法配置。"));
    auto *resetButton = new QPushButton(tr("恢复默认设置"));
    resetButton->setCursor(Qt::PointingHandCursor);
    resetCard->addWidget(makeRow(tr("恢复默认"), tr("皮肤、缩放、状态栏与日志级别"), resetButton));
    connect(resetButton, &QPushButton::clicked, this, [this] {
        settings_ = config::Settings{};
        syncWidgets();
        setStatus(tr("已恢复默认值，点击“应用”写入。"));
    });

    layout->addStretch();
    return scrolled(page);
}

QWidget *SettingsWindow::buildAboutPage() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(6);

    QVBoxLayout *card =
        addCard(layout, tr("先！进！输入法"), tr("跨平台输入法的 UI 与皮肤层，Linux/Fcitx5 优先。"));
    card->addWidget(makeLabel(tr("输入引擎：Fcitx5 + libime（不重写拼音引擎）"), "rowHint"));
    card->addWidget(makeLabel(tr("皮肤格式：superior.skin/1（自建、可校验）"), "rowHint"));
    card->addWidget(makeLabel(tr("界面：Qt 6，前端只依赖契约与纯数据模块"), "rowHint"));
    card->addWidget(makeLabel(tr("许可：GPL-3.0-or-later"), "rowHint"));

    layout->addStretch();
    return scrolled(page);
}

void SettingsWindow::loadFromDisk() {
    const auto loaded = config::loadSettings(config::defaultSettingsPath());
    settings_ = loaded.settings;
    for (const auto &warning : loaded.warnings) {
        SUPERIOR_WARN(diag::categories::Ui) << warning;
    }
    refreshSkinList();
}

void SettingsWindow::syncWidgets() {
    if (shiftToggle_ != nullptr) {
        shiftToggle_->setChecked(settings_.shiftToggle);
    }
    if (cloudToggle_ != nullptr) {
        cloudToggle_->setChecked(settings_.cloudPinyinEnabled);
    }
    if (cloudBackendCombo_ != nullptr) {
        const QString value = QString::fromStdString(settings_.cloudPinyinBackend);
        const int index = cloudBackendCombo_->findData(value);
        if (index >= 0) {
            cloudBackendCombo_->setCurrentIndex(index);
        }
    }
    if (statusBarToggle_ != nullptr) {
        statusBarToggle_->setChecked(settings_.showStatusBar);
    }
    if (voiceToggle_ != nullptr) {
        voiceToggle_->setChecked(settings_.voiceEnabled);
    }
    if (placementCombo_ != nullptr) {
        placementCombo_->setCurrentText(placementLabel(settings_.statusPlacement));
    }
    if (scaleSlider_ != nullptr) {
        const int percent = static_cast<int>(settings_.uiScale * 100.0 + 0.5);
        scaleSlider_->setValue(std::clamp(percent, kMinScale, kMaxScale));
        scaleValue_->setText(QString::number(scaleSlider_->value()) + QStringLiteral("%"));
    }
    if (logLevelCombo_ != nullptr) {
        logLevelCombo_->setCurrentText(QString::fromStdString(settings_.logLevel));
    }
    if (skinCombo_ != nullptr && skinCombo_->isEnabled()) {
        const int index = skinCombo_->findData(QString::fromStdString(settings_.skinId));
        if (index >= 0) {
            skinCombo_->setCurrentIndex(index);
        }
    }
}

void SettingsWindow::refreshSkinList() {
    if (skinCombo_ == nullptr) {
        return;
    }

    skins_.clear();
    for (const auto &root : config::skinRoots(paths_.dataHome, paths_.dataDirs)) {
        for (auto &summary : skin::scanSkins(root)) {
            const bool seen = std::any_of(skins_.begin(), skins_.end(),
                                          [&summary](const auto &existing) {
                                              return existing.id == summary.id;
                                          });
            if (!seen) {
                skins_.push_back(std::move(summary));
            }
        }
    }
    std::sort(skins_.begin(), skins_.end(),
              [](const auto &lhs, const auto &rhs) { return lhs.id < rhs.id; });
    skinCombo_->clear();
    for (const auto &summary : skins_) {
        const QString name =
            QString::fromStdString(summary.name.empty() ? summary.id : summary.name);
        skinCombo_->addItem(name, QString::fromStdString(summary.id));
    }
    if (skins_.empty()) {
        skinCombo_->addItem(tr("未找到已安装皮肤"));
        skinCombo_->setEnabled(false);
        return;
    }
    skinCombo_->setEnabled(true);
    const int index = skinCombo_->findData(QString::fromStdString(settings_.skinId));
    if (index >= 0) {
        skinCombo_->setCurrentIndex(index);
    }
}

void SettingsWindow::reloadFcitxState() {
    fcitxState_ = config::inspect(paths_, settings_.themeName);
    if (themeState_ != nullptr) {
        const bool installed =
            std::any_of(skins_.begin(), skins_.end(), [this](const auto &summary) {
                return summary.id == settings_.skinId;
            });
        themeState_->setText(installed ? tr("皮肤已就绪，由 Superior 浮层绘制")
                                       : tr("未找到所选皮肤，将使用内置外观"));
    }
    if (triggerState_ != nullptr) {
        QStringList keys;
        for (const auto &key : fcitxState_.triggerKeys) {
            keys << QString::fromStdString(key);
        }
        triggerState_->setText(
            settings_.shiftToggle
                ? tr("输入法内切换已启用；Fcitx5 全局触发键不作为中英文开关")
                : tr("输入法内切换已关闭；按 Fcitx5 的全局触发键切换输入法"));
    }
}

config::Settings SettingsWindow::currentSettings() const {
    config::Settings result = settings_;

    if (skinCombo_ != nullptr && skinCombo_->isEnabled()) {
        const QString id = skinCombo_->currentData().toString();
        if (!id.isEmpty()) {
            result.skinId = id.toStdString();
            result.themeName = id.toStdString();
        }
    }
    if (shiftToggle_ != nullptr) {
        result.shiftToggle = shiftToggle_->isChecked();
    }
    if (cloudToggle_ != nullptr) {
        result.cloudPinyinEnabled = cloudToggle_->isChecked();
    }
    if (cloudBackendCombo_ != nullptr) {
        const QString value = cloudBackendCombo_->currentData().toString();
        if (!value.isEmpty()) {
            result.cloudPinyinBackend = value.toStdString();
        }
    }
    if (statusBarToggle_ != nullptr) {
        result.showStatusBar = statusBarToggle_->isChecked();
    }
    if (voiceToggle_ != nullptr) {
        result.voiceEnabled = voiceToggle_->isChecked();
    }
    if (placementCombo_ != nullptr) {
        result.statusPlacement = placementValue(placementCombo_->currentText());
    }
    if (scaleSlider_ != nullptr) {
        result.uiScale = static_cast<double>(scaleSlider_->value()) / 100.0;
    }
    if (logLevelCombo_ != nullptr) {
        result.logLevel = logLevelCombo_->currentText().toStdString();
    }
    return result;
}

void SettingsWindow::applyChanges() {
    settings_ = currentSettings();

    std::string error;
    if (!config::saveSettings(config::defaultSettingsPath(), settings_, error)) {
        setStatus(QString::fromStdString(error), true);
        return;
    }

    // Deliberately does NOT touch classicui.conf: the skin is painted by our own
    // overlay, so other input methods keep their Fcitx5 appearance.
    reloadFcitxState();
    setStatus(tr("已保存。界面与 Shift 切换即时生效。"));
    SUPERIOR_INFO(diag::categories::Ui)
        .field("theme", settings_.themeName)
        .field("shift", settings_.shiftToggle)
        << "settings applied";
}

void SettingsWindow::setStatus(const QString &text, bool error) {
    if (status_ == nullptr) {
        return;
    }
    status_->setObjectName(error ? QStringLiteral("statusError") : QStringLiteral("status"));
    status_->setText(text);
    status_->style()->unpolish(status_);
    status_->style()->polish(status_);
}

} // namespace superior::ui
