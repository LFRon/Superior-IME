// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "superior/api/Scene.hpp"
#include "superior/core/CandidateModel.hpp"

namespace superior::core {

/// Semantic state of one input session.
///
/// This is the single source of truth for what the user is composing and what
/// will be committed. It never touches windows, fonts, pixels or a clock.
class ImeSession {
public:
    explicit ImeSession(api::SessionKey key);

    [[nodiscard]] const api::SessionKey &key() const noexcept;

    void show();
    void hide();
    [[nodiscard]] bool visible() const noexcept;

    void setPreedit(api::Preedit preedit);
    void clearPreedit();
    [[nodiscard]] const api::Preedit &preedit() const noexcept;

    void setAnchor(api::Anchor anchor);
    [[nodiscard]] const api::Anchor &anchor() const noexcept;

    void setSkin(std::string skinId, api::SkinGeneration generation);
    void setStyle(std::string styleId);
    void setMode(std::string mode);

    void setStatus(std::vector<api::StatusItem> items);
    void setStatusPlacement(api::StatusPlacement placement);

    CandidateModel &candidates() noexcept;
    [[nodiscard]] const CandidateModel &candidates() const noexcept;

    bool selectNext();
    bool selectPrevious();
    bool pageNext();
    bool pagePrevious();
    bool select(api::CandidateId id);

    /// Commits a specific candidate and clears the composition.
    std::optional<std::string> commit(api::CandidateId id);
    std::optional<std::string> commitSelected();

    /// Page information owned by the engine. When set, it overrides the page
    /// numbers derived from the local candidate model, because Fcitx5 pages its
    /// list itself and only hands over the current slice.
    void setPageInfo(int pageIndex, int pageCount);
    void clearPageInfo();

    void setTransition(std::string motion, std::uint64_t startMs, double from = 0.0,
                       double to = 1.0);
    void clearTransition();

    [[nodiscard]] api::UiSceneFrame snapshot(api::FrameId frameId) const;

private:
    api::SessionKey key_;
    bool visible_ = false;
    api::Preedit preedit_{};
    api::Anchor anchor_{};
    std::string skinId_;
    api::SkinGeneration skinGeneration_ = 0;
    std::string styleId_;
    std::string mode_;
    CandidateModel candidates_;
    std::vector<api::StatusItem> statusItems_;
    api::StatusPlacement statusPlacement_ = api::StatusPlacement::BottomRight;
    std::optional<api::TransitionSpec> transition_;
    int pageIndex_ = 0;
    int pageCount_ = 0;
};

} // namespace superior::core
