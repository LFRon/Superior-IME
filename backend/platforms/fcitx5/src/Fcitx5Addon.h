// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <fcitx-utils/event.h>
#include <fcitx-utils/handlertable.h>

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

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "superior/api/Commands.hpp"
#include "superior/api/Composer.hpp"
#include "superior/api/Host.hpp"
#include "superior/api/Key.hpp"
#include "superior/api/Ports.hpp"
#include "superior/api/Scene.hpp"
#include "superior/core/TextOutbox.hpp"
#include "superior/pinyin/CloudService.hpp"
#include "superior/pinyin/LearningStore.hpp"
#include "superior/pinyin/RimeComposer.hpp"
#include "superior/runtime/Runtime.hpp"

#include "OverlayController.h"
#include "SpeechController.h"
#include "superior/fcitx5/ShiftTap.hpp"
#include "superior/fcitx5/TextSession.hpp"

namespace superior::fcitx5 {

/// The Fcitx5 host.
///
/// This class is the **only** place that knows Fcitx5, and it is deliberately
/// thin: it translates keys, reports facts, executes text edits, presents
/// scenes and mirrors the status area. It owns no product state: the runtime
/// owns sessions, modes, settings, candidates, cloud and learning.
///
/// It is also the composition root: the one file that may include the pinyin
/// and speech implementations and inject them into the runtime.
class Fcitx5Addon final : public fcitx::InputMethodEngineV2,
                          public api::Scheduler,
                          public api::ScenePresenter,
                          public api::NativePanelSink,
                          public api::StatusSink,
                          public api::LifecyclePort,
                          public runtime::SpeechPort {
public:
    explicit Fcitx5Addon(fcitx::AddonManager *manager);
    ~Fcitx5Addon() override;

    Fcitx5Addon(const Fcitx5Addon &) = delete;
    Fcitx5Addon &operator=(const Fcitx5Addon &) = delete;

    // ------------------------------------------------------- InputMethodEngine
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

    // ------------------------------------------------------------- Scheduler
    [[nodiscard]] bool onOwnerThread() const noexcept override;
    void post(std::function<void()> work) override;
    [[nodiscard]] std::uint64_t nowMs() const override;
    std::uint64_t after(std::uint64_t delayMs, std::function<void()> work) override;
    void cancel(std::uint64_t timerId) override;
    std::uint64_t nextFrame(std::function<void(std::uint64_t timeMs)> work) override;

    // --------------------------------------------------------- ScenePresenter
    void present(const api::UiSceneFrame &frame) override;
    void setHitMap(const api::SessionKey &session, const api::HitMap &hitMap) override;
    void setVisible(const api::SessionKey &session, bool visible) override;

    // --------------------------------------------------------- NativePanelSink
    void show(const api::SessionKey &session, const api::NativePanel &panel) override;
    void hide(const api::SessionKey &session) override;

    // -------------------------------------------------------------- StatusSink
    void setStatusItems(std::span<const api::StatusItem> items) override;
    bool activate(std::string_view id) override;

    // ----------------------------------------------------------- LifecyclePort
    void degraded(api::DegradedReason reason, const std::string &detail) override;
    void recovered(api::DegradedReason reason) override;
    void sessionFailed(const api::SessionKey &session, const std::string &detail) override;

    // ------------------------------------------------------------- SpeechPort
    [[nodiscard]] bool enabled() const override;
    [[nodiscard]] bool hotkeyEnabled() const override;
    [[nodiscard]] bool busy() const override;
    [[nodiscard]] bool recording() const override;
    [[nodiscard]] bool canStart() const override;
    void setEnabled(bool enabled) override;
    void setHotkeyEnabled(bool enabled) override;
    void setModelId(std::string id) override;
    void toggle() override;
    void toggleByHotkey() override;
    void start() override;
    void stop() override;
    void cancel(std::string_view reason) override;
    void reload() override;
    void tick() override;
    void holdCommits() override;
    void flushHeld() override;
    [[nodiscard]] api::SpeechScene scene(std::string_view skinId,
                                         api::SkinGeneration generation) const override;
    void shutdown() override;

private:
    void wireEvents();
    void startTickTimer();
    void handleCommand(std::string bytes);
    void drainCommands();

    [[nodiscard]] api::SessionKey keyFor(fcitx::InputContext *context);
    [[nodiscard]] api::SessionKey keyIfKnown(fcitx::InputContext *context) const;
    [[nodiscard]] fcitx::InputContext *contextFor(const api::SessionKey &key) const;
    [[nodiscard]] fcitx::InputContext *activeContext() const;
    [[nodiscard]] api::HostFacts hostFacts(fcitx::InputContext &context) const;

    void onFocusIn(fcitx::InputContext *context);
    void onFocusOut(fcitx::InputContext *context);
    void onDestroyed(fcitx::InputContext *context);
    void onCursorChanged(fcitx::InputContext *context);
    void onSwitchInputMethod(fcitx::InputContext *context);

    void syncPanelOwnership(fcitx::InputContext *context);
    void syncAllPanelOwnership();
    void takePreeditOwnership(fcitx::InputContext *context);
    void releasePreeditOwnership(fcitx::InputContext *context);

    void registerActions();
    void attachActions(fcitx::InputContext *context);
    void refreshActions();
    void toggleStatusBar();

    static void clearPanelKeepingCallback(fcitx::InputPanel &panel);
    static bool sensitive(const fcitx::InputContext &context);

    fcitx::AddonManager *manager_ = nullptr;
    fcitx::Instance *instance_ = nullptr;

    OverlayController overlay_;

    /// Paths are a host detail: the capability receives them, it does not parse
    /// the environment itself.
    pinyin::Paths paths_ = pinyin::Paths::fromEnvironment();
    pinyin::LearningStore learning_;
    pinyin::CloudService cloud_;
    pinyin::RimeComposerFactory pinyinFactory_;

    TextSession textSession_;
    /// Speech keeps its own outbox instance but writes through the same
    /// `TextSessionPort`; the only writer of client text is still `TextSession`.
    core::TextOutbox speechOutbox_;
    SpeechController speechController_;
    runtime::Runtime runtime_;

    std::map<api::SessionKey, fcitx::InputContext *> contexts_;
    std::map<fcitx::InputContext *, api::SessionKey> keys_;

    std::map<fcitx::InputContext *, ShiftTapDetector> shiftTaps_;
    std::map<fcitx::InputContext *, ModifierTap> ctrlTaps_;
    std::map<fcitx::InputContext *, bool> preeditOwned_;
    /// Candidate texts the current native list was built from. While they
    /// match, the list object is reused, so moving the highlight or updating
    /// the preedit does not rebuild (and flicker) the list.
    std::map<api::SessionKey, std::vector<std::string>> nativeTexts_;

    /// UI commands are decoded on the socket thread; the queue plus the pipe
    /// wakeup is how they reach the Fcitx5 owner thread that owns the runtime.
    std::mutex commandMutex_;
    std::vector<api::UiCommand> pendingCommands_;
    int commandPipe_[2] = {-1, -1};
    std::unique_ptr<fcitx::EventSourceIO> commandEvent_;

    fcitx::SimpleAction modeAction_;
    fcitx::SimpleAction punctuationAction_;
    fcitx::SimpleAction speechAction_;
    fcitx::SimpleAction toolbarAction_;
    bool actionsRegistered_ = false;

    std::map<std::uint64_t, std::unique_ptr<fcitx::EventSourceTime>> timers_;
    std::unique_ptr<fcitx::EventSourceTime> tickTimer_;
    std::uint64_t nextTimer_ = 1;

    std::vector<std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>>> handlers_;

    api::Epoch epoch_ = 1;
    api::ContextId nextContext_ = 1;
    api::SessionKey activeKey_{};
    std::size_t lastClientCount_ = 0;
    std::string modeLabel_ = "中";
    std::string punctuationLabel_ = "全角";
    bool speechActive_ = false;
    bool speechAttached_ = false;
};

} // namespace superior::fcitx5
