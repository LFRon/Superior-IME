// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "superior/api/Ports.hpp"
#include "superior/api/Scene.hpp"

namespace superior::runtime {

/// Binds one semantic session to a presenter and a scheduler.
///
/// Public surface deliberately exposes only `superior::api` types: the core
/// session implementation stays private, so downstream code cannot reach
/// around the runtime.
///
/// `markDirty()` coalesces presentation: at most one frame request is in
/// flight, and the frame is built from the latest state when the scheduler
/// delivers its timestamp. Semantic mutations are owner-thread operations.
///
/// Lifetime: the presenter and scheduler must outlive the runtime. Off-owner
/// work already handed to the scheduler is not cancelled by the destructor;
/// call stop() and let the owner drain before tearing down the scheduler.
class SessionRuntime {
public:
    SessionRuntime(api::SessionKey key, api::ScenePresenter &presenter,
                   api::Scheduler &scheduler);
    ~SessionRuntime();

    SessionRuntime(SessionRuntime &&) noexcept;
    SessionRuntime &operator=(SessionRuntime &&) noexcept;
    SessionRuntime(const SessionRuntime &) = delete;
    SessionRuntime &operator=(const SessionRuntime &) = delete;

    [[nodiscard]] api::SessionKey key() const;

    void start();
    void stop();

    /// Thread-safe ingress for producer callbacks.
    void post(std::function<void()> work);

    /// Requests at most one presentation for the current state.
    void markDirty();

    void show();
    void hide();
    void setPreedit(api::Preedit preedit);
    void setCandidates(std::vector<api::Candidate> candidates, int pageSize);
    void setAnchor(api::Anchor anchor);
    void setSkin(std::string skinId, api::SkinGeneration generation);
    void setStyle(std::string styleId);
    void setMode(std::string mode);
    void setStatus(std::vector<api::StatusItem> items);
    void setStatusPlacement(api::StatusPlacement placement);
    void setTransition(std::string motion, std::uint64_t startMs, double from, double to);

    bool select(api::CandidateId id);
    bool selectNext();
    bool selectPrevious();
    bool page(int delta);

    std::optional<std::string> commit(api::CandidateId id);
    std::optional<std::string> commitSelected();

    [[nodiscard]] api::UiSceneFrame snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace superior::runtime
