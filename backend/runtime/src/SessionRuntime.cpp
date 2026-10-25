// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/runtime/SessionRuntime.hpp"

#include <cstdint>
#include <utility>

#include "superior/core/ImeSession.hpp"
#include "superior/diagnostics/Log.hpp"
#include "superior/runtime/SerialPump.hpp"

namespace superior::runtime {

namespace diag = ::superior::diagnostics;

struct SessionRuntime::Impl {
    Impl(api::SessionKey key, api::ScenePresenter &scenePresenter, api::Scheduler &serialScheduler)
        : session(std::move(key)), presenter(scenePresenter), scheduler(serialScheduler),
          pump(serialScheduler) {}

    core::ImeSession session;
    api::ScenePresenter &presenter;
    api::Scheduler &scheduler;
    SerialPump pump;

    api::FrameId nextFrameId = 1;
    std::uint64_t frameTimer = 0;
    bool framePending = false;
    bool dirty = false;
    bool stopped = false;

    void requestFrame() {
        if (stopped) {
            return;
        }
        if (framePending) {
            // Coalesce: the in-flight request will pick up the newest state.
            dirty = true;
            SUPERIOR_TRACE(diag::categories::Runtime) << "frame request coalesced";
            return;
        }
        framePending = true;
        dirty = true;
        frameTimer = scheduler.nextFrame([this](std::uint64_t timeMs) {
            pump.post([this, timeMs] {
                framePending = false;
                frameTimer = 0;
                present(timeMs);
            });
        });
    }

    void present(std::uint64_t /*timeMs*/) {
        if (stopped) {
            return;
        }
        dirty = false;
        const api::UiSceneFrame frame = session.snapshot(nextFrameId++);
        SUPERIOR_DEBUG(diag::categories::Runtime)
            .field("frame", static_cast<std::uint64_t>(frame.frameId))
            .field("session", static_cast<std::uint64_t>(frame.session.context))
            .field("visible", frame.panel.visible)
            .field("candidates", static_cast<std::uint64_t>(frame.panel.candidates.items.size()))
            << "presenting scene";
        presenter.present(frame);
        if (dirty) {
            requestFrame();
        }
    }

    void stop() {
        SUPERIOR_DEBUG(diag::categories::Runtime)
            .field("session", static_cast<std::uint64_t>(session.key().context))
            << "session runtime stopping";
        stopped = true;
        if (framePending && frameTimer != 0) {
            scheduler.cancel(frameTimer);
        }
        framePending = false;
        frameTimer = 0;
        dirty = false;
    }
};

SessionRuntime::SessionRuntime(api::SessionKey key, api::ScenePresenter &presenter,
                               api::Scheduler &scheduler)
    : impl_(std::make_unique<Impl>(std::move(key), presenter, scheduler)) {}

SessionRuntime::~SessionRuntime() {
    if (impl_) {
        impl_->stop();
    }
}

SessionRuntime::SessionRuntime(SessionRuntime &&) noexcept = default;
SessionRuntime &SessionRuntime::operator=(SessionRuntime &&) noexcept = default;

api::SessionKey SessionRuntime::key() const {
    return impl_->session.key();
}

void SessionRuntime::start() {
    markDirty();
}

void SessionRuntime::stop() {
    impl_->stop();
}

void SessionRuntime::post(std::function<void()> work) {
    impl_->pump.post(std::move(work));
}

void SessionRuntime::markDirty() {
    impl_->pump.post([impl = impl_.get()] { impl->requestFrame(); });
}

void SessionRuntime::show() {
    impl_->session.show();
    markDirty();
}

void SessionRuntime::hide() {
    impl_->session.hide();
    markDirty();
}

void SessionRuntime::setPreedit(api::Preedit preedit) {
    impl_->session.setPreedit(std::move(preedit));
    markDirty();
}

void SessionRuntime::setCandidates(std::vector<api::Candidate> candidates, int pageSize) {
    impl_->session.candidates().set(std::move(candidates), pageSize);
    markDirty();
}

void SessionRuntime::setAnchor(api::Anchor anchor) {
    impl_->session.setAnchor(anchor);
    markDirty();
}

void SessionRuntime::setSkin(std::string skinId, api::SkinGeneration generation) {
    impl_->session.setSkin(std::move(skinId), generation);
    markDirty();
}

void SessionRuntime::setStyle(std::string styleId) {
    impl_->session.setStyle(std::move(styleId));
    markDirty();
}

void SessionRuntime::setMode(std::string mode) {
    impl_->session.setMode(std::move(mode));
    markDirty();
}

void SessionRuntime::setStatus(std::vector<api::StatusItem> items) {
    impl_->session.setStatus(std::move(items));
    markDirty();
}

void SessionRuntime::setStatusPlacement(api::StatusPlacement placement) {
    impl_->session.setStatusPlacement(placement);
    markDirty();
}

void SessionRuntime::setTransition(std::string motion, std::uint64_t startMs, double from,
                                   double to) {
    impl_->session.setTransition(std::move(motion), startMs, from, to);
    markDirty();
}

bool SessionRuntime::select(api::CandidateId id) {
    const bool changed = impl_->session.select(id);
    if (changed) {
        markDirty();
    }
    return changed;
}

bool SessionRuntime::selectNext() {
    const bool changed = impl_->session.selectNext();
    if (changed) {
        markDirty();
    }
    return changed;
}

bool SessionRuntime::selectPrevious() {
    const bool changed = impl_->session.selectPrevious();
    if (changed) {
        markDirty();
    }
    return changed;
}

bool SessionRuntime::page(int delta) {
    bool changed = false;
    for (int i = 0; i < delta; ++i) {
        changed = impl_->session.pageNext() || changed;
    }
    for (int i = 0; i < -delta; ++i) {
        changed = impl_->session.pagePrevious() || changed;
    }
    if (changed) {
        markDirty();
    }
    return changed;
}

std::optional<std::string> SessionRuntime::commit(api::CandidateId id) {
    auto text = impl_->session.commit(id);
    if (text) {
        markDirty();
    }
    return text;
}

std::optional<std::string> SessionRuntime::commitSelected() {
    return commit(impl_->session.candidates().selectedId());
}

api::UiSceneFrame SessionRuntime::snapshot() const {
    return impl_->session.snapshot(0);
}

} // namespace superior::runtime
