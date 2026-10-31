// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace superior::ui::x11 {

/// One uniform X11 implementation. XWayland counts as X11.
///
/// Everything the X server can answer lives here: the resource database (which
/// is where a desktop publishes its scale) and window properties (which is how a
/// compositor convention is signalled). No desktop-specific code is needed for
/// placement itself — EWMH/ICCCM semantics are the same everywhere.

/// `Xft.dpi` from the root window's resource database, or 0 when unreadable.
///
/// Defined by `X11Scale.cpp` when XCB is available, and by a stub returning 0
/// otherwise: a build without libxcb-dev still produces a working overlay, it
/// just cannot ask X11 about scaling.
[[nodiscard]] int xftDpi() noexcept;

/// Writes the Treeland candidate-panel atom on `window`.
///
/// Returns false when there is no X connection or the property cannot be
/// written, so the caller keeps its own placement instead of waiting for a
/// compositor that will never answer.
bool adoptAsCandidatePanel(std::uintptr_t window) noexcept;

} // namespace superior::ui::x11
