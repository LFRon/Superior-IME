// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/fcitx5/StatusMapping.hpp"

namespace superior::fcitx5 {

namespace {
constexpr std::string_view kPredictionAction = "pinyin-prediction";
constexpr std::string_view kPunctuationAction = "punctuation";
} // namespace

bool isStatusActionHidden(std::string_view actionName) noexcept {
    return actionName == kPredictionAction || actionName == kToolbarToggleAction ||
           actionName == kSpeechToggleAction;
}

bool statusIconIsOn(std::string_view icon) noexcept {
    const auto endsWith = [icon](std::string_view suffix) {
        return icon.size() >= suffix.size() &&
               icon.substr(icon.size() - suffix.size()) == suffix;
    };
    // Check the OFF variant first: "fcitx-punc-inactive" also ends with the
    // last characters of "active" plus more, and a plain substring search would
    // report full width for half width.
    if (endsWith("-inactive") || icon == "inactive") {
        return false;
    }
    return endsWith("-active") || icon == "active";
}

api::StatusRole statusRoleFor(std::string_view actionName) noexcept {
    if (actionName == kPunctuationAction) {
        return api::StatusRole::Punctuation;
    }
    return api::StatusRole::Text;
}

} // namespace superior::fcitx5
