// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/cloudpinyin/Fetcher.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <mutex>

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

void cleanupMulti(void *handle) {
    if (handle != nullptr) {
        curl_multi_cleanup(static_cast<CURLM *>(handle));
    }
}

} // namespace

struct Fetcher::Transfer {
    CURL *easy = nullptr;
    std::string pinyin;
    std::string body;
    Backend backend = Backend::GoogleCn;
};

Fetcher::Fetcher(int timeoutMs)
    : multi_(nullptr, &cleanupMulti), timeoutMs_(std::max(100, timeoutMs)) {
    ensureCurlGlobal();
    CURLM *multi = curl_multi_init();
    if (multi == nullptr) {
        SUPERIOR_WARN(diag::categories::Cloud)
            << "libcurl multi handle unavailable; cloud pinyin disabled";
    }
    multi_.reset(multi);
}

Fetcher::~Fetcher() {
    cancelAll();
}

bool Fetcher::submit(std::string pinyin, Backend backend) {
    if (multi_ == nullptr) {
        return false;
    }
    if (transfers_.size() >= kMaxQueue) {
        return false;
    }
    const std::string url = requestUrl(backend, pinyin);
    if (url.empty()) {
        // Backend without a public endpoint (sogou): nothing to ask.
        return false;
    }

    auto transfer = std::make_unique<Transfer>();
    transfer->pinyin = std::move(pinyin);
    transfer->backend = backend;
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
        // No signals: we run inside Fcitx5's main thread.
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
        // Never follow a redirect: a hijacked host must not walk us elsewhere.
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_USERAGENT, "superior-ime/0.1") == CURLE_OK;
    if (!configured || curl_multi_add_handle(static_cast<CURLM *>(multi_.get()), easy) != CURLM_OK) {
        curl_easy_cleanup(easy);
        transfer->easy = nullptr;
        return false;
    }

    SUPERIOR_DEBUG(diag::categories::Cloud)
        .field("backend", std::string(backendName(backend)))
        .field("length", static_cast<std::int64_t>(transfer->pinyin.size()))
        .field("queued", static_cast<std::int64_t>(transfers_.size() + 1))
        << "cloud request queued"; // never log the pinyin itself
    transfers_.push_back(std::move(transfer));
    return true;
}

std::vector<Fetcher::Result> Fetcher::poll() {
    std::vector<Result> results;
    if (multi_ == nullptr || transfers_.empty()) {
        return results;
    }
    auto *multi = static_cast<CURLM *>(multi_.get());

    int running = 0;
    // Zero timeout: this runs on the UI thread, so it must never wait.
    curl_multi_perform(multi, &running);
    curl_multi_wait(multi, nullptr, 0, 0, nullptr);
    curl_multi_perform(multi, &running);

    int queued = 0;
    while (CURLMsg *message = curl_multi_info_read(multi, &queued)) {
        if (message->msg != CURLMSG_DONE) {
            continue;
        }
        CURL *easy = message->easy_handle;
        const auto it = std::find_if(transfers_.begin(), transfers_.end(),
                                     [easy](const std::unique_ptr<Transfer> &transfer) {
                                         return transfer->easy == easy;
                                     });
        if (it == transfers_.end()) {
            continue;
        }
        Transfer &transfer = **it;
        Result result;
        result.pinyin = transfer.pinyin;
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
            if (const auto text = parseTopCandidate(transfer.backend, transfer.body)) {
                result.text = *text;
            } else {
                // A 200 without a candidate is not an error: the provider simply
                // had nothing for this input. Keep it distinguishable in logs.
                result.ok = false;
            }
        }

        SUPERIOR_DEBUG(diag::categories::Cloud)
            .field("backend", std::string(backendName(transfer.backend)))
            .field("http", result.httpCode)
            .field("ok", result.ok)
            .field("networkError", result.networkError)
            .field("length", static_cast<std::int64_t>(result.pinyin.size()))
            << "cloud request finished";

        curl_multi_remove_handle(multi, easy);
        curl_easy_cleanup(easy);
        results.push_back(std::move(result));
        transfers_.erase(it);
    }
    return results;
}

void Fetcher::cancelAll() {
    if (multi_ == nullptr) {
        transfers_.clear();
        return;
    }
    auto *multi = static_cast<CURLM *>(multi_.get());
    for (auto &transfer : transfers_) {
        if (transfer->easy != nullptr) {
            curl_multi_remove_handle(multi, transfer->easy);
            curl_easy_cleanup(transfer->easy);
        }
    }
    transfers_.clear();
}

} // namespace superior::cloudpinyin
