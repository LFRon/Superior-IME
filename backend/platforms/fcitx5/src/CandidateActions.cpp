// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/fcitx5/CandidateActions.hpp"

namespace superior::fcitx5 {

std::optional<int> candidateIndexFor(api::CandidateId id, int count) noexcept {
    if (count <= 0 || id == 0) {
        return std::nullopt;
    }
    const int index = static_cast<int>(id) - 1;
    if (index < 0 || index >= count) {
        return std::nullopt;
    }
    return index;
}

} // namespace superior::fcitx5
