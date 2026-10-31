// SPDX-License-Identifier: GPL-3.0-or-later
#include "X11Connection.hpp"

#if defined(SUPERIOR_HAVE_XCB)

namespace superior::ui::x11 {

xcb_connection_t *connection() noexcept {
    static xcb_connection_t *handle = [] {
        xcb_connection_t *created = xcb_connect(nullptr, nullptr);
        if (created != nullptr && xcb_connection_has_error(created) != 0) {
            xcb_disconnect(created);
            return static_cast<xcb_connection_t *>(nullptr);
        }
        return created;
    }();
    return handle;
}

} // namespace superior::ui::x11

#endif
