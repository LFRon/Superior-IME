// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/core/ImeSession.hpp"

#include <algorithm>
#include <utility>

#include "superior/diagnostics/Log.hpp"

namespace superior::core {

namespace diag = ::superior::diagnostics;

ImeSession::ImeSession(api::SessionKey key) : key_(key) {}

const api::SessionKey &ImeSession::key() const noexcept {
    return key_;
}

void ImeSession::show() {
    visible_ = true;
}

void ImeSession::hide() {
    visible_ = false;
}

bool ImeSession::visible() const noexcept {
    return visible_;
}

void ImeSession::setPreedit(api::Preedit preedit) {
    preedit_ = std::move(preedit);
}

void ImeSession::clearPreedit() {
    preedit_ = api::Preedit{};
}

const api::Preedit &ImeSession::preedit() const noexcept {
    return preedit_;
}

void ImeSession::setAnchor(api::Anchor anchor) {
    anchor_ = anchor;
}

const api::Anchor &ImeSession::anchor() const noexcept {
    return anchor_;
}

void ImeSession::setSkin(std::string skinId, api::SkinGeneration generation) {
    skinId_ = std::move(skinId);
    skinGeneration_ = generation;
}

void ImeSession::setStyle(std::string styleId) {
    styleId_ = std::move(styleId);
}

void ImeSession::setMode(std::string mode) {
    mode_ = std::move(mode);
}

void ImeSession::setStatus(std::vector<api::StatusItem> items) {
    statusItems_ = std::move(items);
}

void ImeSession::setStatusPlacement(api::StatusPlacement placement) {
    statusPlacement_ = placement;
}

CandidateModel &ImeSession::candidates() noexcept {
    return candidates_;
}

const CandidateModel &ImeSession::candidates() const noexcept {
    return candidates_;
}

bool ImeSession::selectNext() {
    return candidates_.selectNext();
}

bool ImeSession::selectPrevious() {
    return candidates_.selectPrevious();
}

bool ImeSession::pageNext() {
    return candidates_.nextPage();
}

bool ImeSession::pagePrevious() {
    return candidates_.previousPage();
}

bool ImeSession::select(api::CandidateId id) {
    return candidates_.select(id);
}

std::optional<std::string> ImeSession::commit(api::CandidateId id) {
    const api::Candidate *candidate = candidates_.find(id);
    if (candidate == nullptr) {
        // Unknown or stale id: never changes state, so an in-flight click from
        // an older scene cannot commit the wrong text.
        SUPERIOR_DEBUG(diag::categories::Core)
            .field("session", static_cast<std::uint64_t>(key_.context))
            .field("candidate", static_cast<std::uint64_t>(id))
            << "commit rejected: unknown candidate";
        return std::nullopt;
    }
    std::string text = candidate->text;
    SUPERIOR_INFO(diag::categories::Core)
        .field("session", static_cast<std::uint64_t>(key_.context))
        .field("candidate", static_cast<std::uint64_t>(id))
        .redacted("text", text)
        << "candidate committed";
    candidates_.clear();
    clearPreedit();
    clearPageInfo();
    visible_ = false;
    clearTransition();
    return text;
}

std::optional<std::string> ImeSession::commitSelected() {
    return commit(candidates_.selectedId());
}

void ImeSession::setPageInfo(int pageIndex, int pageCount) {
    pageIndex_ = pageIndex;
    pageCount_ = pageCount;
}

void ImeSession::clearPageInfo() {
    pageIndex_ = 0;
    pageCount_ = 0;
}

void ImeSession::setTransition(std::string motion, std::uint64_t startMs, double from, double to) {
    api::TransitionSpec transition;
    transition.motion = std::move(motion);
    transition.startMs = startMs;
    transition.fromOpacity = from;
    transition.toOpacity = to;
    transition_ = std::move(transition);
}

void ImeSession::clearTransition() {
    transition_.reset();
}

api::UiSceneFrame ImeSession::snapshot(api::FrameId frameId) const {
    api::UiSceneFrame frame;
    frame.frameId = frameId;
    frame.session = key_;

    frame.panel.visible =
        visible_ && (!preedit_.text.empty() || !candidates_.empty());
    frame.panel.anchor = anchor_;
    frame.panel.skinId = skinId_;
    frame.panel.skinGeneration = skinGeneration_;
    frame.panel.styleId = styleId_;
    frame.panel.mode = mode_;
    frame.panel.preedit = preedit_;
    frame.panel.candidates = candidates_.page();
    if (pageCount_ > 0) {
        frame.panel.candidates.pageIndex = std::max(0, pageIndex_ - 1);
        frame.panel.candidates.pageCount = pageCount_;
        frame.panel.candidates.hasPrevious = pageIndex_ > 1;
        frame.panel.candidates.hasNext = pageIndex_ < pageCount_;
    }
    frame.panel.selected = candidates_.selectedId();

    frame.status.visible = !statusItems_.empty();
    frame.status.placement = statusPlacement_;
    frame.status.skinId = skinId_;
    frame.status.skinGeneration = skinGeneration_;
    frame.status.items = statusItems_;

    frame.transition = transition_;
    return frame;
}

} // namespace superior::core
