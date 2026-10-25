// SPDX-License-Identifier: GPL-3.0-or-later
#include "OverlayLayout.hpp"

#include <algorithm>
#include <cmath>

namespace superior::ui {

namespace {

constexpr int kSpeechPanelWidth = 320;
constexpr int kSpeechPanelHeight = 150;
constexpr int kSpeechSetupHeight = 200;
constexpr int kSpeechBottomGap = 24;

bool empty(const Rect &rect) noexcept {
    return rect.width <= 0 || rect.height <= 0;
}

bool intersects(const Rect &a, const Rect &b) noexcept {
    if (empty(a) || empty(b)) {
        return false;
    }
    return a.left < b.left + b.width && b.left < a.left + a.width && a.top < b.top + b.height &&
           b.top < a.top + a.height;
}

int scaled(int value, double scale) {
    return static_cast<int>(std::lround(static_cast<double>(value) * scale));
}

} // namespace

Size speechPanelSize(bool setup, double scale) noexcept {
    return Size{scaled(kSpeechPanelWidth, scale),
                scaled(setup ? kSpeechSetupHeight : kSpeechPanelHeight, scale)};
}

Rect placeSpeechPanel(Rect screen, Rect statusBar, Size desired, int margin) noexcept {
    if (screen.width <= 0 || screen.height <= 0 || desired.width <= 0 || desired.height <= 0) {
        return Rect{};
    }

    const int available = std::max(0, screen.width - 2 * margin);
    Rect panel;
    panel.width = std::min(desired.width, available);
    panel.height = std::min(desired.height, std::max(0, screen.height - 2 * margin));
    panel.left = screen.left + (screen.width - panel.width) / 2;
    panel.top = screen.top + screen.height - panel.height - margin - kSpeechBottomGap;

    // A dragged status bar can end up anywhere, including on top of the panel.
    // Lifting the panel is deterministic; asking the window manager for stacking
    // favours is not (there is no window manager for override-redirect windows).
    if (intersects(panel, statusBar)) {
        panel.top = statusBar.top - panel.height - margin;
        if (panel.top < screen.top + margin) {
            // Not enough room above: move beside it instead, which keeps both
            // windows fully visible on very short screens.
            panel.top = statusBar.top + statusBar.height + margin;
        }
    }

    panel.top = std::clamp(panel.top, screen.top + margin,
                           std::max(screen.top + margin,
                                    screen.top + screen.height - panel.height - margin));
    panel.left = std::clamp(panel.left, screen.left + margin,
                            std::max(screen.left + margin,
                                     screen.left + screen.width - panel.width - margin));
    return panel;
}

Rect liftAbove(Rect candidate, Rect speechPanel, Rect screen, int margin) noexcept {
    if (empty(candidate) || empty(speechPanel) || !intersects(candidate, speechPanel)) {
        return candidate;
    }
    Rect lifted = candidate;
    lifted.top = speechPanel.top - candidate.height - margin;
    if (lifted.top < screen.top + margin) {
        // No room above the panel: place it below, then clamp. The candidate
        // panel is more useful slightly displaced than hidden behind the speech
        // window.
        lifted.top = speechPanel.top + speechPanel.height + margin;
    }
    lifted.top = std::clamp(lifted.top, screen.top + margin,
                            std::max(screen.top + margin,
                                     screen.top + screen.height - lifted.height - margin));
    return lifted;
}

} // namespace superior::ui
