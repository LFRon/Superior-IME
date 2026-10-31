// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if defined(SUPERIOR_HAVE_XCB_SCALE) || defined(SUPERIOR_HAVE_XCB_PANEL)
#define SUPERIOR_HAVE_XCB 1
#include <xcb/xcb.h>
#endif

namespace superior::ui::x11 {

#if defined(SUPERIOR_HAVE_XCB)
/// The overlay's single X11 connection, opened on first use and closed at exit.
///
/// Shared rather than opened per call: the scale is read once at start-up and
/// the panel atom is written once per window, so a second socket would be paid
/// for twice and used twice.
///
/// Returns nullptr when X11 is unreachable (or when this build has no libxcb
/// headers); every caller treats that as "X11 cannot answer".
[[nodiscard]] xcb_connection_t *connection() noexcept;
#endif

} // namespace superior::ui::x11
