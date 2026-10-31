// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

#include "Scale.hpp"
#include "Session.hpp"

namespace superior::ui {

/// Why a compositor cannot place our panel, when it cannot.
enum class PanelChannel {
    /// The window system has a working convention and we applied it.
    Applied,
    /// The desktop has no such convention (GNOME, KDE, plain X11, …).
    Unsupported,
    /// The desktop has one, but it is not reachable from this window system.
    Unreachable,
};

/// The Linux platform layer: two questions, one place to answer them.
///
/// `frontend/desktop` may only ask these three things — the window system, the
/// scale factor, and whether the compositor can place the candidate panel. That
/// keeps every platform API (XCB, libwayland, a desktop's private convention) in
/// `platform/`, and keeps the windows themselves free of `#ifdef`s.
///
/// Layout of the implementation mirrors the only real split on Linux:
///
/// - `platform/x11/`     — one uniform implementation. XWayland counts as X11:
///                         same pixels, same coordinates, same scale source.
/// - `platform/wayland/` — one implementation per desktop, because "place a
///                         window next to the caret" has no shared answer.
[[nodiscard]] Session detectSession() noexcept;

/// The desktop's scale factor, read from that window system's own declaration.
[[nodiscard]] SystemScale readSystemScale(WindowSystem system) noexcept;

/// True when the compositor can position the panel itself, i.e. when
/// `adoptAsCandidatePanel()` has a convention to apply.
[[nodiscard]] bool compositorPlacesCandidatePanel(const Session &session) noexcept;

/// Publishes the panel hint for `window` and reports what happened.
///
/// Safe to call on any session: a desktop without a convention returns
/// `Unsupported` (or `Unreachable`) and the caller keeps its own placement.
[[nodiscard]] PanelChannel adoptAsCandidatePanel(const Session &session,
                                                 std::uintptr_t window) noexcept;

/// Human-readable reason for a non-`Applied` channel, for the log.
[[nodiscard]] std::string_view panelChannelReason(const Session &session) noexcept;

} // namespace superior::ui
