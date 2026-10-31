// SPDX-License-Identifier: GPL-3.0-or-later
#include "CompositorPanel.hpp"

#include <cstdlib>

#if defined(SUPERIOR_HAVE_XCB_PANEL)
#include <xcb/xcb.h>
#endif

namespace superior::ui {

bool compositorPlacesCandidatePanel(std::string_view sessionDesktop,
                                   std::string_view currentDesktop) noexcept {
    return sessionDesktop == "Treeland" || sessionDesktop == "treeland" ||
           currentDesktop.find("Treeland") != std::string_view::npos ||
           currentDesktop.find("treeland") != std::string_view::npos;
}

#if defined(SUPERIOR_HAVE_XCB_PANEL)

namespace {

/// One connection for the overlay's lifetime: this runs on every frame whose
/// panel has no caret, and opening a socket per frame would be absurd.
xcb_connection_t *connection() {
    static xcb_connection_t *handle = [] {
        xcb_connection_t *created = xcb_connect(nullptr, nullptr);
        if (created != nullptr && xcb_connection_has_error(created)) {
            xcb_disconnect(created);
            return static_cast<xcb_connection_t *>(nullptr);
        }
        return created;
    }();
    return handle;
}

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

bool setCandidatePanelHint(std::uintptr_t window, bool on) noexcept {
    xcb_connection_t *conn = connection();
    if (conn == nullptr || window == 0) {
        return false;
    }
    static const xcb_atom_t atom = intern(conn, kCandidatePanelAtom);
    if (atom == XCB_ATOM_NONE) {
        return false;
    }
    const std::uint32_t value = on ? 1U : 0U;
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, static_cast<xcb_window_t>(window), atom,
                        XCB_ATOM_CARDINAL, 32, 1, &value);
    // Change-property has no reply, so flush with a cheap round-trip: the
    // compositor reads the atom right after the request and a lost race shows
    // the panel in the wrong place until the next frame.
    std::free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), nullptr));
    return xcb_connection_has_error(conn) == 0;
}

#else

bool setCandidatePanelHint(std::uintptr_t window, bool on) noexcept {
    static_cast<void>(window);
    static_cast<void>(on);
    return false;
}

#endif

} // namespace superior::ui
