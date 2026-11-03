// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <fcitx/inputpanel.h>

#include <utility>

namespace superior::fcitx5 {

/// Clears the panel without losing the host's custom callbacks.
///
/// `fcitx::InputPanel::reset()` also drops `customInputPanelCallback` and
/// `customVirtualKeyboardCallback`. Losing the first one hands the panel back to
/// ClassicUI, which then draws its own candidate window with its own theme next
/// to ours — two skins for one composition. Every reset in this host therefore
/// has to go through here, and `InputPanel::reset()` must never be called
/// directly.
inline void resetPanelKeepingCallbacks(fcitx::InputPanel &panel) {
    auto custom = panel.customInputPanelCallback();
    auto virtualKeyboard = panel.customVirtualKeyboardCallback();
    panel.reset();
    panel.setCustomInputPanelCallback(std::move(custom));
    panel.setCustomVirtualKeyboardCallback(std::move(virtualKeyboard));
}

} // namespace superior::fcitx5
