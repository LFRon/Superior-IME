// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

#include "superior/core/ImeSession.hpp"
#include "superior/fcitx5/PanelFacts.hpp"

namespace superior::fcitx5 {

/// Translates observed Fcitx5 state into the session's semantic state.
///
/// The mapper is pure: it never touches Fcitx5, a window or a clock. It is the
/// place where "what Fcitx5 is doing" becomes "what a shell should display".
class PanelMapper {
public:
    static void apply(const PanelFacts &facts, core::ImeSession &session,
                      std::string_view skinId = "org.superior.default",
                      api::SkinGeneration skinGeneration = 1);
};

} // namespace superior::fcitx5
