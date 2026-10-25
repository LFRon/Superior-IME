// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace superior::cloudpinyin {

/// Small LRU of answers, so retyping a word does not hit the network again.
///
/// Bounded on purpose: cloud pinyin is a convenience, and an unbounded cache
/// would quietly grow for the whole session. `capacity` entries is plenty for
/// the words a person retypes in a row.
class AnswerCache {
public:
    explicit AnswerCache(std::size_t capacity = 256);

    [[nodiscard]] std::optional<std::string> find(std::string_view pinyin);
    void insert(std::string pinyin, std::string text);
    /// Drops everything. Used when the provider changes: an answer from Google
    /// must not be served after switching to Baidu.
    void clear();
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    using Entry = std::pair<std::string, std::string>;
    std::size_t capacity_;
    std::deque<Entry> entries_; // newest first
};

/// Decides when to stop asking.
///
/// Mirrors upstream behaviour: a handful of consecutive failures turns cloud
/// pinyin off for a while instead of hammering a dead endpoint on every
/// keystroke. One success restores it immediately.
class ErrorBudget {
public:
    ErrorBudget(int maxConsecutiveErrors = 10, std::int64_t cooldownMs = 300000);

    void recordSuccess() noexcept;
    /// `weight` lets a connectivity failure count as several: "no network" will
    /// not fix itself on the next keystroke, so the provider is parked quickly
    /// instead of after a handful of pointless timeouts.
    void recordFailure(std::int64_t nowMs, int weight = 1) noexcept;
    [[nodiscard]] bool allowed(std::int64_t nowMs) noexcept;
    [[nodiscard]] int consecutiveErrors() const noexcept { return consecutiveErrors_; }
    [[nodiscard]] int maxConsecutiveErrors() const noexcept { return maxConsecutiveErrors_; }

private:
    int maxConsecutiveErrors_;
    std::int64_t cooldownMs_;
    int consecutiveErrors_ = 0;
    /// Zero while healthy; otherwise when asking becomes allowed again.
    std::int64_t blockedUntilMs_ = 0;
    /// While blocked, the failures that caused it are kept; clearing them on
    /// every rejected attempt would let one keystroke end the cooldown early.
    bool blocked_ = false;
};

} // namespace superior::cloudpinyin
