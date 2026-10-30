// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace superior::ui::detail {

/// `Xft.dpi` from the X resource database, or 0 when it cannot be read.
///
/// Defined by `ScreenScaleX11.cpp` when XCB is available, and by a stub that
/// returns 0 otherwise: a build without libxcb-dev still has a working overlay,
/// it just cannot ask X11 about scaling.
[[nodiscard]] int x11XftDpi() noexcept;

/// The largest `wl_output.scale` the compositor reports, or 0 when the session
/// is not Wayland or no connection could be made.
///
/// Defined by `ScreenScaleWayland.cpp` when libwayland-client is available,
/// and by a stub that returns 0 otherwise.
[[nodiscard]] int waylandOutputScale() noexcept;

} // namespace superior::ui::detail
