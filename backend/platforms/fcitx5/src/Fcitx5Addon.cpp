// SPDX-License-Identifier: GPL-3.0-or-later
#include "Fcitx5Addon.h"

#include <fcntl.h>
#include <unistd.h>

#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>

#include <fcitx/event.h>
#include <fcitx/inputmethodentry.h>

#include <algorithm>
#include <chrono>
#include <utility>

#include "superior/config/Settings.hpp"
#include "SpeechProcess.h"
#include "superior/diagnostics/Log.hpp"
#include "superior/fcitx5/InlineText.hpp"
#include "superior/fcitx5/KeyTranslation.hpp"
#include "superior/protocol/Wire.hpp"

#include "PanelCallbacks.hpp"

namespace superior::fcitx5 {

namespace diag = ::superior::diagnostics;

namespace {

/// Candidate list that reports the capability's pagination, not its own.
///
/// The list only ever holds one page: the pinyin capability hands out the
/// current page. Without these overrides the page indicator would always read
/// "1/1" and wheel paging would silently do nothing.
class RuntimeCandidateList final : public fcitx::CommonCandidateList {
public:
    using Pager = std::function<void(bool forward)>;

    void setPagination(int pageIndex, int pageCount, bool hasPrevious, bool hasNext) {
        pageIndex_ = pageIndex;
        pageCount_ = pageCount;
        hasPrevious_ = hasPrevious;
        hasNext_ = hasNext;
    }

    void setPager(Pager pager) { pager_ = std::move(pager); }

    [[nodiscard]] int totalPages() const override { return pageCount_; }
    [[nodiscard]] int currentPage() const override { return pageIndex_; }
    [[nodiscard]] bool hasPrev() const override { return hasPrevious_; }
    [[nodiscard]] bool hasNext() const override { return hasNext_; }

    void prev() override { dispatch(false); }
    void next() override { dispatch(true); }

private:
    void dispatch(bool forward) {
        // Copy first: the pager rebuilds the panel, which may drop the last
        // reference to this list while `dispatch` is still on the stack.
        auto pager = pager_;
        if (pager) {
            pager(forward);
        }
    }

    int pageIndex_ = 0;
    int pageCount_ = 0;
    bool hasPrevious_ = false;
    bool hasNext_ = false;
    Pager pager_;
};

/// A candidate that reports its stable id, never a row index.
///
/// The runtime re-validates the id against the current generation, so a click
/// that arrives after the list changed is rejected instead of selecting a
/// different word.
class RuntimeCandidateWord final : public fcitx::CandidateWord {
public:
    using Selector = std::function<void(api::CandidateId)>;

    RuntimeCandidateWord(fcitx::Text text, fcitx::Text comment, api::CandidateId id,
                         Selector selector)
        : fcitx::CandidateWord(std::move(text)), id_(id), selector_(std::move(selector)) {
        if (!comment.empty()) {
            setComment(std::move(comment));
        }
    }

    void select(fcitx::InputContext *) const override {
        auto selector = selector_;
        if (selector) {
            selector(id_);
        }
    }

private:
    api::CandidateId id_ = 0;
    Selector selector_;
};

/// The Unicode codepoint a keysym names, or 0 when it names no character.
///
/// X11 encodes a Unicode keysym as 0x01000000 + codepoint; Latin-1 keysyms use
/// the codepoint directly. This keeps non-ASCII punctuation working without
/// teaching the runtime or the capability a keysym number.
char32_t codepointFor(std::uint32_t keysym) {
    if (keysym >= 0x01000000U && keysym <= 0x0110FFFFU) {
        return static_cast<char32_t>(keysym - 0x01000000U);
    }
    if ((keysym >= 0x20U && keysym <= 0x7EU) || (keysym >= 0xA0U && keysym <= 0xFFU)) {
        return static_cast<char32_t>(keysym);
    }
    return 0;
}

std::string_view reasonName(api::DegradedReason reason) {
    switch (reason) {
    case api::DegradedReason::SkinUnavailable:
        return "skin-unavailable";
    case api::DegradedReason::PopupUnavailable:
        return "popup-unavailable";
    case api::DegradedReason::VoiceUnavailable:
        return "voice-unavailable";
    case api::DegradedReason::PresenterUnavailable:
        return "presenter-unavailable";
    case api::DegradedReason::EngineUnavailable:
        return "engine-unavailable";
    case api::DegradedReason::IpcDisconnected:
        return "ipc-disconnected";
    }
    return "unknown";
}

} // namespace

Fcitx5Addon::Fcitx5Addon(fcitx::AddonManager *manager)
    : manager_(manager),
      instance_(manager != nullptr ? manager->instance() : nullptr),
      overlay_(instance_),
      pinyinFactory_(paths_, learning_, cloud_),
      textSession_(
          [this](const api::SessionKey &key) { return contextFor(key); },
          [this]() { return activeKey_; }),
      speechOutbox_(textSession_),
      speechController_(
          SpeechController::Deps{
              .outbox = [this]() -> core::TextOutbox & { return speechOutbox_; },
              .caretRefusesDictation =
                  [this]() {
                      auto *context = activeContext();
                      return context != nullptr && sensitive(*context);
                  },
              .repaint =
                  [this]() {
                      if (auto *context = activeContext()) {
                          runtime_.hostUpdate(keyFor(context), hostFacts(*context));
                      }
                  },
              .createCapability =
                  [this]() -> std::unique_ptr<SpeechCapability> {
                      return std::make_unique<SpeechProcess>(instance_);
                  },
              .nowMs = [this]() { return nowMs(); },
              .layout = {},
          }),
      runtime_(runtime::Ports{textSession_, *this, *this, *this, this, this}, pinyinFactory_,
               *this, runtime::Options{config::defaultSettingsPath(),
                                       "org.superior.default"}) {
    diag::configureFromEnvironment();
    learning_.load();
    overlay_.setCommandHandler([this](std::string bytes) { handleCommand(std::move(bytes)); });
    registerActions();
    overlay_.ensure();
    wireEvents();
    if (::pipe2(commandPipe_, O_NONBLOCK | O_CLOEXEC) == 0 && instance_ != nullptr) {
        commandEvent_ = instance_->eventLoop().addIOEvent(
            commandPipe_[0], fcitx::IOEventFlag::In,
            [this](fcitx::EventSourceIO *, int, fcitx::IOEventFlags) {
                char buffer[64];
                while (::read(commandPipe_[0], buffer, sizeof(buffer)) > 0) {
                }
                drainCommands();
                return true;
            });
    } else {
        SUPERIOR_WARN(diag::categories::Fcitx5)
            << "cannot create the UI command wakeup pipe; overlay clicks disabled";
    }
    startTickTimer();
    lastClientCount_ = overlay_.clientCount();
    hostReady_ = true;
    SUPERIOR_INFO(diag::categories::Fcitx5)
        .field("engine", pinyinFactory_.available())
        .field("overlay", overlay_.clientCount())
        << "fcitx5 host ready";
}

Fcitx5Addon::~Fcitx5Addon() {
    tickTimer_.reset();
    timers_.clear();
    handlers_.clear();
    if (actionsRegistered_ && instance_ != nullptr) {
        instance_->userInterfaceManager().unregisterAction(&modeAction_);
        instance_->userInterfaceManager().unregisterAction(&punctuationAction_);
        instance_->userInterfaceManager().unregisterAction(&speechAction_);
        instance_->userInterfaceManager().unregisterAction(&toolbarAction_);
        actionsRegistered_ = false;
    }
    overlay_.shutdown();
    commandEvent_.reset();
    if (commandPipe_[0] >= 0) {
        ::close(commandPipe_[0]);
        commandPipe_[0] = -1;
    }
    if (commandPipe_[1] >= 0) {
        ::close(commandPipe_[1]);
        commandPipe_[1] = -1;
    }
}

// ---------------------------------------------------------------------------
// InputMethodEngineV2
// ---------------------------------------------------------------------------

void Fcitx5Addon::keyEvent(const fcitx::InputMethodEntry &entry, fcitx::KeyEvent &event) {
    (void)entry;
    auto *context = event.inputContext();
    if (context == nullptr || instance_ == nullptr) {
        return;
    }
    if (instance_->inputMethod(context) != "super-ime") {
        return;
    }

    const api::SessionKey key = keyFor(context);
    const api::HostFacts host = hostFacts(*context);
    const fcitx::KeySym sym = event.key().sym();
    const bool release = event.isRelease();
    const auto now = std::chrono::steady_clock::now();

    if (sym == FcitxKey_Shift_L || sym == FcitxKey_Shift_R) {
        ShiftTapDetector &tap = shiftTaps_[context];
        const auto which = sym == FcitxKey_Shift_L ? ShiftTapDetector::ShiftKey::Left
                                                   : ShiftTapDetector::ShiftKey::Right;
        const auto result =
            release ? tap.shiftReleased(which, now) : tap.shiftPressed(which, now);
        if (result == ShiftTapDetector::Result::Toggle) {
            static_cast<void>(runtime_.gesture(key, api::Gesture::ShiftTap, host));
        }
        // Never filter Shift: the application still needs modifier state for
        // selection and shortcuts. A tap only changes our inline mode.
        return;
    }

    if (sym == FcitxKey_Control_R) {
        ModifierTap &tap = ctrlTaps_[context];
        if (release) {
            if (tap.released(now)) {
                static_cast<void>(runtime_.gesture(key, api::Gesture::RightCtrlTap, host));
            }
        } else {
            tap.pressed(now);
        }
        return;
    }

    if (!release) {
        shiftTaps_[context].otherKeyPressed();
        ctrlTaps_[context].otherKeyPressed();
    }

    if (!release && sym == FcitxKey_Escape && busy()) {
        if (runtime_.gesture(key, api::Gesture::EscapeWhileSpeaking, host)) {
            event.filterAndAccept();
            return;
        }
    }

    const fcitx::KeyStates states = event.key().states();
    const bool shift = states.test(fcitx::KeyState::Shift);
    const auto symValue = static_cast<std::uint32_t>(sym);
    const std::optional<std::string> inlineText = inlineTextFor(symValue, shift);
    const std::optional<std::string> inlineBase = inlineTextFor(symValue, false);
    const char32_t typed =
        (inlineText && !inlineText->empty())
            ? static_cast<char32_t>(static_cast<unsigned char>((*inlineText)[0]))
            : codepointFor(symValue);
    const char32_t base =
        (inlineBase && !inlineBase->empty())
            ? static_cast<char32_t>(static_cast<unsigned char>((*inlineBase)[0]))
            : codepointFor(symValue);

    api::KeyPress press;
    press.code = keyCodeFor(symValue);
    press.text = typed;
    press.baseText = base;
    press.shift = shift;
    press.control = states.test(fcitx::KeyState::Ctrl);
    press.alt = states.test(fcitx::KeyState::Alt);
    press.super = states.test(fcitx::KeyState::Super);
    press.release = release;

    if (runtime_.key(key, press, host)) {
        event.filterAndAccept();
    }
}

void Fcitx5Addon::activate(const fcitx::InputMethodEntry &entry,
                           fcitx::InputContextEvent &event) {
    (void)entry;
    onFocusIn(event.inputContext());
}

void Fcitx5Addon::deactivate(const fcitx::InputMethodEntry &entry,
                             fcitx::InputContextEvent &event) {
    (void)entry;
    onFocusOut(event.inputContext());
}

void Fcitx5Addon::reset(const fcitx::InputMethodEntry &entry,
                        fcitx::InputContextEvent &event) {
    (void)entry;
    auto *context = event.inputContext();
    if (context == nullptr) {
        return;
    }
    const api::SessionKey key = keyIfKnown(context);
    if (key.valid()) {
        runtime_.reset(key);
    }
}

std::string Fcitx5Addon::subMode(const fcitx::InputMethodEntry &entry,
                                 fcitx::InputContext &inputContext) {
    (void)entry;
    const api::SessionKey key = keyIfKnown(&inputContext);
    return key.valid() && runtime_.english(key) ? "en" : "zh";
}

std::string Fcitx5Addon::subModeLabelImpl(const fcitx::InputMethodEntry &entry,
                                          fcitx::InputContext &inputContext) {
    (void)entry;
    const api::SessionKey key = keyIfKnown(&inputContext);
    return key.valid() && runtime_.english(key) ? "英" : "中";
}

// ---------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------

bool Fcitx5Addon::onOwnerThread() const noexcept { return true; }

void Fcitx5Addon::post(std::function<void()> work) {
    // The runtime runs on Fcitx5's owner thread; a direct call preserves the
    // ordering the contract promises.
    if (work) {
        work();
    }
}

std::uint64_t Fcitx5Addon::nowMs() const {
    return static_cast<std::uint64_t>(fcitx::now(CLOCK_MONOTONIC) / 1000U);
}

std::uint64_t Fcitx5Addon::after(std::uint64_t delayMs, std::function<void()> work) {
    if (instance_ == nullptr) {
        return 0;
    }
    const std::uint64_t id = nextTimer_++;
    const std::uint64_t usec =
        fcitx::now(CLOCK_MONOTONIC) + (delayMs > 0 ? delayMs : 1) * 1000U;
    timers_[id] = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, usec, 0,
        [this, id, work = std::move(work)](fcitx::EventSourceTime *source, std::uint64_t) {
            if (work) {
                work();
            }
            // The entry stays in the map until `cancel` or shutdown: destroying
            // an event source from inside its own callback is not safe.
            source->setEnabled(false);
            static_cast<void>(id);
            return false;
        });
    return id;
}

void Fcitx5Addon::cancel(std::uint64_t timerId) {
    const auto found = timers_.find(timerId);
    if (found == timers_.end()) {
        return;
    }
    found->second->setEnabled(false);
    timers_.erase(found);
}

std::uint64_t Fcitx5Addon::nextFrame(std::function<void(std::uint64_t timeMs)> work) {
    return after(16, [this, work = std::move(work)]() {
        if (work) {
            work(nowMs());
        }
    });
}

void Fcitx5Addon::present(const api::UiSceneFrame &frame) {
    const std::size_t clients = overlay_.clientCount();
    overlay_.publish(protocol::encodeScene(frame));
    if (clients != lastClientCount_) {
        lastClientCount_ = clients;
        syncAllPanelOwnership();
    }
}

void Fcitx5Addon::setHitMap(const api::SessionKey &session, const api::HitMap &hitMap) {
    (void)session;
    (void)hitMap;
}

void Fcitx5Addon::setVisible(const api::SessionKey &session, bool visible) {
    (void)session;
    (void)visible;
}

void Fcitx5Addon::show(const api::SessionKey &session, const api::NativePanel &panel) {
    auto *context = contextFor(session);
    if (context == nullptr) {
        return;
    }
    // Self-healing: anything that resets the panel without going through
    // `resetPanelKeepingCallbacks` (another addon, a client-side reset) would
    // hand the list to ClassicUI and show a second candidate window. Re-assert
    // ownership before every list update; the check is a `std::function` test.
    syncPanelOwnership(context);

    auto &inputPanel = context->inputPanel();
    if (!inputPanel.customInputPanelCallback() && overlay_.clientCount() > 0) {
        // The invariant `syncPanelOwnership` just established. If it does not
        // hold, ClassicUI is about to draw this list too, so say so instead of
        // letting "two skins" look like a paint bug.
        SUPERIOR_WARN(diag::categories::Fcitx5)
            .field("program", context->program())
            << "panel callback missing while drawing our candidate list";
    }

    fcitx::Text preedit(panel.preedit.text);
    preedit.setCursor(static_cast<int>(panel.preedit.cursor));
    inputPanel.setPreedit(preedit);

    if (panel.page.items.empty()) {
        inputPanel.setCandidateList(nullptr);
        context->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        return;
    }

    const int pageIndex = panel.pageIndex;
    const int pageCount = panel.pageCount;
    const bool hasPrevious = pageIndex > 0;
    const bool hasNext = pageCount == 0 || pageIndex + 1 < pageCount;

    std::vector<std::string> texts;
    texts.reserve(panel.page.items.size());
    for (const api::Candidate &candidate : panel.page.items) {
        // Text, comment and id are the list's identity. The id carries both the
        // generation and the row, so a page change with the same words still
        // rebuilds instead of keeping a click target that points elsewhere.
        texts.push_back(candidate.text + '\x1f' + candidate.comment + '\x1e' +
                        std::to_string(candidate.id));
    }

    auto current = std::dynamic_pointer_cast<RuntimeCandidateList>(inputPanel.candidateList());
    if (current != nullptr && nativeTexts_[session] == texts &&
        current->size() == static_cast<int>(panel.page.items.size())) {
        current->setPagination(pageIndex, pageCount, hasPrevious, hasNext);
        context->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        return;
    }

    auto list = std::make_unique<RuntimeCandidateList>();
    list->setLayoutHint(fcitx::CandidateLayoutHint::Vertical);
    list->setPagination(pageIndex, pageCount, hasPrevious, hasNext);
    list->setPageSize(static_cast<int>(panel.page.items.size()));

    for (const api::Candidate &candidate : panel.page.items) {
        list->append<RuntimeCandidateWord>(
            fcitx::Text(candidate.text), fcitx::Text(candidate.comment), candidate.id,
            [this, session](api::CandidateId id) {
                runtime_.command(api::ActivateCandidate{session, id});
            });
    }
    list->setPager([this, session](bool forward) {
        runtime_.command(api::PageCandidates{session, forward ? 1 : -1});
    });
    nativeTexts_[session] = std::move(texts);

    inputPanel.setCandidateList(std::move(list));
    context->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void Fcitx5Addon::hide(const api::SessionKey &session) {
    auto *context = contextFor(session);
    if (context == nullptr) {
        return;
    }
    nativeTexts_.erase(session);
    clearPanelKeepingCallback(context->inputPanel());
    context->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void Fcitx5Addon::setStatusItems(std::span<const api::StatusItem> items) {
    std::string modeLabel = "中";
    std::string punctuationLabel = "全角";
    bool speechActive = false;
    bool hasSpeech = false;
    for (const api::StatusItem &item : items) {
        if (item.id == "inline-mode") {
            if (!item.label.empty()) {
                modeLabel = item.label;
            }
        } else if (item.id == "punctuation") {
            if (!item.label.empty()) {
                punctuationLabel = item.label;
            }
        } else if (item.id == "speech") {
            hasSpeech = true;
            speechActive = item.active;
        }
    }

    bool changed = false;
    if (modeLabel != modeLabel_) {
        modeLabel_ = std::move(modeLabel);
        modeAction_.setShortText(modeLabel_);
        changed = true;
    }
    if (punctuationLabel != punctuationLabel_) {
        punctuationLabel_ = std::move(punctuationLabel);
        punctuationAction_.setShortText(punctuationLabel_);
        changed = true;
    }
    if (speechActive_ != speechActive) {
        speechActive_ = speechActive;
        speechAction_.setChecked(speechActive_);
        changed = true;
    }
    if (hasSpeech != speechAttached_) {
        speechAttached_ = hasSpeech;
        attachActions(activeContext());
        changed = true;
    }
    if (changed) {
        refreshActions();
    }
}

bool Fcitx5Addon::activate(std::string_view id) {
    (void)id;
    // Every action this host owns is handled by the runtime through
    // `ToggleStatusItem`; an unknown id is not a host action.
    return false;
}

void Fcitx5Addon::degraded(api::DegradedReason reason, const std::string &detail) {
    SUPERIOR_WARN(diag::categories::Fcitx5)
        .field("reason", std::string(reasonName(reason)))
        .field("detail", detail)
        << "input method degraded";
}

void Fcitx5Addon::recovered(api::DegradedReason reason) {
    SUPERIOR_INFO(diag::categories::Fcitx5)
        .field("reason", std::string(reasonName(reason)))
        << "input method recovered";
}

void Fcitx5Addon::sessionFailed(const api::SessionKey &session, const std::string &detail) {
    SUPERIOR_ERROR(diag::categories::Fcitx5)
        .field("epoch", static_cast<std::uint64_t>(session.epoch))
        .field("context", static_cast<std::uint64_t>(session.context))
        .field("detail", detail)
        << "input session failed";
}

// ---------------------------------------------------------------------------
// SpeechPort
// ---------------------------------------------------------------------------

bool Fcitx5Addon::enabled() const { return speechController_.enabled(); }
bool Fcitx5Addon::hotkeyEnabled() const { return speechController_.hotkeyEnabled(); }
bool Fcitx5Addon::busy() const { return speechController_.busy(); }
bool Fcitx5Addon::recording() const { return speechController_.recording(); }
bool Fcitx5Addon::canStart() const { return speechController_.canStart(); }
void Fcitx5Addon::setEnabled(bool value) { speechController_.setEnabled(value); }
void Fcitx5Addon::setHotkeyEnabled(bool value) { speechController_.setHotkeyEnabled(value); }
void Fcitx5Addon::setModelId(std::string id) { speechController_.setModelId(std::move(id)); }
void Fcitx5Addon::toggle() { speechController_.togglePanel(); }
void Fcitx5Addon::toggleByHotkey() { speechController_.toggleByHotkey(); }
void Fcitx5Addon::start() { static_cast<void>(speechController_.start()); }
void Fcitx5Addon::stop() { speechController_.stop(); }
void Fcitx5Addon::cancel(std::string_view reason) { speechController_.cancel(reason); }
void Fcitx5Addon::reload() {
    speechController_.refreshSetup();
    speechController_.reconfigure();
}
void Fcitx5Addon::tick() { speechController_.tick(); }
void Fcitx5Addon::holdCommits() { speechController_.holdCommits(); }
void Fcitx5Addon::flushHeld() { speechController_.flushHeld(); }
api::SpeechScene Fcitx5Addon::scene(std::string_view skinId,
                                    api::SkinGeneration generation) const {
    return speechController_.scene(std::string(skinId), generation);
}
void Fcitx5Addon::shutdown() { speechController_.shutdown(); }

// ---------------------------------------------------------------------------
// Internal wiring
// ---------------------------------------------------------------------------

api::SessionKey Fcitx5Addon::keyFor(fcitx::InputContext *context) {
    if (context == nullptr) {
        return {};
    }
    const auto found = keys_.find(context);
    if (found != keys_.end()) {
        return found->second;
    }
    const api::SessionKey key{epoch_, nextContext_++};
    keys_.emplace(context, key);
    contexts_.emplace(key, context);
    return key;
}

api::SessionKey Fcitx5Addon::keyIfKnown(fcitx::InputContext *context) const {
    if (context == nullptr) {
        return {};
    }
    const auto found = keys_.find(context);
    return found == keys_.end() ? api::SessionKey{} : found->second;
}

fcitx::InputContext *Fcitx5Addon::contextFor(const api::SessionKey &key) const {
    if (!key.valid() || !hostReady_) {
        return nullptr;
    }
    const auto found = contexts_.find(key);
    if (found == contexts_.end() || found->second == nullptr) {
        return nullptr;
    }
    // A command that names a background context must not type into it; only
    // the focused context may be written to.
    return found->second->hasFocus() ? found->second : nullptr;
}

fcitx::InputContext *Fcitx5Addon::activeContext() const {
    // Before `hostReady_` the member maps (and `activeKey_` itself) are not
    // constructed yet; the runtime constructor can ask for a repaint.
    return hostReady_ ? contextFor(activeKey_) : nullptr;
}

api::HostFacts Fcitx5Addon::hostFacts(fcitx::InputContext &context) const {
    api::HostFacts host;
    host.program = context.program();
    host.scale = context.scaleFactor();
    host.focused = context.hasFocus();

    const auto &cursor = context.cursorRect();
    // The frontend sends the factor its rect is in: 1.0 for X11/XIM (already
    // device pixels), the client's own scale for the D-Bus path. Multiplying
    // normalises the anchor to device pixels, which is what the scene promises.
    const double cursorScale = host.scale > 0.0 ? host.scale : 1.0;
    host.cursorRect = api::Rect{static_cast<double>(cursor.left()) * cursorScale,
                                static_cast<double>(cursor.top()) * cursorScale,
                                static_cast<double>(cursor.width()) * cursorScale,
                                static_cast<double>(cursor.height()) * cursorScale};
    // A caret is a line: zero width is legitimate. Only the all-zero rect means
    // "nothing was reported" (fcitx5's Wayland frontend never sets it).
    host.cursorValid = context.hasFocus() &&
                       (cursor.width() > 0 || cursor.height() > 0 || cursor.left() != 0 ||
                        cursor.top() != 0);

    const auto flags = context.capabilityFlags();
    host.clientPreedit = flags.test(fcitx::CapabilityFlag::Preedit);
    host.surroundingText = flags.test(fcitx::CapabilityFlag::SurroundingText);
    host.password = flags.test(fcitx::CapabilityFlag::Password) ||
                    flags.test(fcitx::CapabilityFlag::PasswordOrSensitive);
    if (instance_ != nullptr) {
        host.inputMethod = instance_->inputMethod(&context);
    }
    return host;
}

void Fcitx5Addon::onFocusIn(fcitx::InputContext *context) {
    if (context == nullptr || instance_ == nullptr) {
        return;
    }
    if (instance_->inputMethod(context) != "super-ime") {
        return;
    }
    const api::SessionKey key = keyFor(context);
    activeKey_ = key;
    // Set the speech destination first: `Runtime::focusIn` flushes held
    // transcripts, and they must land in this context, not the previous one.
    speechOutbox_.setDestination(key);
    runtime_.focusIn(key, hostFacts(*context));
    syncPanelOwnership(context);
    attachActions(context);
}

void Fcitx5Addon::onFocusOut(fcitx::InputContext *context) {
    if (context == nullptr) {
        return;
    }
    const api::SessionKey key = keyIfKnown(context);
    if (key.valid()) {
        runtime_.focusOut(key);
    }
    if (activeKey_ == key) {
        activeKey_ = {};
        speechOutbox_.setDestination({});
    }
    syncPanelOwnership(context);
}

void Fcitx5Addon::onDestroyed(fcitx::InputContext *context) {
    if (context == nullptr) {
        return;
    }
    const api::SessionKey key = keyIfKnown(context);
    if (key.valid()) {
        runtime_.destroy(key);
        contexts_.erase(key);
    }
    keys_.erase(context);
    shiftTaps_.erase(context);
    ctrlTaps_.erase(context);
    preeditOwned_.erase(context);
    nativeTexts_.erase(key);
    if (activeKey_ == key) {
        activeKey_ = {};
    }
    if (speechOutbox_.destination() == key) {
        // The caret that dictation was typing into is gone; a transcript that
        // arrives later must be held, not written into whatever is focused next.
        speechOutbox_.setDestination({});
    }
}

void Fcitx5Addon::onCursorChanged(fcitx::InputContext *context) {
    if (context == nullptr) {
        return;
    }
    const api::SessionKey key = keyIfKnown(context);
    if (key.valid()) {
        runtime_.hostUpdate(key, hostFacts(*context));
    }
}

void Fcitx5Addon::onSwitchInputMethod(fcitx::InputContext *context) {
    if (context == nullptr) {
        return;
    }
    syncPanelOwnership(context);
    if (instance_ != nullptr && instance_->inputMethod(context) == "super-ime") {
        onFocusIn(context);
    } else {
        onFocusOut(context);
    }
}

void Fcitx5Addon::syncPanelOwnership(fcitx::InputContext *context) {
    if (instance_ == nullptr || context == nullptr) {
        return;
    }
    const std::string inputMethod = instance_->inputMethod(context);
    const bool ours = inputMethod == "super-ime";
    const bool hasUi = overlay_.clientCount() > 0;
    auto &panel = context->inputPanel();

    if (!ours || !hasUi) {
        releasePreeditOwnership(context);
        if (panel.customInputPanelCallback()) {
            SUPERIOR_DEBUG(diag::categories::Fcitx5)
                .field("program", context->program())
                .field("inputMethod", inputMethod)
                .field("clients", static_cast<std::uint64_t>(overlay_.clientCount()))
                << "releasing the input panel back to classicui";
            panel.setCustomInputPanelCallback({});
            context->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        }
        return;
    }

    takePreeditOwnership(context);
    if (!panel.customInputPanelCallback()) {
        SUPERIOR_DEBUG(diag::categories::Fcitx5)
            .field("program", context->program())
            .field("inputMethod", inputMethod)
            << "taking over the input panel";
        // Replaces Classic UI for this context only. Fcitx5 still routes
        // update batching through the same path; the callback forwards the new
        // facts to the runtime, which repaints the overlay.
        panel.setCustomInputPanelCallback(
            [this](fcitx::InputContext *changed) { onCursorChanged(changed); });
    }
}

void Fcitx5Addon::syncAllPanelOwnership() {
    for (auto &entry : contexts_) {
        syncPanelOwnership(entry.second);
    }
}

void Fcitx5Addon::takePreeditOwnership(fcitx::InputContext *context) {
    if (context == nullptr) {
        return;
    }
    if (preeditOwned_.find(context) != preeditOwned_.end()) {
        return;
    }
    const auto flags = context->capabilityFlags();
    if (!flags.test(fcitx::CapabilityFlag::Preedit)) {
        preeditOwned_[context] = false;
        return;
    }
    // Dropping the capability makes engines put the composition into
    // `InputPanel::preedit` (which we draw) instead of `clientPreedit` (which
    // the application draws inline in its own font and colour).
    context->setCapabilityFlags(flags.unset(fcitx::CapabilityFlag::Preedit));
    preeditOwned_[context] = true;
}

void Fcitx5Addon::releasePreeditOwnership(fcitx::InputContext *context) {
    const auto found = preeditOwned_.find(context);
    if (found == preeditOwned_.end()) {
        return;
    }
    if (found->second) {
        context->setCapabilityFlags(context->capabilityFlags() |
                                    fcitx::CapabilityFlag::Preedit);
    }
    preeditOwned_.erase(found);
}

void Fcitx5Addon::registerActions() {
    if (actionsRegistered_ || instance_ == nullptr) {
        return;
    }
    modeAction_.setShortText("中");
    modeAction_.setLongText("切换中文/英文");
    modeAction_.connect<fcitx::SimpleAction::Activated>([this](fcitx::InputContext *) {
        runtime_.command(api::ToggleStatusItem{"inline-mode"});
        refreshActions();
    });

    punctuationAction_.setShortText("全角");
    punctuationAction_.setLongText("切换全角/半角标点");
    punctuationAction_.connect<fcitx::SimpleAction::Activated>([this](fcitx::InputContext *) {
        runtime_.command(api::ToggleStatusItem{"punctuation"});
        refreshActions();
    });

    speechAction_.setCheckable(true);
    speechAction_.setChecked(false);
    speechAction_.setShortText("语音输入");
    speechAction_.setLongText("显示或隐藏语音输入窗口");
    speechAction_.connect<fcitx::SimpleAction::Activated>([this](fcitx::InputContext *) {
        runtime_.command(api::ToggleStatusItem{"speech"});
        refreshActions();
    });

    toolbarAction_.setCheckable(true);
    toolbarAction_.setChecked(runtime_.settings().showStatusBar);
    toolbarAction_.setShortText("显示工具栏");
    toolbarAction_.setLongText("显示或隐藏输入法工具栏");
    toolbarAction_.connect<fcitx::SimpleAction::Activated>(
        [this](fcitx::InputContext *) { toggleStatusBar(); });

    struct Entry {
        const char *name;
        fcitx::SimpleAction *action;
    };
    const Entry entries[] = {
        {"superior-inline-mode", &modeAction_},
        {"superior-punctuation", &punctuationAction_},
        {"superior-speech", &speechAction_},
        {"superior-toolbar", &toolbarAction_},
    };
    for (const Entry &entry : entries) {
        if (!instance_->userInterfaceManager().registerAction(entry.name, entry.action)) {
            SUPERIOR_WARN(diag::categories::Fcitx5)
                .field("action", entry.name)
                << "cannot publish a status action";
            for (const Entry &previous : entries) {
                if (previous.action == entry.action) {
                    break;
                }
                instance_->userInterfaceManager().unregisterAction(previous.action);
            }
            return;
        }
    }
    actionsRegistered_ = true;
    SUPERIOR_INFO(diag::categories::Fcitx5) << "status actions published";
}

void Fcitx5Addon::attachActions(fcitx::InputContext *context) {
    if (!actionsRegistered_ || context == nullptr) {
        return;
    }
    auto &statusArea = context->statusArea();
    const auto present = [&statusArea](const fcitx::Action *action) {
        for (auto *candidate : statusArea.allActions()) {
            if (candidate == action) {
                return true;
            }
        }
        return false;
    };
    const auto add = [&statusArea, &present](fcitx::SimpleAction &action, bool wanted) {
        const bool here = present(&action);
        if (wanted && !here) {
            statusArea.addAction(fcitx::StatusGroup::InputMethod, &action);
            return true;
        }
        if (!wanted && here) {
            statusArea.removeAction(&action);
            return true;
        }
        return false;
    };

    bool changed = false;
    changed = add(modeAction_, true) || changed;
    changed = add(punctuationAction_, true) || changed;
    changed = add(speechAction_, enabled()) || changed;
    changed = add(toolbarAction_, true) || changed;
    if (changed) {
        context->updateUserInterface(fcitx::UserInterfaceComponent::StatusArea);
    }
}

void Fcitx5Addon::refreshActions() {
    if (auto *context = activeContext()) {
        attachActions(context);
        context->updateUserInterface(fcitx::UserInterfaceComponent::StatusArea);
    }
}

void Fcitx5Addon::toggleStatusBar() {
    const std::filesystem::path path = config::defaultSettingsPath();
    config::Settings settings = config::loadSettings(path).settings;
    settings.showStatusBar = !settings.showStatusBar;
    std::string error;
    if (!config::saveSettings(path, settings, error)) {
        SUPERIOR_WARN(diag::categories::Fcitx5)
            .field("path", path.string())
            .field("error", error)
            << "cannot persist the status bar visibility";
    }
    toolbarAction_.setChecked(settings.showStatusBar);
    runtime_.reloadSettings();
    refreshActions();
}

void Fcitx5Addon::clearPanelKeepingCallback(fcitx::InputPanel &panel) {
    resetPanelKeepingCallbacks(panel);
}

bool Fcitx5Addon::sensitive(const fcitx::InputContext &context) {
    return context.capabilityFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive);
}

void Fcitx5Addon::handleCommand(std::string bytes) {
    std::size_t consumed = 0;
    protocol::WireMessage message;
    protocol::DecodeError error;
    if (protocol::tryDecodeFrame(bytes, consumed, message, error) !=
        protocol::FrameStatus::Complete) {
        SUPERIOR_WARN(diag::categories::Fcitx5)
            .field("code", error.code)
            .field("detail", error.detail)
            << "malformed frame from UI";
        return;
    }
    if (message.type != protocol::MessageType::Command) {
        return;
    }
    const auto command = protocol::decodeCommand(message.payload, error);
    if (!command) {
        SUPERIOR_WARN(diag::categories::Fcitx5)
            .field("code", error.code)
            .field("detail", error.detail)
            << "cannot decode UI command";
        return;
    }
    if (commandPipe_[1] < 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        if (pendingCommands_.size() >= 256) {
            SUPERIOR_WARN(diag::categories::Fcitx5)
                << "dropping UI command: the owner thread is not draining";
            return;
        }
        pendingCommands_.push_back(std::move(*command));
    }
    const char wake = 1;
    [[maybe_unused]] const ssize_t written = ::write(commandPipe_[1], &wake, 1);
}

void Fcitx5Addon::drainCommands() {
    std::vector<api::UiCommand> commands;
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        commands.swap(pendingCommands_);
    }
    for (const api::UiCommand &command : commands) {
        runtime_.command(command);
    }
}

void Fcitx5Addon::wireEvents() {
    if (instance_ == nullptr) {
        return;
    }
    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextFocusIn, fcitx::EventWatcherPhase::PostInputMethod,
        [this](fcitx::Event &event) {
            onFocusIn(static_cast<fcitx::InputContextEvent &>(event).inputContext());
        }));
    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextFocusOut, fcitx::EventWatcherPhase::PostInputMethod,
        [this](fcitx::Event &event) {
            onFocusOut(static_cast<fcitx::InputContextEvent &>(event).inputContext());
        }));
    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextDestroyed, fcitx::EventWatcherPhase::PostInputMethod,
        [this](fcitx::Event &event) {
            onDestroyed(static_cast<fcitx::InputContextEvent &>(event).inputContext());
        }));
    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextCursorRectChanged,
        fcitx::EventWatcherPhase::PostInputMethod, [this](fcitx::Event &event) {
            onCursorChanged(static_cast<fcitx::InputContextEvent &>(event).inputContext());
        }));
    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextInputMethodActivated,
        fcitx::EventWatcherPhase::PostInputMethod, [this](fcitx::Event &event) {
            syncPanelOwnership(
                static_cast<fcitx::InputContextEvent &>(event).inputContext());
        }));
    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextInputMethodDeactivated,
        fcitx::EventWatcherPhase::PostInputMethod, [this](fcitx::Event &event) {
            auto *context = static_cast<fcitx::InputContextEvent &>(event).inputContext();
            if (context != nullptr) {
                SUPERIOR_DEBUG(diag::categories::Fcitx5)
                    .field("program", context->program())
                    << "input method deactivated; classicui takes the panel back";
                context->inputPanel().setCustomInputPanelCallback({});
                releasePreeditOwnership(context);
            }
        }));
    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextSwitchInputMethod,
        fcitx::EventWatcherPhase::PostInputMethod, [this](fcitx::Event &event) {
            onSwitchInputMethod(
                static_cast<fcitx::InputContextEvent &>(event).inputContext());
        }));
}

void Fcitx5Addon::startTickTimer() {
    if (instance_ == nullptr) {
        return;
    }
    tickTimer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 40000, 0,
        [this](fcitx::EventSourceTime *source, std::uint64_t) {
            runtime_.tick();
            const std::size_t clients = overlay_.clientCount();
            if (clients != lastClientCount_) {
                lastClientCount_ = clients;
                syncAllPanelOwnership();
            }
            if (!overlay_.ownsSocket()) {
                overlay_.ensure();
            }
            source->setNextInterval(40000);
            source->setOneShot();
            return true;
        });
}

} // namespace superior::fcitx5

namespace {

class Fcitx5AddonFactory final : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new superior::fcitx5::Fcitx5Addon(manager);
    }
};

} // namespace

FCITX_ADDON_FACTORY_V2(superiorpinyin, Fcitx5AddonFactory);
