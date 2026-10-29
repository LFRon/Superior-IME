// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "superior/cloudpinyin/Cloud.hpp"

namespace superior::cloudpinyin {

/// Asynchronous HTTP for the cloud providers.
///
/// **The network runs on its own thread, never in Fcitx5's event loop.** The
/// first implementation drove `curl_multi` from the agent's 40 ms timer. Even
/// with `curl_multi`, name resolution and connection setup can block inside
/// `perform`, and that thread is also the one dispatching keystrokes. A client
/// that resets/forwards on every key (Telegram under XIM) turned that into
/// visible typing lag.
///
/// The thread is owned by this object and joined in the destructor, so it can
/// never outlive the addon library that Fcitx5 may `dlclose`. Nothing in the
/// thread calls back into Fcitx5: `submit()` is called from the agent thread,
/// `poll()` drains finished results there as well.
///
/// Usage:
///   fetcher.submit("nihao", backend);          // queues, never blocks
///   fetcher.inFlight()                          // true → keep the timer
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
    /// no endpoint, or libcurl/its worker is unavailable — callers treat that as
    /// "skip".
    bool submit(std::string pinyin, Backend backend);

    [[nodiscard]] bool inFlight() const noexcept;
    [[nodiscard]] std::size_t queueSize() const noexcept;

    /// Non-blocking. Returns every transfer that finished since the last call.
    std::vector<Result> poll();

    /// Drops all queued and in-flight transfers and their buffers.
    ///
    /// Safe to call from the agent thread. Transfers already completed but not
    /// yet polled are discarded as well; the generation counter is what keeps a
    /// result that races with the cancel from being returned later.
    void cancelAll();

    /// Largest body we will read. Providers answer in a few hundred bytes; the
    /// cap stops a hijacked endpoint from feeding us megabytes.
    static constexpr std::size_t kMaxBodyBytes = 8192;
    /// One request at a time is enough for typing speed, and keeps the number
    /// of sockets predictable.
    static constexpr std::size_t kMaxQueue = 2;

private:
    struct Transfer;
    struct Request {
        std::string pinyin;
        Backend backend = Backend::GoogleCn;
        std::uint64_t generation = 0;
    };

    void run();
    /// Creates one easy handle and adds it to `multi`. Worker thread only.
    bool startTransfer(void *multi, const Request &request);
    /// Removes and cleans every handle. Worker thread only.
    void cancelTransfers(void *multi);

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Request> requests_;
    /// Worker-owned; guarded by `mutex_` because `inFlight()`/`queueSize()` read
    /// it from the agent thread.
    std::vector<std::unique_ptr<Transfer>> transfers_;
    std::vector<Result> results_;
    std::thread worker_;
    bool stopping_ = false;
    bool cancelRequested_ = false;
    bool workerReady_ = false;
    std::atomic<bool> abortRequested_{false};
    /// Bumped by `cancelAll()`; results from an older generation are discarded.
    std::uint64_t generation_ = 0;
    int timeoutMs_;
};

} // namespace superior::cloudpinyin
