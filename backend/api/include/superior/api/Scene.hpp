// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "superior/api/Content.hpp"
#include "superior/api/Ids.hpp"

namespace superior::api {

/// Logical pixels. The renderer applies the output scale; the agent never
/// computes physical coordinates.
struct Rect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    [[nodiscard]] constexpr bool empty() const noexcept {
        return width <= 0.0 || height <= 0.0;
    }

    friend constexpr bool operator==(const Rect &, const Rect &) = default;
};

/// Where the panel should be placed. `cursor` is the anchor reported by the
/// platform. `offset` is a user drag offset persisted per application.
struct Anchor {
    Rect cursor;
    double offsetX = 0.0;
    double offsetY = 0.0;
    bool valid = false;
};

enum class HitKind {
    None,
    Candidate,
    PagePrevious,
    PageNext,
    StatusItem,
    DragHandle,
};

/// A hit region in the same logical coordinate space as the panel. Regions are
/// ordered; the first containing region wins. UI reports the semantic identity
/// of the region, never raw coordinates.
struct HitRegion {
    Rect bounds;
    HitKind kind = HitKind::None;
    CandidateId candidate = 0;
    std::string action;
};

struct HitMap {
    std::vector<HitRegion> regions;
};

/// Panel content plus the metadata required to render it deterministically.
/// This is a *complete* desired scene, not a delta: dropping a frame is safe.
struct PanelScene {
    bool visible = false;
    Anchor anchor;
    double scale = 1.0;

    std::string skinId;
    SkinGeneration skinGeneration = 0;
    std::string styleId;
    std::string mode;

    Preedit preedit;
    CandidatePage candidates;
    CandidateId selected = 0;

    std::optional<Rect> dragBounds;
};

enum class StatusPlacement {
    BottomRight,
    BottomLeft,
    TopRight,
    TopLeft,
};

struct StatusScene {
    bool visible = false;
    StatusPlacement placement = StatusPlacement::BottomRight;
    std::string skinId;
    SkinGeneration skinGeneration = 0;
    std::vector<StatusItem> items;
};

/// Animation is expressed as data so the UI can sample it on its own vsync.
/// The agent supplies stable timing (monotonic milliseconds); it never sends
/// one message per animation frame.
struct TransitionSpec {
    std::string motion;
    std::uint64_t startMs = 0;
    double fromOpacity = 1.0;
    double toOpacity = 1.0;
};

struct UiSceneFrame {
    FrameId frameId = 0;
    SessionKey session;
    PanelScene panel;
    StatusScene status;
    std::optional<TransitionSpec> transition;
};

} // namespace superior::api
