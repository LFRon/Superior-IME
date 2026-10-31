// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <fcitx/action.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/statusarea.h>
#include <fcitx/text.h>
#include <fcitx/userinterfacemanager.h>

#include <fcitx-utils/handlertable.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "superior/core/Modes.hpp"
#include "superior/pinyin/LibrimePort.hpp"
#include "superior/pinyin/RimeEngine.hpp"

namespace pinyin = superior::pinyin;

namespace superior::fcitx5 {

/// The pinyin engine itself, backed by librime.
///
/// This is a real `InputMethodEngine`, not an observer: it owns the candidate
/// list of its input methods. That is what lets the whole 先！进！ feature set
/// (cloud pinyin, learned words, Sogou-style Shift switching, self-drawn UI)
/// keep working while the language model changes underneath.
///
/// Data isolation: schemas and `userdb` live under
/// `$XDG_DATA_HOME/superior-ime/rime`, so installing this never touches a user's
/// existing Rime configuration. `fcitx5-rime` is not used at all.
class RimeInputMethod : public fcitx::InputMethodEngineV2 {
public:
    explicit RimeInputMethod(fcitx::AddonManager *manager);
    ~RimeInputMethod() override;

    RimeInputMethod(const RimeInputMethod &) = delete;
    RimeInputMethod &operator=(const RimeInputMethod &) = delete;

    void keyEvent(const fcitx::InputMethodEntry &entry, fcitx::KeyEvent &event) override;
    void activate(const fcitx::InputMethodEntry &entry,
                  fcitx::InputContextEvent &event) override;
    void deactivate(const fcitx::InputMethodEntry &entry,
                    fcitx::InputContextEvent &event) override;
    void reset(const fcitx::InputMethodEntry &entry,
               fcitx::InputContextEvent &event) override;

    [[nodiscard]] std::string subMode(const fcitx::InputMethodEntry &entry,
                                      fcitx::InputContext &inputContext) override;
    [[nodiscard]] std::string subModeLabelImpl(const fcitx::InputMethodEntry &entry,
                                               fcitx::InputContext &inputContext) override;

    /// Exposed for diagnostics: true when librime could be loaded.
    [[nodiscard]] bool engineAvailable() const;

private:
    /// Per-context engine state.
    ///
    /// One librime session per input context: switching windows must not carry a
    /// half-typed composition across, and `userdb` learning is per session.
    ///
    /// The port lives here, not in `RimeInputMethod`: a librime session handle
    /// belongs to exactly one port, so sharing a port across contexts would make
    /// every window type into the same composition.
    struct Session {
        pinyin::LibrimePort port;
        pinyin::RimeEngine engine{port};
        pinyin::Snapshot lastSnapshot;
        /// Candidate texts the current panel list was built from. While they
        /// match, the list is left alone, so rows inserted by the agent (cloud
        /// pinyin, learned words) survive cursor movement.
        std::vector<std::string> builtTexts;
        bool listBuilt = false;
    };

    [[nodiscard]] Session *sessionFor(fcitx::InputContext *inputContext);
    void forget(fcitx::InputContext *inputContext);
    void updatePanel(fcitx::InputContext *inputContext, Session &session);
    void commit(fcitx::InputContext *inputContext, const std::string &text);

    /// Handles keys the engine must not see because the visible list may
    /// contain rows that did not come from librime.
    ///
    /// Cloud pinyin inserts a word at position 2, so "select candidate 2" means
    /// the *visible* row, not librime's row. Routing selection through the
    /// list's own `CandidateWord::select()` keeps both cases correct.
    bool handleSelectionKey(fcitx::InputContext *inputContext, fcitx::KeyEvent &event);

    /// Attaches the punctuation action to a context and syncs its state.
    void attachPunctuation(fcitx::InputContext *inputContext, Session &session);
    /// Pushes the decision layer's punctuation mode into one engine session.
    ///
    /// Only the switches the punctuation action governs: it is what the action
    /// shows, so whoever owns the action must be the only writer.
    void applyModes(Session &session);
    /// Pushes the schema switches that come from `settings.json` instead of from
    /// a toolbar control.
    ///
    /// Applied unconditionally, unlike `applyModes`: `emoji` is ours even when
    /// another addon registered the `punctuation` action first, and skipping it
    /// there would make the setting silently do nothing.
    void applyPreferences(Session &session);
    /// Re-reads `settings.json` when it changed since the last read.
    ///
    /// The engine addon is loaded by Fcitx5 and has no settings watcher, so the
    /// file's mtime is the notification. Called on activation and (throttled) on
    /// the key path, because a composition can stay open across a settings
    /// change: flipping a switch takes effect on the next key, not the next
    /// login.
    void refreshSettings();
    /// Mirrors `modes_` into the toolbar action.
    void refreshPunctuationAction();
    /// Toggles Chinese (full-width) vs ASCII (half-width) punctuation.
    void togglePunctuation(fcitx::InputContext *inputContext);

    fcitx::AddonManager *manager_ = nullptr;
    fcitx::Instance *instance_ = nullptr;
    std::map<fcitx::InputContext *, std::unique_ptr<Session>> sessions_;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> destroyedHandler_;
    fcitx::SimpleAction punctuationAction_;
    bool punctuationRegistered_ = false;
    /// Punctuation width, and its only owner.
    ///
    /// The toolbar action and every session's `ascii_punct` option are *views*
    /// of this value. Before, the state was inferred back out of the engine and
    /// a `punctuationSeeded_` flag decided which session won; that is two
    /// writers for one switch.
    ///
    /// Scope note: this addon owns punctuation (it owns the status action); the
    /// agent owns the Chinese/English switch. Each `Modes` instance therefore
    /// owns exactly the fields its process can write.
    core::Modes modes_;
    pinyin::Paths paths_;
    bool libraryReady_ = false;
    std::string startError_;
    /// Schema switches owned by `settings.json` rather than by the toolbar.
    ///
    /// `emoji` is one: the wanxiang schema ships an emoji filter and reports its
    /// state through its own panel, but the product-level switch lives in our
    /// settings, so this is the value every session is told.
    bool emojiCandidates_ = true;
    bool settingsLoaded_ = false;
    /// The first read is logged once: a default-on value leaves no other trace,
    /// and "is the emoji switch even reaching librime" is the first question.
    bool emojiLogged_ = false;
    std::filesystem::file_time_type settingsStamp_{};
    /// When the file was last stat'ed, so the per-keystroke check costs nothing
    /// on a normal typing burst.
    std::chrono::steady_clock::time_point settingsChecked_{};
};

} // namespace superior::fcitx5
