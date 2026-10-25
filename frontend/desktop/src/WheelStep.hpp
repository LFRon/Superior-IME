// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace superior::ui {

/// Converts a wheel angle delta into a page step; positive means "next page".
///
/// One notch is 120 units on X11/Wayland. Trackpads emit many small deltas, so
/// anything below a notch is ignored instead of paging on every event — that
/// flickered through several pages per gesture.
///
/// Lives in the UI layer on purpose: "what a wheel means" is input handling,
/// while "which page is valid" stays with the engine.
[[nodiscard]] int pageStepForWheel(int angleDelta) noexcept;

} // namespace superior::ui
