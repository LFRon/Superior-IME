// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/cloudpinyin/CloudState.hpp"

#include <algorithm>

namespace superior::cloudpinyin {

AnswerCache::AnswerCache(std::size_t capacity) : capacity_(std::max<std::size_t>(1, capacity)) {}

std::optional<std::string> AnswerCache::find(std::string_view pinyin) {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [pinyin](const Entry &entry) { return entry.first == pinyin; });
    if (it == entries_.end()) {
        return std::nullopt;
    }
    // Refresh recency: move the hit to the front.
    Entry entry = std::move(*it);
    entries_.erase(it);
    entries_.push_front(std::move(entry));
    return entries_.front().second;
}

void AnswerCache::insert(std::string pinyin, std::string text) {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&pinyin](const Entry &entry) {
                                     return entry.first == pinyin;
                                 });
    if (it != entries_.end()) {
        entries_.erase(it);
    }
    entries_.emplace_front(std::move(pinyin), std::move(text));
    while (entries_.size() > capacity_) {
        entries_.pop_back();
    }
}

void AnswerCache::clear() {
    entries_.clear();
}

ErrorBudget::ErrorBudget(int maxConsecutiveErrors, std::int64_t cooldownMs)
    : maxConsecutiveErrors_(std::max(1, maxConsecutiveErrors)),
      cooldownMs_(std::max<std::int64_t>(0, cooldownMs)) {}

void ErrorBudget::recordSuccess() noexcept {
    consecutiveErrors_ = 0;
    blocked_ = false;
    blockedUntilMs_ = 0;
}

void ErrorBudget::recordFailure(std::int64_t nowMs, int weight) noexcept {
    if (blocked_) {
        return;
    }
    consecutiveErrors_ += std::max(1, weight);
    if (consecutiveErrors_ >= maxConsecutiveErrors_) {
        blocked_ = true;
        blockedUntilMs_ = nowMs + cooldownMs_;
    }
}

bool ErrorBudget::allowed(std::int64_t nowMs) noexcept {
    if (!blocked_) {
        return true;
    }
    if (nowMs < blockedUntilMs_) {
        return false;
    }
    // Cooldown expired: give the endpoint one more chance, with a clean slate.
    blocked_ = false;
    blockedUntilMs_ = 0;
    consecutiveErrors_ = 0;
    return true;
}

} // namespace superior::cloudpinyin
