// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <fcitx-utils/eventloopinterface.h>

#include <fcitx/action.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/instance.h>
#include <fcitx/userinterfacemanager.h>

#include <fcitx-utils/handlertable.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "superior/api/Commands.hpp"
#include "superior/config/Learning.hpp"
#include "superior/cloudpinyin/Cloud.hpp"
#include "superior/cloudpinyin/CloudState.hpp"
#include "superior/cloudpinyin/Fetcher.hpp"
#include "superior/core/ImeSession.hpp"
#include "superior/core/Modes.hpp"
#include "superior/core/SceneInput.hpp"
#include "superior/core/SpeechState.hpp"
#include "superior/core/TextOutbox.hpp"

#include "OverlayController.h"
#include "SpeechController.h"
#include "SpeechProcess.h"
#include "superior/fcitx5/ShiftTap.hpp"
#include "superior/fcitx5/TextSession.hpp"
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
    /// the Rime engine keeps working unchanged; English mode commits the typed
    /// character directly, which is what "Shift 切换英文" means for a Sogou-style
    /// IME: no input method switch, no separate keyboard layout.
    bool handleKeyEvent(fcitx::KeyEvent &event);

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

    /// Resolves the session a command claims to act on.
    ///
    /// Returns null unless the session belongs to the currently active input
    /// context, so a click delivered after focus moved elsewhere cannot type
    /// into the wrong application.
    fcitx::InputContext *contextFor(const api::SessionKey &session);

    /// Per-context composition ownership.
    ///
    /// Drawing the composition ourselves means the client must stop drawing it,
    /// which is expressed through the `Preedit` capability: engines then fill
    /// `InputPanel::preedit` instead of `clientPreedit`.
    struct PreeditOwnership {
        bool owned = false;
        bool clientShowedPreedit = false;
    };
    void takePreeditOwnership(fcitx::InputContext *inputContext);
    void releasePreeditOwnership(fcitx::InputContext *inputContext);

    /// Activates a Fcitx5 status-area action by name (`punctuation`, …).
    ///
    /// The UI never mutates engine state: it names an action, the engine decides
    /// what that means, exactly like a click in classicui's tray.
    void activateStatusItem(const std::string &itemId);

    /// Publishes the overlay's two switches as Fcitx5 status actions.
    ///
    /// Fcitx5's tray menu is built from the status area and has no extension
    /// point (`notificationitem/dbusmenu.cpp` lists every registered action of
    /// the current context, then Configure/Restart/Exit). Contributing an entry
    /// therefore means registering an action and putting it in our contexts.
    void registerTrayActions();

    /// Adds the tray actions to one of our contexts. Idempotent: focus passes
    /// over the same window many times.
    void attachTrayActions(fcitx::InputContext *inputContext);

    /// Copies the overlay state into the actions. Fcitx5 only re-reads a check
    /// mark when the status area is rebuilt, so a change is flushed.
    void refreshTrayActions();

    /// Flips `showStatusBar` in `settings.json`, which stays the single owner:
    /// the overlay polls that file, so it follows without a second channel.
    void toggleStatusBar();

    /// Cloud pinyin, sitting next to the engine's own candidates.
    ///
    /// Everything here is additive: we never remove or reorder what the engine
    /// produced, we only splice in one extra candidate. If the provider has
    /// nothing, the list is left exactly as the engine built it.
    struct CloudContext {
        /// The candidate list we injected into. The engine rebuilds the list on
        /// every keystroke, so a different pointer means "this is a new
        /// composition" and any stored index is stale.
        const fcitx::CandidateList *list = nullptr;
        std::string pinyin;
        /// Slot holding the learned word (index 0) and its text.
        int learnedIndex = -1;
        std::string learnedText;
        /// Slot the cloud candidate ended up in, once it arrived.
        int insertIndex = -1;
        /// The request for this pinyin was already handed to the debounce; the
        /// answer inserts itself when it comes back.
        bool queried = false;
        /// Text currently sitting at `insertIndex` because we put it there. Used
        /// to tell "our row is still there" from "the engine rebuilt the list and
        /// that position now holds a real candidate".
        std::string text;
    };
    void syncCloudCandidate(fcitx::InputContext *inputContext);
    /// Runs on a short timer: drains finished transfers, then fires the debounced
    /// request once the composition has settled.
    void pumpCloud();
    void applyCloudResult(fcitx::InputContext *inputContext, const std::string &pinyin,
                          const std::string &text);
    void dropCloudCandidate(fcitx::InputContext *inputContext);
    /// Forgets a queued (not yet sent) query for this context.
    void clearPendingCloud(fcitx::InputContext *inputContext);
    void updateCloudTimer();

    /// Commits a cloud candidate: drop the half-typed pinyin, then insert the
    /// text. Kept in one place so the policy cannot drift between call sites.
    void commitCloudText(fcitx::InputContext *inputContext, std::string_view text);

    /// Commits the candidate the user clicked. The id is re-validated against
    /// the live list, never trusted as an index.
    void activateCandidate(const api::ActivateCandidate &command);
    /// Pages the candidate list by one step (`+1` next, `-1` previous).
    void pageCandidates(const api::PageCandidates &command);
    /// Splits what Fcitx5 reports into engine facts (what the language engine
    /// produced) and host facts (where the cursor is, which client capabilities
    /// exist). The two are never mixed again: that split is what lets a second
    /// platform produce a scene without an `InputPanel`.
    void collect(fcitx::InputContext *inputContext, core::EngineFacts &engine,
                 core::HostFacts &host);
    core::ImeSession &sessionFor(fcitx::InputContext *inputContext);
    core::ImeSession *findSession(fcitx::InputContext *inputContext);
    void broadcast(fcitx::InputContext *inputContext);

    /// Hides our windows when another input method takes over. Fcitx5 then
    /// draws its own panel again, so Rime & friends keep their own UI.
    void broadcastHidden();

    [[nodiscard]] bool isOurs(fcitx::InputContext *inputContext) const;

    fcitx::AddonManager *manager_ = nullptr;
    fcitx::Instance *instance_ = nullptr;
    /// The overlay process and the socket it connects to (P5.4).
    ///
    /// Declared first so it is destroyed last: the socket must outlive the
    /// handlers that publish through it.
    OverlayController overlay_;
    std::map<fcitx::InputContext *, std::unique_ptr<core::ImeSession>> sessions_;
    std::map<fcitx::InputContext *, ShiftTapDetector> shiftTaps_;
    /// Right-Ctrl tap per context: the gesture is local to the focused client.
    std::map<fcitx::InputContext *, ModifierTap> ctrlTaps_;
    std::map<fcitx::InputContext *, PreeditOwnership> preeditOwnership_;
    std::map<fcitx::InputContext *, CloudContext> cloudContexts_;
    /// What this user picked from a cloud candidate, in `$XDG_CONFIG_HOME`.
    ///
    /// The engine already learns its own candidates (librime writes its userdb),
    /// so the only gap is a word that did not exist locally: it is committed by
    /// us, and the engine never saw it. Recording it here is what makes the
    /// second `yuanshen` offer 原神 without asking the network again.
    std::vector<config::LearnedWord> learned_;

    cloudpinyin::Fetcher cloudFetcher_;
    cloudpinyin::AnswerCache cloudCache_{256};
    /// Park the provider after three fruitless attempts. Upstream waits ten,
    /// which is long enough for a user to notice "cloud is on but nothing
    /// arrives" — and then wait five more minutes for the cooldown.
    cloudpinyin::ErrorBudget cloudBudget_{3, 60000};
    cloudpinyin::Backend cloudBackend_ = cloudpinyin::Backend::GoogleCn;
    bool cloudEnabled_ = false;
    int cloudMinLength_ = 2;
    int cloudIndex_ = 2;
    std::unique_ptr<fcitx::EventSourceTime> cloudTimer_;
    bool cloudPumping_ = false;
    /// Debounce: only the settled composition is sent, because typing `nihao`
    /// would otherwise fire four requests (`ni`, `nih`, `niha`, `nihao`).
    fcitx::InputContext *cloudPendingContext_ = nullptr;
    std::string cloudPendingPinyin_;
    std::chrono::steady_clock::time_point cloudPendingSince_{};
    bool settingsLoaded_ = false;
    /// The tray menu's two entries. They live here because the agent owns both
    /// pieces of state they show (the speech panel and `settings.json`); the
    /// overlay only draws.
    fcitx::SimpleAction toolbarAction_;
    fcitx::SimpleAction speechAction_;
    bool trayActionsRegistered_ = false;
    /// Mirrors `settings.json: showStatusBar`, which remains the single owner.
    bool showStatusBar_ = true;
    /// Last values published to Fcitx5, so an unchanged state costs no flush.
    bool trayToolbarChecked_ = true;
    bool traySpeechChecked_ = false;
    std::unique_ptr<fcitx::EventSourceTime> settingsTimer_;
    std::unique_ptr<fcitx::EventSourceTime> startupTimer_;
    /// The speech panel's decision layer (P5.4).
    ///
    /// The capability itself (recording and recognition) lives in
    /// `superior_speech_recognition`; this object only owns "what should the
    /// panel show and may this transcript be committed", and it reaches the
    /// adapter through three injected effects instead of calling back into it.
    SpeechController speech_;
    /// The single exit for text (§6.6, P5.3).
    ///
    /// `textSession_` is the Fcitx5 implementation of `api::TextSessionPort`; it
    /// owns "drop the composition, then commit" and "never write into a
    /// sensitive field". `outbox_` adds the hold/flush policy: a transcript
    /// waits for a caret, a clicked candidate does not.
    ///
    /// Declaration order matters: the outbox holds a reference to the session.
    TextSession textSession_;
    core::TextOutbox outbox_;
    bool inlineEnglishEnabled_ = true;
    /// The single owner of Chinese/English. Nothing else may keep its own copy:
    /// two booleans flipping on the same Shift release is exactly the bug this
    /// member replaced.
    core::Modes modes_;
    std::string skinId_ = "org.superior.default";
    fcitx::InputContext *activeContext_ = nullptr;
    bool refreshing_ = false;
    std::filesystem::file_time_type settingsStamp_{};
    std::vector<std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>>> handlers_;
    api::Epoch epoch_ = 1;
    api::ContextId nextContext_ = 1;
    api::FrameId nextFrame_ = 1;
};

} // namespace superior::fcitx5
