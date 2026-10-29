// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/cloudpinyin/Fetcher.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <utility>

#include "superior/diagnostics/Log.hpp"

namespace superior::cloudpinyin {

namespace diag = ::superior::diagnostics;

namespace {

/// `curl_global_init` must run before any handle exists and only once.
/// Fcitx5 unloads addons, so a second load in the same process must not
/// re-initialise it.
void ensureCurlGlobal() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

/// Aborts an in-flight transfer once `Fetcher::cancelAll()` says so.
///
/// A free function keeps `curl_off_t` out of the header (the header must stay
/// free of `<curl/curl.h>`).
int xferInfo(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto *abort = static_cast<const std::atomic<bool> *>(clientp);
    return abort->load(std::memory_order_relaxed) ? 1 : 0;
}

std::size_t writeBody(char *data, std::size_t size, std::size_t count, void *userdata) {
    auto *body = static_cast<std::string *>(userdata);
    const std::size_t bytes = size * count;
    if (bytes == 0) {
        return 0;
    }
    // Refuse to grow past the cap: returning a short count aborts the transfer,
    // which is exactly what we want from an endpoint that misbehaves.
    if (body->size() + bytes > Fetcher::kMaxBodyBytes) {
        return 0;
    }
    body->append(data, bytes);
    return bytes;
}

} // namespace

struct Fetcher::Transfer {
    CURL *easy = nullptr;
    std::string pinyin;
    std::string body;
    Backend backend = Backend::GoogleCn;
    std::uint64_t generation = 0;
};

Fetcher::Fetcher(int timeoutMs) : timeoutMs_(std::max(100, timeoutMs)) {
    ensureCurlGlobal();
    // Set before starting the thread: `run()` may fail immediately (no
    // `curl_multi` handle) and publish its own "not ready"; the constructor must
    // not overwrite that with a stale true afterwards.
    workerReady_ = true;
    try {
        worker_ = std::thread([this] { run(); });
    } catch (...) {
        // No thread means cloud pinyin is unavailable, not that the whole agent
        // fails to load.
        workerReady_ = false;
    }
}

Fetcher::~Fetcher() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        requests_.clear();
        cancelRequested_ = true;
        abortRequested_.store(true, std::memory_order_relaxed);
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool Fetcher::submit(std::string pinyin, Backend backend) {
    if (backend == Backend::Sogou) {
        // No public endpoint: nothing to queue.
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!workerReady_ || stopping_) {
        return false;
    }
    if (requests_.size() + transfers_.size() >= kMaxQueue) {
        return false;
    }
    requests_.push_back(Request{std::move(pinyin), backend, generation_});
    wake_.notify_one();
    return true;
}

bool Fetcher::inFlight() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return !requests_.empty() || !transfers_.empty();
}

std::size_t Fetcher::queueSize() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_.size() + transfers_.size();
}

std::vector<Fetcher::Result> Fetcher::poll() {
    std::vector<Result> results;
    std::lock_guard<std::mutex> lock(mutex_);
    results.swap(results_);
    return results;
}

void Fetcher::cancelAll() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        requests_.clear();
        results_.clear();
        cancelRequested_ = true;
    }
    abortRequested_.store(true, std::memory_order_relaxed);
    wake_.notify_all();
}

bool Fetcher::startTransfer(void *multiPtr, const Request &request) {
    auto *multi = static_cast<CURLM *>(multiPtr);
    const std::string url = requestUrl(request.backend, request.pinyin);
    if (url.empty()) {
        return false;
    }

    auto transfer = std::make_unique<Transfer>();
    transfer->pinyin = request.pinyin;
    transfer->backend = request.backend;
    transfer->generation = request.generation;
    transfer->easy = curl_easy_init();
    if (transfer->easy == nullptr) {
        return false;
    }

    CURL *easy = transfer->easy;
    const bool configured =
        curl_easy_setopt(easy, CURLOPT_URL, url.c_str()) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &writeBody) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &transfer->body) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs_)) == CURLE_OK &&
        // No signals: the worker thread is not the main thread, but signals are
        // still process-wide and must not be used by a library.
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
        // Never follow a redirect: a hijacked host must not walk us elsewhere.
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_USERAGENT, "superior-ime/0.1") == CURLE_OK &&
        // Let `cancelAll()` interrupt an in-flight handshake or body read.
        curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, &xferInfo) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &abortRequested_) == CURLE_OK;
    if (!configured || curl_multi_add_handle(multi, easy) != CURLM_OK) {
        curl_easy_cleanup(easy);
        transfer->easy = nullptr;
        return false;
    }

    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // `cancelAll()` may have run while this handle was being configured.
        // The generation check is what makes a canceled request really stop
        // instead of merely hiding its result later.
        accepted = !stopping_ && !cancelRequested_ && request.generation == generation_;
        if (accepted) {
            transfers_.push_back(std::move(transfer));
        }
    }
    if (!accepted) {
        curl_multi_remove_handle(multi, easy);
        curl_easy_cleanup(easy);
        return false;
    }
    SUPERIOR_DEBUG(diag::categories::Cloud)
        .field("backend", std::string(backendName(request.backend)))
        .field("length", static_cast<std::int64_t>(request.pinyin.size()))
        << "cloud request queued"; // never log the pinyin itself
    return true;
}

void Fetcher::cancelTransfers(void *multiPtr) {
    auto *multi = static_cast<CURLM *>(multiPtr);
    std::vector<std::unique_ptr<Transfer>> doomed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        doomed.swap(transfers_);
    }
    for (auto &transfer : doomed) {
        if (transfer->easy != nullptr) {
            curl_multi_remove_handle(multi, transfer->easy);
            curl_easy_cleanup(transfer->easy);
            transfer->easy = nullptr;
        }
    }
    abortRequested_.store(false, std::memory_order_relaxed);
}

void Fetcher::run() {
    CURLM *multi = curl_multi_init();
    if (multi == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        workerReady_ = false;
        requests_.clear();
        return;
    }

    while (true) {
        std::deque<Request> incoming;
        bool cancel = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] {
                return stopping_ || cancelRequested_ || !requests_.empty() || !transfers_.empty();
            });
            if (stopping_ || cancelRequested_) {
                cancel = true;
                cancelRequested_ = false;
            }
            incoming.swap(requests_);
        }

        if (cancel) {
            // Requests swapped out just before the cancel belong to the old
            // generation; starting them now would occupy the only queue slot
            // and delay the query the user is actually typing.
            incoming.clear();
            cancelTransfers(multi);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_) {
                    break;
                }
            }
        }

        for (const Request &request : incoming) {
            static_cast<void>(startTransfer(multi, request));
        }

        bool hasTransfers = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hasTransfers = !transfers_.empty();
        }
        if (!hasTransfers) {
            continue;
        }

        int running = 0;
        curl_multi_perform(multi, &running);
        // Always wait, not only when `running` is non-zero: libcurl can report
        // zero running handles while a completed message is still queued (or
        // while a handle is being torn down). Skipping the wait there made this
        // loop spin at 100% of a core.
        curl_multi_wait(multi, nullptr, 0, 50, nullptr);
        curl_multi_perform(multi, &running);

        int queued = 0;
        while (CURLMsg *message = curl_multi_info_read(multi, &queued)) {
            if (message->msg != CURLMSG_DONE) {
                continue;
            }
            CURL *easy = message->easy_handle;
            std::unique_ptr<Transfer> transfer;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto it = std::find_if(
                    transfers_.begin(), transfers_.end(),
                    [easy](const std::unique_ptr<Transfer> &item) { return item->easy == easy; });
                if (it != transfers_.end()) {
                    transfer = std::move(*it);
                    transfers_.erase(it);
                }
            }
            if (!transfer) {
                curl_multi_remove_handle(multi, easy);
                curl_easy_cleanup(easy);
                continue;
            }

            Result result;
            result.pinyin = transfer->pinyin;
            result.ok = message->data.result == CURLE_OK;
            if (!result.ok) {
                // Distinguish "the host is unreachable" from "the host said no":
                // only the first one justifies parking the provider right away.
                switch (message->data.result) {
                case CURLE_COULDNT_RESOLVE_HOST:
                case CURLE_COULDNT_CONNECT:
                case CURLE_OPERATION_TIMEDOUT:
                case CURLE_COULDNT_RESOLVE_PROXY:
                    result.networkError = true;
                    break;
                default:
                    break;
                }
            }
            if (result.ok) {
                curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &result.httpCode);
                result.ok = result.httpCode == 200;
            }
            if (result.ok) {
                if (const auto text = parseTopCandidate(transfer->backend, transfer->body)) {
                    result.text = *text;
                } else {
                    // A 200 without a candidate is not an error: the provider simply
                    // had nothing for this input. Keep it distinguishable in logs.
                    result.ok = false;
                }
            }

            SUPERIOR_DEBUG(diag::categories::Cloud)
                .field("backend", std::string(backendName(transfer->backend)))
                .field("http", result.httpCode)
                .field("ok", result.ok)
                .field("networkError", result.networkError)
                .field("length", static_cast<std::int64_t>(result.pinyin.size()))
                << "cloud request finished";

            curl_multi_remove_handle(multi, easy);
            curl_easy_cleanup(easy);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                // A cancel bumped the generation; a result from the canceled
                // composition must never be inserted into a later one.
                if (transfer->generation == generation_ && !stopping_) {
                    results_.push_back(std::move(result));
                }
            }
        }
    }

    cancelTransfers(multi);
    curl_multi_cleanup(multi);
}

} // namespace superior::cloudpinyin
