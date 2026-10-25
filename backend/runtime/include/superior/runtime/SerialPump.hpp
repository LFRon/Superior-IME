// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <deque>
#include <functional>

#include "superior/api/Ports.hpp"

namespace superior::runtime {

/// Serializes work onto one owner thread.
///
/// Off-owner callers are forwarded through the scheduler; on-owner callers run
/// inline unless a drain is already in progress. Work posted from inside a
/// callback is appended and processed in the same drain, so an effect that
/// synchronously reports a result is ordered deterministically.
class SerialPump {
public:
    explicit SerialPump(api::Scheduler &scheduler);

    void post(std::function<void()> work);

    /// Owner thread only. Safe to call reentrantly; nested calls are no-ops.
    void drain();

    [[nodiscard]] bool draining() const noexcept;
    [[nodiscard]] std::size_t pending() const noexcept;

private:
    api::Scheduler &scheduler_;
    std::deque<std::function<void()>> queue_;
    bool draining_ = false;
};

} // namespace superior::runtime
