// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>

#include "superior/api/Ids.hpp"

namespace superior::fcitx5 {

/// Maps the id carried by the scene (`index + 1`, see PanelMapper) back to the
/// index Fcitx5's `CandidateList` expects.
///
/// The id is 1-based because 0 is reserved for "nothing selected". Out-of-range
/// ids are rejected instead of clamped: a click on a stale frame must not
/// activate whatever happens to be at that position now.
[[nodiscard]] std::optional<int> candidateIndexFor(api::CandidateId id, int count) noexcept;

} // namespace superior::fcitx5
