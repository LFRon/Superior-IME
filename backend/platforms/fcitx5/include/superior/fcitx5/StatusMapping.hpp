// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

#include "superior/api/Content.hpp"

namespace superior::fcitx5 {

/// Fcitx5 action names that must not reach the toolbar.
///
/// `pinyin-prediction` is pinyin's own on/off switch for prediction. It is a
/// configuration knob, not something you toggle while typing, and Sogou's
/// toolbar does not show it either.
[[nodiscard]] bool isStatusActionHidden(std::string_view actionName) noexcept;

/// Whether a status action's icon marks the feature as ON.
///
/// Some engine actions (the punctuation toggle) never report `isChecked`, but do
/// swap their icon between an `-active` and an `-inactive` variant. `inactive`
/// contains `active` as a substring, so this compares the suffix rather than
/// searching.
[[nodiscard]] bool statusIconIsOn(std::string_view icon) noexcept;

/// Draw role for a status action.
///
/// Everything unknown stays text: an addon we have never seen keeps working as
/// a labelled button instead of vanishing.
[[nodiscard]] api::StatusRole statusRoleFor(std::string_view actionName) noexcept;

} // namespace superior::fcitx5
