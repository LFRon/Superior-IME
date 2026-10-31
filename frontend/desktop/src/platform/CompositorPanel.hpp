// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

namespace superior::ui {

/// The X11 property Treeland reads to adopt a window as its input-method
/// candidate panel.
///
/// Treeland (`IMCandidatePanelManager`) moves such a window into its popup
/// container, drops its decoration and dock entry, and places it at the
/// **text-input cursor rectangle** it receives from the focused application —
/// the one coordinate an external process cannot otherwise obtain on Wayland.
/// A Wayland-native candidate window would use the equivalent
/// `xdg_toplevel.set_tag("org.deepin.treeland.im-candidate-panel")`; our overlay
/// runs on XWayland, so the X11 property is the matching channel.
///
/// It is a privileged-ish but public convention: writing an unknown atom on a
/// compositor that does not implement it changes nothing, which is why this is
/// safe to attempt anywhere.
inline constexpr std::string_view kCandidatePanelAtom = "_DEEPIN_IM_CANDIDATE_PANEL";

/// True when the running desktop is Treeland, i.e. when that property is
/// meaningful.
///
/// Decided from the session name rather than by guessing from an atom lookup:
/// `xcb_intern_atom` always succeeds, so it cannot tell "supported" from "just a
/// name the server has now heard of".
[[nodiscard]] bool compositorPlacesCandidatePanel(std::string_view sessionDesktop,
                                                  std::string_view currentDesktop) noexcept;

/// Marks (or unmarks) `window` as the compositor's candidate panel.
///
/// Returns false when no X connection could be made or the property could not be
/// written; the caller then keeps its own placement, so a compositor without
/// this convention still gets a usable (if less accurate) window.
bool setCandidatePanelHint(std::uintptr_t window, bool on) noexcept;

} // namespace superior::ui
