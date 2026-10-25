// SPDX-License-Identifier: GPL-3.0-or-later
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QLockFile>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include "superior/api/Commands.hpp"
#include "superior/config/Fcitx5Config.hpp"
#include "superior/config/Settings.hpp"
#include "superior/protocol/SceneServer.hpp"
#include "superior/skin/Skin.hpp"

#include "CandidateWindow.hpp"
#include "LayoutRules.hpp"
#include "Logging.hpp"
#include "PlatformChoice.hpp"
#include "SceneClient.hpp"
#include "SkinTheme.hpp"
#include "StatusWindow.hpp"

#include "superior/diagnostics/Log.hpp"

namespace {

using namespace superior;

namespace diag = ::superior::diagnostics;

/// Loads the skin selected in `settings.json`, searched in the user directory
/// first and then in each system data directory.
///
/// The overlay is the only painter of the candidate window for our input
/// method: no Fcitx5 theme is touched, so Rime and every other input method keep
/// their own appearance.
std::optional<skin::SkinSpec> loadConfiguredSkin(const config::Paths &paths,
                                                 const std::string &id) {
    if (id.empty()) {
        return std::nullopt;
    }
    for (const auto &root : config::skinRoots(paths.dataHome, paths.dataDirs)) {
        std::error_code error;
        if (!std::filesystem::is_directory(root, error)) {
            continue;
        }
        for (const auto &summary : skin::scanSkins(root)) {
            if (summary.id != id) {
                continue;
            }
            auto loaded = skin::loadSkin(summary.path);
            if (loaded.spec) {
                return loaded.spec;
            }
            std::cerr << "superior-ui: skin '" << summary.id
                      << "' failed to load: " << loaded.summary() << '\n';
        }
    }
    return std::nullopt;
}

} // namespace

int main(int argc, char **argv) {
    // Must happen before QApplication so the platform plugin is chosen here and
    // not by Qt's own auto-detection: on a Wayland session the overlay has to
    // run through XWayland to stay override-redirect (always on top, no focus).
    {
        const auto env = [](const char *name) -> std::string {
            const char *value = std::getenv(name);
            return value != nullptr ? std::string(value) : std::string{};
        };
        const auto decision = superior::ui::chooseQtPlatform(
            env("SUPERIOR_UI_PLATFORM"), env("QT_QPA_PLATFORM"), env("WAYLAND_DISPLAY"),
            env("DISPLAY"), env("XDG_SESSION_TYPE"));
        if (!decision.value.empty()) {
            qputenv("QT_QPA_PLATFORM", QByteArray::fromStdString(decision.value));
        }
        std::cerr << "superior-ui: platform choice=" << decision.value
                  << " reason=" << decision.reason << '\n';
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("superior-ui"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    superior::ui::installQtLogging();

    // A single overlay owns the socket connection. The agent may spawn us more
    // than once (focus retries, Fcitx5 reloads); a second copy would draw a
    // second set of windows.
    const QString runtimeDir =
        QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    QLockFile instanceLock(runtimeDir + QStringLiteral("/superior-ime-ui.lock"));
    instanceLock.setStaleLockTime(30000);
    if (!instanceLock.tryLock(0)) {
        instanceLock.removeStaleLockFile();
        if (!instanceLock.tryLock(0)) {
            std::cout << "superior-ui: already running" << std::endl;
            return 0;
        }
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Superior-IME overlay: self-drawn candidate window and status bar"));
    parser.addHelpOption();
    QCommandLineOption socketOption(QStringList{QStringLiteral("s"), QStringLiteral("socket")},
                                    QStringLiteral("Scene socket path"), QStringLiteral("path"));
    QCommandLineOption skinOption(QStringLiteral("skin"),
                                  QStringLiteral("Skin id; defaults to settings.json"),
                                  QStringLiteral("id"));
    QCommandLineOption noCandidatesOption(
        QStringLiteral("no-candidates"),
        QStringLiteral("Do not draw the candidate window"));
    QCommandLineOption candidatesOption(
        QStringLiteral("candidates"),
        QStringLiteral("Accepted for compatibility; the candidate window is on by default"));
    parser.addOption(socketOption);
    parser.addOption(skinOption);
    parser.addOption(noCandidatesOption);
    parser.addOption(candidatesOption);
    parser.process(app);

    QString socketPath = parser.value(socketOption);
    if (socketPath.isEmpty()) {
        socketPath = QString::fromStdString(protocol::SceneServer::defaultSocketPath());
    }

    const bool drawCandidates = !parser.isSet(noCandidatesOption);
    const config::Paths paths = config::Paths::fromEnvironment();
    const std::filesystem::path settingsPath = config::defaultSettingsPath();

    config::Settings settings = config::loadSettings(settingsPath).settings;
    std::string activeSkinId = parser.value(skinOption).toStdString();
    if (activeSkinId.empty()) {
        activeSkinId = settings.skinId;
    }
    std::optional<skin::SkinSpec> activeSkin = loadConfiguredSkin(paths, activeSkinId);

    superior::ui::StatusWindow status;
    superior::ui::CandidateWindow candidates;

    status.setScale(settings.uiScale);
    candidates.setScale(settings.uiScale);
    status.setPlacement(superior::ui::placementFromString(settings.statusPlacement));
    status.setEnabled(settings.showStatusBar);

    superior::ui::SceneClient client(socketPath);

    std::optional<api::UiSceneFrame> lastFrame;
    const auto render = [&](const api::UiSceneFrame &frame) {
        // A frame with both surfaces hidden means another input method is
        // active. Draw nothing at all: Fcitx5 keeps its own panel and theme in
        // that case.
        if (!frame.panel.visible && !frame.status.visible) {
            status.apply(frame.status, {}, {}, superior::ui::SkinTheme::fallback());
            candidates.apply(frame.panel, superior::ui::SkinTheme::fallback());
            return;
        }

        const superior::ui::SkinTheme theme =
            activeSkin ? superior::ui::SkinTheme::from(*activeSkin, frame.panel.styleId)
                       : superior::ui::SkinTheme::fallback();
        status.apply(frame.status, frame.panel.mode, {}, theme);
        if (drawCandidates) {
            candidates.apply(frame.panel, theme);
        }
    };

    client.setHandler([&](const api::UiSceneFrame &frame) {
        lastFrame = frame;
        render(frame);
    });

    // The status bar only sends intent; the agent owns the mode and answers
    // with a new scene.
    status.setItemClickHandler([&client](const std::string &itemId) {
        SUPERIOR_INFO(diag::categories::Ui)
            .field("item", itemId)
            << "status item clicked";
        client.send(api::ToggleStatusItem{itemId});
    });

    client.start();

    // Settings are shared with the agent, which polls the same file. Polling
    // here as well keeps the two processes from inventing another IPC surface.
    QTimer settingsTimer;
    settingsTimer.setInterval(500);
    std::filesystem::file_time_type settingsStamp{};
    bool settingsStampValid = false;
    {
        // Seed the stamp so the first tick does not re-apply the settings that
        // were just read above (and does not report a change that never
        // happened).
        std::error_code error;
        const auto stamp = std::filesystem::last_write_time(settingsPath, error);
        if (!error) {
            settingsStamp = stamp;
            settingsStampValid = true;
        }
    }
    QObject::connect(&settingsTimer, &QTimer::timeout, [&]() {
        std::error_code error;
        const auto stamp = std::filesystem::last_write_time(settingsPath, error);
        if (error) {
            return;
        }
        if (settingsStampValid && stamp == settingsStamp) {
            return;
        }
        settingsStamp = stamp;
        settingsStampValid = true;

        const auto loaded = config::loadSettings(settingsPath);
        settings = loaded.settings;
        if (settings.skinId != activeSkinId) {
            activeSkinId = settings.skinId;
            activeSkin = loadConfiguredSkin(paths, activeSkinId);
        }
        status.setScale(settings.uiScale);
        candidates.setScale(settings.uiScale);
        status.setPlacement(superior::ui::placementFromString(settings.statusPlacement));
        status.setEnabled(settings.showStatusBar);
        SUPERIOR_INFO(diag::categories::Ui)
            .field("skin", settings.skinId)
            .field("scale", settings.uiScale, 2)
            .field("statusBar", settings.showStatusBar)
            .field("placement", settings.statusPlacement)
            << "settings applied";
        if (lastFrame) {
            render(*lastFrame);
        }
    });
    settingsTimer.start();

    std::cout << "superior-ui: platform=" << QGuiApplication::platformName().toStdString()
              << " socket=" << socketPath.toStdString()
              << " skin=" << (activeSkin ? activeSkin->id : std::string("fallback"))
              << " candidates=" << (drawCandidates ? "on" : "off") << std::endl;

    const int result = QApplication::exec();
    instanceLock.unlock();
    return result;
}
