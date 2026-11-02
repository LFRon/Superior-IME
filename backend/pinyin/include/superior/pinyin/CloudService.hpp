// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "superior/api/Composition.hpp"
#include "superior/cloudpinyin/Cloud.hpp"
#include "superior/cloudpinyin/CloudState.hpp"
#include "superior/cloudpinyin/Fetcher.hpp"

namespace superior::pinyin {

/// The cloud candidate source, shared by every composer.
///
/// One network worker per process, not per input context: a typing burst must
/// not multiply sockets, and the answer cache is useful across windows. The
/// class has no UI and no platform dependency; it only knows pinyin, HTTP and
/// the error budget.
class CloudService {
public:
    CloudService();
    ~CloudService();

    CloudService(const CloudService &) = delete;
    CloudService &operator=(const CloudService &) = delete;

    void setOptions(const api::PipelineOptions &options);

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] int minLength() const noexcept { return minLength_; }

    [[nodiscard]] std::optional<std::string> find(std::string_view pinyin);

    /// Queues one request. Returns false when disabled, too short, in cooldown,
    /// already pending, or the worker queue is full.
    bool submit(std::string pinyin, std::uint64_t nowMs);

    /// Drains finished transfers into the cache and the error budget.
    void poll(std::uint64_t nowMs);

    void cancel();

    [[nodiscard]] bool busy() const noexcept;

private:
    bool enabled_ = false;
    int minLength_ = 2;
    std::string backendName_ = "auto";
    cloudpinyin::Backend backend_ = cloudpinyin::Backend::Auto;
    cloudpinyin::Fetcher fetcher_;
    cloudpinyin::AnswerCache cache_{256};
    cloudpinyin::ErrorBudget budget_{3, 60000};
    std::string pending_;
};

} // namespace superior::pinyin
