// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "superior/rime/RimePort.hpp"
#include "superior/rime/Types.hpp"

namespace superior::rime {

/// Input method state machine on top of a `RimePort`.
///
/// This is the part that used to live in `fcitx5-rime`; here it is platform
/// neutral, so the same logic can drive a Windows TSF or Android backend later.
/// It knows only about keys, commits, candidates and options — no Fcitx5 and no
/// Qt types.
class RimeEngine {
public:
    explicit RimeEngine(RimePort &port);

    RimeEngine(const RimeEngine &) = delete;
    RimeEngine &operator=(const RimeEngine &) = delete;

    /// Initialize the library and open a session.
    ///
    /// When `paths.userData` has no build artifacts, librime still creates a
    /// session but produces no candidates; `needsDeploy()` reports that so the
    /// caller can run `deploy()` and tell the user why typing looks inert.
    bool start(const Paths &paths, std::string &error);
    void stop();

    [[nodiscard]] bool running() const noexcept { return sessionOpen_; }
    [[nodiscard]] bool needsDeploy() const noexcept { return needsDeploy_; }

    /// Runs a blocking deployment. Reports progress through `onProgress` when the
    /// port can provide it.
    bool deploy(std::string &error);

    struct KeyResult {
        /// The engine consumed the key.
        bool consumed = false;
        /// Text to commit now, if any. Empty when nothing was produced.
        std::string commit;
    };

    /// Processes one key.
    ///
    /// Handles the release bit, drains the commit exactly once, and keeps the
    /// ASCII-mode flag in sync with the session's `ascii_mode` option.
    KeyResult processKey(const KeyInput &key);

    /// Current frame. Cheap enough to call per event; the menu is copied.
    [[nodiscard]] Snapshot snapshot();

    bool selectCandidate(std::size_t index);
    bool page(bool backward);

    /// Removes and returns the text produced by the last non-key action
    /// (`selectCandidate`), if any.
    ///
    /// Key processing drains its own commit, but a click on a candidate is a
    /// separate entry point and must not leave the commit queued: the next key
    /// would then commit it twice.
    bool takeCommit(std::string &out);

    /// Toggles the engine's own Chinese/English switch.
    ///
    /// This is the Sogou-style behaviour: the *input method* switches mode, the
    /// application keeps the same keyboard layout and the same input method.
    bool setAsciiMode(bool ascii);
    [[nodiscard]] bool asciiMode();

    /// Generic schema switch, for options such as `ascii_punct` / `full_shape`.
    ///
    /// The platform layer owns the toolbar, so it needs to read and write the
    /// schema switches without knowing librime's C API.
    bool setOption(std::string_view name, bool value);
    [[nodiscard]] bool getOption(std::string_view name);

    /// Candidate index from a 1-based id, or -1 when out of range.
    ///
    /// The UI sends ids back verbatim; re-validating here is what makes a stale
    /// click harmless. `count` is the number of candidates on the *current*
    /// page, not the total, because the id is a page-local position.
    [[nodiscard]] static int indexForId(std::uint32_t id, int count) noexcept;

private:
    void refreshAsciiMode();

    RimePort &port_;
    bool sessionOpen_ = false;
    bool needsDeploy_ = false;
    /// Cached so `snapshot()` does not need an extra query per frame.
    bool ascii_ = false;
};

} // namespace superior::rime
