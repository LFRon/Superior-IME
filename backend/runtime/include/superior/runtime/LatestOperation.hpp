// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>

namespace superior::runtime {

/// Tracks at most one in-flight asynchronous operation and rejects results
/// belonging to an older one.
///
/// This is the C++ port of the ticket pattern used by MiFreeForm:
/// - begin() supersedes the previous ticket and cancels its resource;
/// - complete() reports whether the ticket is still the active one;
/// - attach() tolerates both orders: a callback may fire before the platform
///   API returns its cancellation handle.
template <typename Resource>
class LatestOperation {
public:
    struct Ticket {
        std::uint64_t id = 0;

        [[nodiscard]] bool valid() const noexcept { return id != 0; }
    };

    explicit LatestOperation(std::function<void(Resource &)> cancelResource = {})
        : cancel_(std::move(cancelResource)) {}

    ~LatestOperation() { cancel(); }

    LatestOperation(const LatestOperation &) = delete;
    LatestOperation &operator=(const LatestOperation &) = delete;
    LatestOperation(LatestOperation &&) = default;
    LatestOperation &operator=(LatestOperation &&) = default;

    Ticket begin() {
        retireActive();

        const std::uint64_t id = nextId_++;
        slots_[id] = Slot{State::Pending, std::nullopt};
        active_ = id;
        return Ticket{id};
    }

    void attach(Ticket ticket, Resource resource) {
        const auto it = slots_.find(ticket.id);
        if (it == slots_.end()) {
            return;
        }
        switch (it->second.state) {
        case State::Pending:
            it->second.resource = std::move(resource);
            return;
        case State::Cancelled:
            release(resource);
            slots_.erase(it);
            return;
        case State::Completed:
            slots_.erase(it);
            return;
        }
    }

    [[nodiscard]] bool complete(Ticket ticket) {
        const auto it = slots_.find(ticket.id);
        if (it == slots_.end() || it->second.state != State::Pending) {
            return false;
        }
        it->second.state = State::Completed;
        it->second.resource.reset();
        if (active_ == ticket.id) {
            active_ = 0;
        }
        return true;
    }

    [[nodiscard]] bool owns(Ticket ticket) const {
        if (active_ != ticket.id) {
            return false;
        }
        const auto it = slots_.find(ticket.id);
        return it != slots_.end() && it->second.state == State::Pending;
    }

    void cancel() {
        retireActive();
    }

    [[nodiscard]] bool active() const noexcept { return active_ != 0; }

private:
    enum class State { Pending, Completed, Cancelled };
    struct Slot {
        State state = State::Pending;
        std::optional<Resource> resource;
    };

    void retireActive() {
        if (active_ == 0) {
            return;
        }
        const auto it = slots_.find(active_);
        if (it != slots_.end() && it->second.state == State::Pending) {
            it->second.state = State::Cancelled;
            if (it->second.resource) {
                // The handle already exists: release it now and drop the slot.
                release(*it->second.resource);
                it->second.resource.reset();
                slots_.erase(it);
            }
            // Otherwise keep the cancelled slot so a late attach() still
            // releases the handle the platform returns after the callback.
        }
        active_ = 0;
    }

    void release(Resource &resource) {
        if (cancel_) {
            cancel_(resource);
        }
    }

    std::function<void(Resource &)> cancel_;
    std::map<std::uint64_t, Slot> slots_;
    std::uint64_t active_ = 0;
    std::uint64_t nextId_ = 1;
};

} // namespace superior::runtime
