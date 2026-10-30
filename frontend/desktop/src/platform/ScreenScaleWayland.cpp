// SPDX-License-Identifier: GPL-3.0-or-later
#include "ScalePlatform.hpp"

#if defined(SUPERIOR_HAVE_WAYLAND_SCALE)

#include <wayland-client.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <poll.h>
#include <vector>

namespace superior::ui::detail {

namespace {

/// One `wl_output` and the last scale it announced.
struct OutputEntry {
    std::uint32_t name = 0;
    wl_output *output = nullptr;
    int scale = 0;
};

void onGeometry(void *, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t, const char *,
                const char *, int32_t) {}
void onMode(void *, wl_output *, uint32_t, int32_t, int32_t, int32_t) {}
void onDone(void *, wl_output *) {}
void onScale(void *data, wl_output *, int32_t factor) {
    static_cast<OutputEntry *>(data)->scale = factor;
}

const wl_output_listener kOutputListener = {
    .geometry = &onGeometry,
    .mode = &onMode,
    .done = &onDone,
    .scale = &onScale,
    // Version 4 fields. A compositor that speaks an older protocol never sends
    // them; a newer one must still find a handler rather than a null pointer.
    .name = [](void *, wl_output *, const char *) {},
    .description = [](void *, wl_output *, const char *) {},
};

/// The compositor connection, kept open: the scale is polled, and a Wayland
/// connection per poll would be a socket every second for no reason.
struct Registry {
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    /// `unique_ptr` so the listener's user data stays valid when the vector grows.
    std::vector<std::unique_ptr<OutputEntry>> outputs;

    ~Registry() { teardown(); }

    void teardown() {
        for (auto &entry : outputs) {
            if (entry->output != nullptr) {
                wl_output_destroy(entry->output);
            }
        }
        outputs.clear();
        if (registry != nullptr) {
            wl_registry_destroy(registry);
            registry = nullptr;
        }
        if (display != nullptr) {
            wl_display_disconnect(display);
            display = nullptr;
        }
    }

    [[nodiscard]] bool open() {
        display = wl_display_connect(nullptr);
        if (display == nullptr) {
            return false;
        }
        registry = wl_display_get_registry(display);
        if (registry == nullptr) {
            teardown();
            return false;
        }
        return true;
    }

    [[nodiscard]] int largestScale() const {
        int largest = 0;
        for (const auto &entry : outputs) {
            largest = std::max(largest, entry->scale);
        }
        return largest;
    }
};

void onGlobal(void *data, wl_registry *registry, std::uint32_t name, const char *interface,
              std::uint32_t version) {
    auto *state = static_cast<Registry *>(data);
    if (std::strcmp(interface, wl_output_interface.name) != 0) {
        return;
    }
    auto entry = std::make_unique<OutputEntry>();
    entry->name = name;
    const std::uint32_t bindVersion = std::min<std::uint32_t>(version, 4);
    entry->output = static_cast<wl_output *>(
        wl_registry_bind(registry, name, &wl_output_interface, bindVersion));
    if (entry->output == nullptr) {
        return;
    }
    wl_output_add_listener(entry->output, &kOutputListener, entry.get());
    state->outputs.push_back(std::move(entry));
}

void onGlobalRemove(void *data, wl_registry *, std::uint32_t name) {
    auto *state = static_cast<Registry *>(data);
    const auto it = std::find_if(state->outputs.begin(), state->outputs.end(),
                                 [name](const auto &entry) { return entry->name == name; });
    if (it == state->outputs.end()) {
        return;
    }
    if ((*it)->output != nullptr) {
        wl_output_destroy((*it)->output);
    }
    state->outputs.erase(it);
}

const wl_registry_listener kRegistryListener = {
    .global = &onGlobal,
    .global_remove = &onGlobalRemove,
};

/// A roundtrip that cannot hang the UI thread.
///
/// `wl_display_roundtrip` waits forever when the compositor stops answering,
/// and this runs on the overlay's main thread. A bounded poll keeps a stuck
/// compositor from freezing the toolbar as well.
bool roundtripBounded(wl_display *display, std::chrono::milliseconds budget) {
    wl_callback *callback = wl_display_sync(display);
    if (callback == nullptr) {
        return false;
    }
    bool done = false;
    const wl_callback_listener listener = {
        .done = [](void *data, wl_callback *, std::uint32_t) { *static_cast<bool *>(data) = true; },
    };
    wl_callback_add_listener(callback, &listener, &done);

    const int fd = wl_display_get_fd(display);
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!done) {
        if (wl_display_dispatch_pending(display) < 0) {
            break;
        }
        if (done) {
            break;
        }
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            break;
        }
        pollfd socket{fd, POLLIN, 0};
        const int ready = ::poll(&socket, 1, static_cast<int>(remaining.count()));
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            break;
        }
        if (wl_display_dispatch(display) < 0) {
            break;
        }
    }
    wl_callback_destroy(callback);
    return done;
}

} // namespace

int waylandOutputScale() noexcept {
    // Read once per process: the overlay is restarted with the session, and a
    // desktop scale change on X11/XWayland means logging out anyway.
    static const int scale = [] {
        Registry registry;
        if (!registry.open()) {
            return 0;
        }
        wl_registry_add_listener(registry.registry, &kRegistryListener, &registry);
        // The first roundtrip delivers the global list (which is where the
        // outputs are bound), the second one delivers their initial events.
        static_cast<void>(roundtripBounded(registry.display, std::chrono::milliseconds(400)));
        if (!roundtripBounded(registry.display, std::chrono::milliseconds(400))) {
            return 0;
        }
        if (registry.outputs.empty()) {
            return 0;
        }
        // `wl_output.scale` defaults to 1 and a compositor only sends it on
        // bind, so a missing value means 1x, not "unknown".
        return std::max(registry.largestScale(), 1);
    }();
    return scale;
}

} // namespace superior::ui::detail

#else

namespace superior::ui::detail {

int waylandOutputScale() noexcept { return 0; }

} // namespace superior::ui::detail

#endif
