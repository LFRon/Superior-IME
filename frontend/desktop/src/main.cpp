// SPDX-License-Identifier: GPL-3.0-or-later
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <QLockFile>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include "superior/api/Commands.hpp"
#include "superior/config/Fcitx5Config.hpp"
#include "superior/config/Settings.hpp"
#include "superior/config/StatusBarState.hpp"
#include "superior/protocol/SceneServer.hpp"
#include "superior/skin/Skin.hpp"

#include "CandidateWindow.hpp"
#include "LayoutRules.hpp"
#include "Logging.hpp"
#include "PlatformChoice.hpp"
#include "SceneClient.hpp"
#include "SpeechRecognitionPanel.hpp"
#include "SkinTheme.hpp"
#include "OverlayLayout.hpp"
#include "StatusWindow.hpp"
#include "platform/ScreenScale.hpp"

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
    superior::ui::SpeechRecognitionPanel speech;

    // --------------------------------------------------------------- scale
    //
    // The strategy layer only ever sees one ratio (`setScale`). Where the
    // desktop's own factor comes from is the platform's business: X11 reads
    // `Xft.dpi`, Wayland reads `wl_output.scale` (`platform/ScreenScale*`).
    //
    // It is asked here rather than taken from Qt: Qt derives its DPR from the
    // same resource at start-up but cannot tell a stale `Xft.dpi` from a real
    // one, and it never re-reads either. Reading the desktop's own declaration
    // keeps the toolbar the same size as the applications that honour it.
    // `scaleForWindows` divides by Qt's cached ratio, so the final size is
    // `system × uiScale` exactly once. The scale is read once: on X11 changing
    // it needs a logout, and the overlay restarts with the session.
    const superior::ui::SystemScale systemScale = superior::ui::readSystemScale();
    double appliedScale = 1.0;
    const auto resolvedScale = [&]() {
        double dpr = 1.0;
        if (const QScreen *screen = QGuiApplication::primaryScreen()) {
            dpr = screen->devicePixelRatio();
        }
        return superior::ui::scaleForWindows(systemScale, dpr, settings.uiScale);
    };
    const auto applyScale = [&](const char *reason) {
        const double next = resolvedScale();
        const bool changed = std::abs(next - appliedScale) >= 0.001;
        appliedScale = next;
        status.setScale(appliedScale);
        candidates.setScale(appliedScale);
        speech.setScale(appliedScale);
        if (!changed) {
            return;
        }
        double dpr = 1.0;
        if (const QScreen *screen = QGuiApplication::primaryScreen()) {
            dpr = screen->devicePixelRatio();
        }
        SUPERIOR_INFO(diag::categories::Ui)
            .field("reason", std::string(reason))
            .field("source", std::string(superior::ui::scaleSourceName(systemScale.source)))
            .field("raw", static_cast<std::int64_t>(systemScale.raw))
            .field("system", systemScale.factor, 3)
            .field("toolkitDpr", dpr, 3)
            .field("userScale", settings.uiScale, 3)
            .field("applied", appliedScale, 3)
            << "overlay scale resolved";
    };
    applyScale("startup");

    status.setPlacement(superior::ui::placementFromString(settings.statusPlacement));
    status.setEnabled(settings.showStatusBar);

    // Dragged position lives in its own file: the overlay writes it on every
    // drag release, the settings page writes settings.json. Mixing them would
    // make one overwrite the other.
    const std::filesystem::path statusStatePath = config::defaultStatusBarStatePath();
    config::StatusBarState statusState = config::loadStatusBarState(statusStatePath).state;
    if (statusState.hasCustomPosition) {
        if (statusState.placement == settings.statusPlacement) {
            status.setSavedPosition(superior::ui::Point{statusState.x, statusState.y});
        } else {
            // The user picked another corner after dragging; the corner wins.
            std::string ignored;
            static_cast<void>(config::clearStatusBarState(statusStatePath, ignored));
            statusState.hasCustomPosition = false;
        }
    }
    status.setPositionChangeHandler([&](int x, int y) {
        statusState.hasCustomPosition = true;
        statusState.x = x;
        statusState.y = y;
        statusState.placement = settings.statusPlacement;
        std::string error;
        if (!config::saveStatusBarState(statusStatePath, statusState, error)) {
            SUPERIOR_WARN(diag::categories::Ui)
                .field("error", error)
                .field("path", statusStatePath.string())
                << "cannot persist the status bar position";
            return;
        }
        SUPERIOR_INFO(diag::categories::Ui)
            .field("x", static_cast<std::int64_t>(x))
            .field("y", static_cast<std::int64_t>(y))
            .field("path", statusStatePath.string())
            << "status bar position saved";
    });

    superior::ui::SceneClient client(socketPath);

    std::optional<api::UiSceneFrame> lastFrame;
    const auto render = [&](const api::UiSceneFrame &frame) {
        // A frame with both surfaces hidden means another input method is
        // active. Fcitx5 keeps its own panel and theme in that case, so nothing
        // of ours is drawn.
        const bool ours = frame.panel.visible || frame.status.visible;
        const superior::ui::SkinTheme theme =
            (activeSkin && ours) ? superior::ui::SkinTheme::from(*activeSkin, frame.panel.styleId)
                                 : superior::ui::SkinTheme::fallback();

        status.apply(frame.status, ours ? frame.panel.mode : std::string{}, {}, theme);

        // The speech panel shares the process with the other two overlays, so a
        // single geometry policy keeps them from ever stacking onto each other.
        const QRect statusRect = status.geometry();
        speech.setStatusBarRect(superior::ui::Rect{statusRect.left(), statusRect.top(),
                                                   statusRect.width(), statusRect.height()});
        if (const QScreen *screen = QGuiApplication::primaryScreen()) {
            const QRect available = screen->availableGeometry();
            speech.setScreen(superior::ui::Rect{available.left(), available.top(),
                                                available.width(), available.height()});
        }
        // Applied even when no surface of ours is visible: leaving the speech
        // panel on screen after a switch to another input method left a window
        // behind that no later frame would ever take down again.
        speech.apply(frame.speech, frame.session, theme);

        if (!ours) {
            candidates.apply(frame.panel, superior::ui::SkinTheme::fallback());
            return;
        }

        // Three overlays, one geometry policy: the candidate panel steps around
        // the speech panel rather than covering it. `raise()` is not enough on a
        // compositor that reorders override-redirect windows.
        candidates.setAvoidRect(speech.occupiedRect());

        if (drawCandidates) {
            candidates.apply(frame.panel, theme);
        }
    };

    client.setHandler([&](const api::UiSceneFrame &frame) {
        lastFrame = frame;
        render(frame);
    });

    // Candidate clicks and wheel paging are intent too: the id of the frame the
    // user saw is sent back, and the agent re-validates it against the live
    // list. Nothing here mutates input state.
    candidates.setActivateHandler([&client, &lastFrame](api::CandidateId candidate) {
        if (!lastFrame) {
            return;
        }
        SUPERIOR_INFO(diag::categories::Ui)
            .field("candidate", static_cast<std::int64_t>(candidate))
            << "candidate clicked";
        client.send(api::ActivateCandidate{lastFrame->session, candidate});
    });
    candidates.setPageHandler([&client, &lastFrame](int delta) {
        if (!lastFrame) {
            return;
        }
        SUPERIOR_DEBUG(diag::categories::Ui)
            .field("delta", static_cast<std::int64_t>(delta))
            << "page requested";
        client.send(api::PageCandidates{lastFrame->session, delta});
    });

    // The status bar only sends intent; the agent owns the mode and answers
    // with a new scene.
    speech.setCommandHandler([&client](const api::UiCommand &command) {
        client.send(command);
    });

    status.setItemClickHandler([&client](const std::string &itemId) {
        SUPERIOR_INFO(diag::categories::Ui)
            .field("item", itemId)
            << "status item clicked";
        client.send(api::ToggleStatusItem{itemId});
    });

    client.start();

    // Settings are shared with the agent through `settings.json`. The file is
    // the single source of truth for both processes, so a second IPC surface
    // would only be one more thing that can drift.
    //
    // Watching beats polling, though. A switch flipped in the Fcitx5 tray menu
    // used to sit in the file until the next 500 ms tick, and that tick *was*
    // the lag the toolbar toggle showed. inotify delivers the write on the next
    // event-loop turn instead, which is what a Qt signal/slot would give us if
    // the click happened in this process rather than in the agent's.
    //
    // The *directory* is watched, not the file: `config::saveSettings` writes a
    // temporary and renames it into place, so the inode behind `settings.json`
    // is replaced by every save and a watch on the file itself would be dropped
    // by the first one. Every writer goes through `saveSettings`.
    std::filesystem::file_time_type settingsStamp{};
    bool settingsStampValid = false;
    auto applySettingsIfChanged = [&]() {
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
        const std::string previousPlacement = settings.statusPlacement;
        settings = loaded.settings;
        if (settings.statusPlacement != previousPlacement) {
            // Choosing a corner in the settings page discards the dragged
            // position, otherwise the choice would appear to do nothing.
            status.setSavedPosition(std::nullopt);
            std::string ignored;
            static_cast<void>(config::clearStatusBarState(statusStatePath, ignored));
            statusState.hasCustomPosition = false;
        }
        if (settings.skinId != activeSkinId) {
            activeSkinId = settings.skinId;
            activeSkin = loadConfiguredSkin(paths, activeSkinId);
        }
        applyScale("settings applied");
        status.setPlacement(superior::ui::placementFromString(settings.statusPlacement));
        status.setEnabled(settings.showStatusBar);
        SUPERIOR_INFO(diag::categories::Ui)
            .field("skin", settings.skinId)
            .field("uiScale", settings.uiScale, 2)
            .field("appliedScale", appliedScale, 3)
            .field("statusBar", settings.showStatusBar)
            .field("placement", settings.statusPlacement)
            << "settings applied";
        if (lastFrame) {
            render(*lastFrame);
        }
    };

    // Seed the stamp so the first event does not re-apply the settings that
    // were just read above (and does not report a change that never happened).
    {
        std::error_code error;
        const auto stamp = std::filesystem::last_write_time(settingsPath, error);
        if (!error) {
            settingsStamp = stamp;
            settingsStampValid = true;
        }
    }

    const std::string settingsDirectory = settingsPath.parent_path().string();
    QFileSystemWatcher settingsWatcher;
    // Only reached when inotify is unavailable or the config directory does not
    // exist yet; every tick also retries the watch so a late directory upgrade
    // stops the polling on its own.
    QTimer settingsRescan;
    settingsRescan.setInterval(500);
    const auto armSettingsWatch = [&]() {
        if (settingsDirectory.empty() || !settingsWatcher.directories().isEmpty()) {
            return;
        }
        std::error_code error;
        if (!std::filesystem::is_directory(settingsDirectory, error)) {
            return;
        }
        if (!settingsWatcher.addPath(QString::fromStdString(settingsDirectory))) {
            return;
        }
        SUPERIOR_INFO(diag::categories::Ui).field("directory", settingsDirectory)
            << "watching settings";
        settingsRescan.stop();
    };

    // The directory can be deleted or renamed away too, and Qt drops the watch
    // when that happens; this handler notices and hands the job back to the
    // poller, which upgrades itself again once the directory returns.
    const auto applySettingsAndRecheck = [&]() {
        applySettingsIfChanged();
        if (settingsWatcher.directories().isEmpty()) {
            settingsRescan.start();
        }
        armSettingsWatch();
    };

    QObject::connect(&settingsWatcher, &QFileSystemWatcher::directoryChanged,
                     [&](const QString &) { applySettingsAndRecheck(); });
    QObject::connect(&settingsRescan, &QTimer::timeout, [&]() { applySettingsAndRecheck(); });
    armSettingsWatch();
    if (settingsWatcher.directories().isEmpty()) {
        SUPERIOR_WARN(diag::categories::Ui).field("directory", settingsDirectory)
            << "cannot watch the settings directory; polling it instead";
        settingsRescan.start();
    }

    std::cout << "superior-ui: platform=" << QGuiApplication::platformName().toStdString()
              << " socket=" << socketPath.toStdString()
              << " skin=" << (activeSkin ? activeSkin->id : std::string("fallback"))
              << " candidates=" << (drawCandidates ? "on" : "off") << std::endl;

    const int result = QApplication::exec();
    instanceLock.unlock();
    return result;
}
