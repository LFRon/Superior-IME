// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "superior/cloudpinyin/Cloud.hpp"

namespace superior::cloudpinyin {

/// Asynchronous HTTP for the cloud providers, driven by polling.
///
/// Deliberately **not** thread based. Fcitx5 can unload this addon at any time,
/// and a thread that outlives the unload would run code from unmapped memory —
/// the same hazard we removed from the overlay reaper. A `curl_multi` handle
/// pumped from an existing event timer has no such problem: all sockets and
/// buffers belong to the caller's event loop.
///
/// Usage:
///   fetcher.submit("nihao", backend);          // starts, never blocks
///   fetcher.inFlight()                          // true → run the timer
///   for (auto &result : fetcher.poll()) ...     // drain finished transfers
class Fetcher {
public:
    struct Result {
        std::string pinyin;
        /// Extracted candidate; empty when the provider had no answer.
        std::string text;
        long httpCode = 0;
        /// False on transport failure, non-200, or unparsable body.
        bool ok = false;
        /// True when the failure was "cannot reach the host" (DNS, connect,
        /// timeout). The caller uses this to park the provider immediately: a
        /// network that is down will not come back on the next keystroke.
        bool networkError = false;
    };

    explicit Fetcher(int timeoutMs = 3000);
    ~Fetcher();

    Fetcher(const Fetcher &) = delete;
    Fetcher &operator=(const Fetcher &) = delete;

    /// Queues one request. Returns false when the queue is full, the backend has
    /// no endpoint, or libcurl is unavailable — callers treat that as "skip".
    bool submit(std::string pinyin, Backend backend);

    [[nodiscard]] bool inFlight() const noexcept { return !transfers_.empty(); }
    [[nodiscard]] std::size_t queueSize() const noexcept { return transfers_.size(); }

    /// Non-blocking. Returns every transfer that finished since the last call.
    std::vector<Result> poll();

    /// Drops all in-flight transfers and their buffers.
    void cancelAll();

    /// Largest body we will read. Providers answer in a few hundred bytes; the
    /// cap stops a hijacked endpoint from feeding us megabytes.
    static constexpr std::size_t kMaxBodyBytes = 8192;
    /// One request at a time is enough for typing speed, and keeps the number
    /// of sockets predictable.
    static constexpr std::size_t kMaxQueue = 2;

private:
    struct Transfer;

    /// Reference so the header stays free of `<curl/curl.h>`.
    std::unique_ptr<void, void (*)(void *)> multi_;
    std::vector<std::unique_ptr<Transfer>> transfers_;
    int timeoutMs_;
};

} // namespace superior::cloudpinyin
