// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/pinyin/CloudService.hpp"

#include <utility>

#include "superior/diagnostics/Log.hpp"

namespace superior::pinyin {

namespace diag = ::superior::diagnostics;

CloudService::CloudService() = default;

CloudService::~CloudService() {
    fetcher_.cancelAll();
}

void CloudService::setOptions(const api::PipelineOptions &options) {
    enabled_ = options.cloudEnabled;
    minLength_ = options.cloudMinLength;

    if (options.cloudBackend != backendName_) {
        backendName_ = options.cloudBackend;
        backend_ = cloudpinyin::backendFromString(backendName_)
                       .value_or(cloudpinyin::Backend::Auto);
        // An answer from one provider must not be served after switching to
        // another: the providers differ in ordering and in what they can read.
        cache_.clear();
        budget_.recordSuccess();
        SUPERIOR_INFO(diag::categories::Cloud)
            .field("backend", backendName_)
            << "cloud pinyin backend changed; cache cleared";
    }
    if (!enabled_) {
        cancel();
    }
}

std::optional<std::string> CloudService::find(std::string_view pinyin) {
    if (!enabled_ || pinyin.empty()) {
        return std::nullopt;
    }
    return cache_.find(pinyin);
}

bool CloudService::submit(std::string pinyin, std::uint64_t nowMs) {
    if (!enabled_ || pinyin.empty() ||
        static_cast<int>(pinyin.size()) < minLength_ ||
        pending_ == pinyin || !budget_.allowed(static_cast<std::int64_t>(nowMs))) {
        return false;
    }
    if (!fetcher_.submit(pinyin, backend_)) {
        return false;
    }
    pending_ = std::move(pinyin);
    return true;
}

void CloudService::poll(std::uint64_t nowMs) {
    for (auto &result : fetcher_.poll()) {
        if (result.pinyin == pending_) {
            pending_.clear();
        }
        if (result.ok && !result.text.empty()) {
            cache_.insert(result.pinyin, result.text);
            budget_.recordSuccess();
            SUPERIOR_TRACE(diag::categories::Cloud)
                .field("length", static_cast<std::int64_t>(result.text.size()))
                .field("http", result.httpCode)
                << "cloud candidate received";
        } else {
            // A connectivity failure counts as several: a network that is down
            // will not recover on the next keystroke.
            budget_.recordFailure(static_cast<std::int64_t>(nowMs), result.networkError ? 3 : 1);
            SUPERIOR_DEBUG(diag::categories::Cloud)
                .field("http", result.httpCode)
                .field("network", result.networkError)
                .field("errors", static_cast<std::int64_t>(budget_.consecutiveErrors()))
                << "cloud candidate failed";
        }
    }
}

void CloudService::cancel() {
    fetcher_.cancelAll();
    pending_.clear();
}

bool CloudService::busy() const noexcept {
    return fetcher_.inFlight();
}

} // namespace superior::pinyin
