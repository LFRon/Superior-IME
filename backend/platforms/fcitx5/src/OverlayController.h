// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <fcitx-utils/event.h>
#include <fcitx/instance.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "superior/protocol/SceneServer.hpp"

namespace superior::fcitx5 {

/// Owns the overlay process and the socket it talks to.
///
/// Split out of the agent (P5.4) because this is process supervision, not input
/// policy: nothing here decides what the panel shows, it only makes sure there
/// is a panel listening. Everything that can fail — a stale socket left by a
/// previous Fcitx5, an overlay that still holds the single-instance lock, a
/// crashed child that must be reaped — is handled here so the agent never has to
/// mix "should this frame be drawn" with "is there anyone to draw it".
class OverlayController {
public:
    using CommandHandler = std::function<void(std::string)>;

    explicit OverlayController(fcitx::Instance *instance);
    ~OverlayController();

    OverlayController(const OverlayController &) = delete;
    OverlayController &operator=(const OverlayController &) = delete;

    void setCommandHandler(CommandHandler handler);

    /// Takes the UI socket, retrying once a second while another instance still
    /// holds it.
    ///
    /// A failed bind used to be permanent, which is how a Fcitx5 handover could
    /// leave the overlay dead for the rest of the session. The other instance is
    /// usually on its way out, so waiting is the right answer.
    void open(std::string_view socketPath);

    /// Creates the socket if we do not already own it.
    ///
    /// Returns false with `error` filled when somebody else owns it; the caller
    /// then leaves the overlay alone, because that agent spawns it.
    bool claim(std::string_view socketPath, std::string &error);

    [[nodiscard]] bool ownsSocket() const;
    [[nodiscard]] bool ownsSocket(std::string_view socketPath) const;
    [[nodiscard]] std::size_t clientCount() const;

    /// Spawns the overlay when nobody is connected, at most once every 3 s.
    void ensure();

    /// Sends one scene frame to every connected overlay.
    void publish(const std::string &payload);

    /// Stops retrying, reaps every spawned overlay and closes the socket.
    ///
    /// Called from the destructor; explicit so the agent can drop the socket
    /// before libcurl's handles go away.
    void shutdown();

private:
    /// Kills an overlay that holds the single-instance lock but never connected.
    bool reclaimStale();
    void retryLater(std::string_view socketPath);
    void joinReapers();

    fcitx::Instance *instance_ = nullptr;
    protocol::SceneServer server_;
    bool spawned_ = false;
    std::chrono::steady_clock::time_point lastAttempt_{};
    std::unique_ptr<fcitx::EventSourceTime> retryTimer_;
    bool retryStop_ = false;
    std::vector<std::thread> reapers_;
    std::atomic<bool> reaperStop_{false};
};

} // namespace superior::fcitx5
