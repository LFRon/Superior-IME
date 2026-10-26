// SPDX-License-Identifier: GPL-3.0-or-later
#include "OverlayController.h"

#include <fcitx-utils/log.h>

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <utility>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "superior/diagnostics/Log.hpp"
#include "superior/paths/Roots.hpp"
#include "superior/protocol/SceneServer.hpp"
#include "superior/protocol/Wire.hpp"

extern char **environ;

namespace superior::fcitx5 {

namespace diag = ::superior::diagnostics;

OverlayController::OverlayController(fcitx::Instance *instance) : instance_(instance) {}

OverlayController::~OverlayController() { shutdown(); }

void OverlayController::setCommandHandler(CommandHandler handler) {
    server_.setCommandHandler(std::move(handler));
}

void OverlayController::open(std::string_view socketPath) {
    std::string error;
    if (claim(socketPath, error)) {
        return;
    }
    SUPERIOR_WARN(diag::categories::Fcitx5)
        .field("path", std::string(socketPath))
        .field("error", error)
        << "cannot listen for the UI yet; retrying";
    retryLater(socketPath);
}

void OverlayController::retryLater(std::string_view socketPath) {
    if (instance_ == nullptr) {
        return;
    }
    retryTimer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 1000000, 0,
        [this, path = std::string(socketPath)](fcitx::EventSourceTime *, std::uint64_t) {
            std::string retryError;
            if (claim(path, retryError)) {
                ensure();
            } else {
                SUPERIOR_DEBUG(diag::categories::Fcitx5)
                    .field("error", retryError)
                    << "UI socket still busy";
            }
            return !retryStop_; // keep polling once a second
        });
}

bool OverlayController::claim(std::string_view socketPath, std::string &error) {
    // Ask the server itself, never a cached flag: a flag stays true after the
    // socket file has been removed or taken over, and then this returned success
    // for the rest of the session while every panel waited forever for a frame
    // that could not arrive.
    const std::string path(socketPath);
    if (server_.ownsSocketFile() && server_.socketPath() == path) {
        return true;
    }
    if (!server_.listen(path, error)) {
        return false;
    }
    // The timer keeps rescheduling itself while it runs; dropping the handle on
    // the next tick is enough, and `reset()` here would delete a live source.
    retryStop_ = true;
    SUPERIOR_INFO(diag::categories::Fcitx5).field("path", path) << "UI socket ready";
    return true;
}

bool OverlayController::ownsSocket() const { return server_.ownsSocketFile(); }

bool OverlayController::ownsSocket(std::string_view socketPath) const {
    return server_.ownsSocketFile() && server_.socketPath() == socketPath;
}

std::size_t OverlayController::clientCount() const { return server_.clientCount(); }

void OverlayController::publish(const std::string &payload) {
    server_.broadcast(protocol::encodeFrame(protocol::MessageType::Scene, payload));
}

bool OverlayController::reclaimStale() {
    // The runtime directory comes from `Superior::Paths`, never from `getenv`:
    // this file is the last place below `platforms/` that parsed it itself, and
    // a port replaces that one function instead of hunting for `getenv` calls.
    const auto runtime = paths::Roots::fromEnvironment().runtime;
    if (runtime.empty()) {
        return false;
    }
    const std::filesystem::path lockPath = runtime / "superior-ime-ui.lock";
    std::ifstream file(lockPath);
    if (!file) {
        return false;
    }
    // QLockFile's format: pid on the first line, then host and application name.
    long pid = 0;
    file >> pid;
    if (pid <= 0 || pid == static_cast<long>(::getpid())) {
        return false;
    }
    // Only signal a process that is really our overlay: a recycled PID must not
    // take a stray SIGTERM, and this file is user-writable.
    std::ifstream cmdline("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    std::string command;
    if (cmdline) {
        std::getline(cmdline, command, '\0');
    }
    if (command.find("superior_ui") == std::string::npos) {
        return false;
    }
    if (::kill(static_cast<pid_t>(pid), SIGTERM) != 0) {
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return true;
}

void OverlayController::ensure() {
    // The overlay can only be reached through our socket. Rebuild it first when
    // it is gone: another Fcitx5 replacing this one (`fcitx5 -r`) can remove the
    // file, and respawning the UI against a dead socket just produced the
    // "started the Superior overlay" / "stopped a stale overlay" loop with no
    // panel in between.
    if (!server_.ownsSocketFile()) {
        std::string error;
        if (!claim(protocol::SceneServer::defaultSocketPath(), error)) {
            // Someone else owns the UI right now. Their agent spawns the
            // overlay; taking it over here would fight it for the single
            // instance lock on every focus change.
            SUPERIOR_DEBUG(diag::categories::Fcitx5)
                .field("error", error)
                << "not touching the overlay while another agent owns the UI socket";
            return;
        }
    }

    if (server_.clientCount() > 0) {
        spawned_ = true;
        return;
    }

    if (const char *disabled = std::getenv("SUPERIOR_UI_AUTOSTART");
        disabled != nullptr && std::string_view(disabled) == "0") {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (spawned_ && now - lastAttempt_ < std::chrono::seconds(3)) {
        return;
    }
    // We already have a socket but nobody is connected: either the overlay just
    // started, or a *previous* one is wedged and still holds the single-instance
    // lock — then every respawn prints "already running" and we never get a
    // window again. Reclaim by stopping the holder, which is safe: the lock file
    // lives in our own runtime directory and records the process that took it.
    if (spawned_ && reclaimStale()) {
        SUPERIOR_WARN(diag::categories::Fcitx5)
            << "stopped a stale overlay that still held the single-instance lock";
    }
    lastAttempt_ = now;

    // Order matters: the per-user install keeps the executable under
    // ~/.local/share/superior-ime/bin (only a tiny wrapper sits in
    // ~/.local/bin), a system install puts it in /usr/bin, and the PATH lookup
    // is the last resort for hand-built trees.
    std::vector<std::string> candidates;
    if (const auto home = paths::Roots::fromEnvironment().home; !home.empty()) {
        candidates.push_back((home / ".local/share/superior-ime/bin/superior_ui").string());
        candidates.push_back((home / ".local/bin/superior_ui").string());
    }
    candidates.emplace_back("/usr/bin/superior_ui");
    candidates.emplace_back("superior_ui");

    for (const auto &binary : candidates) {
        pid_t pid = -1;
        std::string program = binary;
        std::vector<char *> argv{program.data(), nullptr};
        if (::posix_spawnp(&pid, program.c_str(), nullptr, nullptr, argv.data(), environ) == 0) {
            spawned_ = true;
            // Reap the child so a crashed overlay does not stay a zombie for the
            // lifetime of Fcitx5.
            //
            // The reaper is joinable, not detached: Fcitx5 may unload this addon,
            // and a detached thread executing code inside a dlclose'd library
            // would crash. The destructor stops and joins it first.
            reapers_.emplace_back([this, pid] {
                while (!reaperStop_.load(std::memory_order_relaxed)) {
                    int status = 0;
                    const pid_t result = ::waitpid(pid, &status, WNOHANG);
                    if (result != 0) {
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            });
            SUPERIOR_INFO(diag::categories::Fcitx5)
                .field("binary", binary)
                << "started the Superior overlay";
            return;
        }
    }
    SUPERIOR_WARN(diag::categories::Fcitx5)
        << "cannot start the overlay; run superior_ui manually or set SUPERIOR_UI_AUTOSTART=0";
}

void OverlayController::joinReapers() {
    reaperStop_.store(true, std::memory_order_relaxed);
    for (auto &reaper : reapers_) {
        if (reaper.joinable()) {
            reaper.join();
        }
    }
    reapers_.clear();
}

void OverlayController::shutdown() {
    retryStop_ = true;
    retryTimer_.reset();
    joinReapers();
    server_.close();
}

} // namespace superior::fcitx5
