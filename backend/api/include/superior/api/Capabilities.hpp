// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace superior::api {

/// The UI shell family. Content and semantic state are shared; layout,
/// interaction and window ownership are not.
enum class UiProfile {
    Desktop,
    Mobile,
};

/// What the current shell can actually do. The agent adapts window strategy and
/// optional affordances to these facts instead of guessing from the platform.
struct UiCapabilities {
    UiProfile profile = UiProfile::Desktop;

    bool hover = true;
    bool rightClickMenu = true;
    bool multiScreen = true;
    bool layerShell = false;
    bool inputPopupSurface = false;
    bool touch = false;
    bool safeArea = false;
    bool prefersReducedMotion = false;

    /// Logical-to-device scale of the target output. One per shell in M0.
    double scale = 1.0;
};

/// Static description of one shell implementation, exchanged during the
/// protocol handshake.
struct UiShellInfo {
    std::string name;
    std::string version;
    UiProfile profile = UiProfile::Desktop;
    std::vector<std::string> windowBackends;
};

} // namespace superior::api
