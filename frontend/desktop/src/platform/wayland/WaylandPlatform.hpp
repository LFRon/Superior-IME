// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace superior::ui::wayland {

/// Wayland has no single answer, so this layer is deliberately thin and the
/// per-desktop split lives in the session classification.
///
/// What *is* shared is the scale: `wl_output.scale` is core Wayland and every
/// compositor sends it. Placement is not shared — a Wayland client cannot know
/// where it sits on the desktop, and the only standard way to place a surface
/// against another application's caret is to be the input method
/// (`zwp_input_method_v2` + `zwp_input_popup_surface_v2`), which fcitx5 already
/// is. Treeland additionally accepts a toplevel tag, but Qt does not expose the
/// `xdg_toplevel` it created, so the overlay cannot use it from here.

/// Largest `wl_output.scale` the compositor reports, or 0 when unavailable.
[[nodiscard]] int outputScale() noexcept;

} // namespace superior::ui::wayland
