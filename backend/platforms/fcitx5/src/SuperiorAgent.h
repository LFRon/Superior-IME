// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <fcitx-utils/eventloopinterface.h>

#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/instance.h>

#include <fcitx-utils/handlertable.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "superior/api/Commands.hpp"
#include "superior/core/ImeSession.hpp"
#include "superior/fcitx5/PanelFacts.hpp"
#include "superior/fcitx5/ShiftTap.hpp"
#include "superior/protocol/SceneServer.hpp"

namespace superior::fcitx5 {

/// Passive Fcitx5 observer.
///
/// Design constraints that protect normal input:
/// - registered as `Category=Module`, never as an input method engine, a
///   frontend or the single UI addon;
/// - only reads `InputPanel`, `StatusArea` and `InputContext` facts;
/// - never mutates preedit, candidate lists or client preedit, so Wayland
///   clients keep using Fcitx5's wayland frontend and X11 clients keep using
///   its XCB/XIM frontends;
/// - never consumes key events.
class SuperiorAgent : public fcitx::AddonInstance {
public:
    explicit SuperiorAgent(fcitx::AddonManager *manager);
    ~SuperiorAgent() override;

    SuperiorAgent(const SuperiorAgent &) = delete;
    SuperiorAgent &operator=(const SuperiorAgent &) = delete;

private:
    void refresh(fcitx::InputContext *inputContext);

    /// Installs or clears the custom input panel callback for this context.
    ///
    /// When the active input method is ours, Fcitx5 must not draw the panel
    /// itself: the callback replaces Classic UI's drawing for this input
    /// context only, while every other input method keeps using its own UI.
    void syncPanelOwnership(fcitx::InputContext *inputContext);

    /// Handles keys for our input method.
    ///
    /// Returns true when the event was consumed. Chinese mode returns false so
    /// the Fcitx5 pinyin engine keeps working unchanged; English mode commits
    /// the typed character directly, which is what "Shift 切换英文" means for a
    /// Sogou-style IME: no input method switch, no separate keyboard layout.
    bool handleKeyEvent(fcitx::KeyEvent &event);

    /// Spawns the overlay process if no UI is connected, unless disabled.
    /// Retries on later focus events so a crashed overlay comes back.
    void ensureOverlay();

    /// Reloads `~/.config/superior-ime/settings.json` when its mtime changed.
    void reloadSettings();

    /// Switches inline Chinese/English mode. The mode is global on purpose:
    /// Sogou keeps one mode for the input method, not one per application, so
    /// switching windows does not silently change what you type.
    ///
    /// `inputContext` may be null (settings hot reload without a focused
    /// window); the mode still changes, but no scene is repainted.
    void setEnglishMode(fcitx::InputContext *inputContext, bool english, const char *reason);

    /// Per-context Shift tap detector. The mode itself is global; only the
    /// physical key state belongs to a window.
    void forget(fcitx::InputContext *inputContext);
    void handleCommand(std::string bytes);
    /// Applies a validated semantic command from the UI.
    void applyCommand(const api::UiCommand &command);
    [[nodiscard]] PanelFacts collect(fcitx::InputContext *inputContext);
    core::ImeSession &sessionFor(fcitx::InputContext *inputContext);
    core::ImeSession *findSession(fcitx::InputContext *inputContext);
    void broadcast(fcitx::InputContext *inputContext);

    /// Hides our windows when another input method takes over. Fcitx5 then
    /// draws its own panel again, so Rime & friends keep their own UI.
    void broadcastHidden();

    [[nodiscard]] bool isOurs(fcitx::InputContext *inputContext) const;

    fcitx::AddonManager *manager_ = nullptr;
    fcitx::Instance *instance_ = nullptr;
    protocol::SceneServer server_;
    std::map<fcitx::InputContext *, std::unique_ptr<core::ImeSession>> sessions_;
    std::map<fcitx::InputContext *, ShiftTapDetector> shiftTaps_;
    bool overlaySpawned_ = false;
    bool settingsLoaded_ = false;
    std::unique_ptr<fcitx::EventSourceTime> settingsTimer_;
    bool inlineEnglishEnabled_ = true;
    bool englishMode_ = false;
    std::string skinId_ = "org.superior.default";
    fcitx::InputContext *activeContext_ = nullptr;
    bool refreshing_ = false;
    std::filesystem::file_time_type settingsStamp_{};
    std::chrono::steady_clock::time_point lastOverlayAttempt_{};
    std::vector<std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>>> handlers_;
    api::Epoch epoch_ = 1;
    api::ContextId nextContext_ = 1;
    api::FrameId nextFrame_ = 1;
};

} // namespace superior::fcitx5
