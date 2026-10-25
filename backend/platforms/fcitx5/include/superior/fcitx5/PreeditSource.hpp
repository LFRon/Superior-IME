// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>

namespace superior::fcitx5 {

/// What to draw in the composition row, and where the caret goes.
struct PreeditView {
    std::string text;
    /// Byte offset into `text`.
    std::size_t cursor = 0;
    /// True when the text came from the client preedit instead of the panel
    /// preedit. Only used for logging: both are rendered the same way.
    bool fromClient = false;
};

/// Picks the composition text to draw.
///
/// Fcitx5 engines fill `InputPanel::preedit` (drawn by the UI) or
/// `InputPanel::clientPreedit` (drawn by the application, inline) depending on
/// the `Preedit` capability and the engine's own configuration. A Sogou-style
/// UI must show the composition in its own window, so:
/// - the panel preedit wins when present;
/// - otherwise the client preedit is reused, so an engine that only sets it
///   still shows up in our window instead of silently losing the composition;
/// - when both are empty the view is empty and the row is not drawn.
[[nodiscard]] PreeditView choosePreedit(const std::string &panelPreedit,
                                        std::size_t panelCursor,
                                        const std::string &clientPreedit,
                                        std::size_t clientCursor);

} // namespace superior::fcitx5
