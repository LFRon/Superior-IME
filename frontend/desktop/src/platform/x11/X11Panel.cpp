// SPDX-License-Identifier: GPL-3.0-or-later
#include "X11Platform.hpp"

#include <cstdlib>
#include <string_view>

#include "X11Connection.hpp"

namespace superior::ui::x11 {

namespace {

/// The X11 property Treeland reads to adopt a window as its input-method
/// candidate panel (`IMCandidatePanelManager`). Treeland then moves it into its
/// popup container, drops its decoration and dock entry, denies it keyboard
/// focus, and places it from the application's own `set_cursor_rectangle` — the
/// one coordinate an external process cannot obtain on Wayland.
///
/// It is a convention, not a protocol: writing an unknown atom on a compositor
/// that does not implement it changes nothing, which is why this is safe.
constexpr std::string_view kCandidatePanelAtom = "_DEEPIN_IM_CANDIDATE_PANEL";

} // namespace

#if defined(SUPERIOR_HAVE_XCB_PANEL)

namespace {

xcb_atom_t intern(xcb_connection_t *conn, std::string_view name) {
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
        conn, xcb_intern_atom(conn, 0, static_cast<std::uint16_t>(name.size()), name.data()),
        nullptr);
    if (reply == nullptr) {
        return XCB_ATOM_NONE;
    }
    const xcb_atom_t atom = reply->atom;
    std::free(reply);
    return atom;
}

} // namespace

bool adoptAsCandidatePanel(std::uintptr_t window) noexcept {
    xcb_connection_t *conn = connection();
    if (conn == nullptr || window == 0) {
        return false;
    }
    static const xcb_atom_t atom = intern(conn, kCandidatePanelAtom);
    if (atom == XCB_ATOM_NONE) {
        return false;
    }
    const std::uint32_t value = 1;
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, static_cast<xcb_window_t>(window), atom,
                        XCB_ATOM_CARDINAL, 32, 1, &value);
    // Change-property has no reply, so flush with a cheap round-trip: the
    // compositor reads the atom right after the request, and a lost race shows
    // the panel in the wrong place until the next frame.
    std::free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), nullptr));
    return xcb_connection_has_error(conn) == 0;
}

#else

bool adoptAsCandidatePanel(std::uintptr_t window) noexcept {
    static_cast<void>(window);
    return false;
}

#endif

} // namespace superior::ui::x11
