// SPDX-License-Identifier: GPL-3.0-or-later
#include "ScalePlatform.hpp"
#include "ScreenScale.hpp"

#if defined(SUPERIOR_HAVE_XCB_SCALE)

#include <xcb/xcb.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace superior::ui::detail {

namespace {

/// One connection for the life of the overlay.
///
/// The scale is polled, and reconnecting on every poll would open a socket per
/// second for no reason. Only the main thread calls this, which is why a plain
/// function-local static is enough.
class Connection {
public:
    Connection() : handle_(xcb_connect(nullptr, nullptr)) {
        if (handle_ != nullptr && xcb_connection_has_error(handle_)) {
            xcb_disconnect(handle_);
            handle_ = nullptr;
        }
    }
    ~Connection() {
        if (handle_ != nullptr) {
            xcb_disconnect(handle_);
        }
    }
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    [[nodiscard]] xcb_connection_t *get() const noexcept { return handle_; }

private:
    xcb_connection_t *handle_ = nullptr;
};

/// Reads one string property of the root window.
///
/// `RESOURCE_MANAGER` is what `xrdb` writes and what Xft-based toolkits read;
/// `SCREEN_RESOURCES` is the per-screen override a few setups use instead.
std::string readRootString(xcb_connection_t *connection, xcb_window_t root, const char *name) {
    const std::uint16_t length = static_cast<std::uint16_t>(std::strlen(name));
    xcb_intern_atom_reply_t *atom = xcb_intern_atom_reply(
        connection, xcb_intern_atom(connection, 0, length, name), nullptr);
    if (atom == nullptr) {
        return {};
    }
    const xcb_get_property_cookie_t cookie =
        xcb_get_property(connection, 0, root, atom->atom, XCB_ATOM_STRING, 0, 1U << 18);
    std::free(atom);

    xcb_generic_error_t *error = nullptr;
    xcb_get_property_reply_t *reply = xcb_get_property_reply(connection, cookie, &error);
    if (reply == nullptr) {
        std::free(error);
        return {};
    }
    const int bytes = xcb_get_property_value_length(reply);
    std::string text;
    if (bytes > 0) {
        text.assign(static_cast<const char *>(xcb_get_property_value(reply)),
                    static_cast<std::size_t>(bytes));
    }
    std::free(reply);
    return text;
}

} // namespace

int x11XftDpi() noexcept {
    static Connection connection;
    xcb_connection_t *handle = connection.get();
    if (handle == nullptr) {
        return 0;
    }
    const xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(handle));
    if (screens.rem == 0 || screens.data == nullptr) {
        return 0;
    }
    const xcb_window_t root = screens.data->root;

    if (const int dpi = parseXftDpi(readRootString(handle, root, "RESOURCE_MANAGER")); dpi > 0) {
        return dpi;
    }
    return parseXftDpi(readRootString(handle, root, "SCREEN_RESOURCES"));
}

} // namespace superior::ui::detail

#else

namespace superior::ui::detail {

int x11XftDpi() noexcept { return 0; }

} // namespace superior::ui::detail

#endif
