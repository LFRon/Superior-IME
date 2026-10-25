// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/runtime/SerialPump.hpp"

#include <utility>

#include "superior/diagnostics/Log.hpp"

namespace superior::runtime {

namespace {
class DrainGuard {
public:
    explicit DrainGuard(bool &flag) : flag_(flag) { flag_ = true; }
    ~DrainGuard() { flag_ = false; }
    DrainGuard(const DrainGuard &) = delete;
    DrainGuard &operator=(const DrainGuard &) = delete;

private:
    bool &flag_;
};
} // namespace

SerialPump::SerialPump(api::Scheduler &scheduler) : scheduler_(scheduler) {}

void SerialPump::post(std::function<void()> work) {
    if (!work) {
        return;
    }
    if (!scheduler_.onOwnerThread()) {
        scheduler_.post([this, work = std::move(work)]() mutable {
            post(std::move(work));
        });
        return;
    }

    queue_.push_back(std::move(work));
    if (!draining_) {
        drain();
    } else {
        SUPERIOR_TRACE(::superior::diagnostics::categories::Runtime)
            .field("pending", static_cast<std::uint64_t>(queue_.size()))
            << "work queued during drain";
    }
}

void SerialPump::drain() {
    if (draining_) {
        return;
    }
    const DrainGuard guard(draining_);
    while (!queue_.empty()) {
        auto work = std::move(queue_.front());
        queue_.pop_front();
        work();
    }
}

bool SerialPump::draining() const noexcept {
    return draining_;
}

std::size_t SerialPump::pending() const noexcept {
    return queue_.size();
}

} // namespace superior::runtime
