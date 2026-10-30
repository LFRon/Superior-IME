// SPDX-License-Identifier: GPL-3.0-or-later
#include "SuperiorAgent.h"

#include <fcitx-utils/event.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/log.h>
#include <fcitx/action.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputpanel.h>
#include <fcitx/statusarea.h>
#include <fcitx/text.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "superior/diagnostics/Log.hpp"
#include "superior/config/Settings.hpp"
#include "superior/core/KeyPolicy.hpp"
#include "superior/paths/Roots.hpp"
#include "superior/speech_recognition/Layout.hpp"
#include "superior/speech_recognition/Models.hpp"
#include "superior/speech_recognition/Protocol.hpp"
#include "superior/fcitx5/InlineText.hpp"
#include "superior/fcitx5/KeyTranslation.hpp"
#include "superior/fcitx5/PreeditSource.hpp"
#include "superior/fcitx5/ShiftTap.hpp"
#include "superior/fcitx5/StatusMapping.hpp"
#include "superior/protocol/Wire.hpp"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace superior::fcitx5 {

namespace diag = ::superior::diagnostics;

namespace {

std::size_t byteCursor(const fcitx::Text &text) {
    const int cursor = text.cursor();
    if (cursor < 0) {
        return text.toString().size();
    }
    return static_cast<std::size_t>(cursor);
}

/// True while the engine holds something the user is still composing.
///
/// A live candidate list counts even with an empty preedit: the engine showing
/// words means the user is mid-word, which is the state the Shift tap has to
/// recognise before it can decide between committing and switching language.
bool panelComposing(const fcitx::InputPanel &panel) {
    return !panel.preedit().toString().empty() || !panel.clientPreedit().toString().empty() ||
           panel.candidateList() != nullptr;
}

/// Marker shown while a cloud answer is still on its way. Matches upstream's
/// glyph so a user who knows fcitx5-cloudpinyin recognises it immediately.
constexpr std::string_view kCloudPlaceholder = "☁";

/// Waits this long for the composition to settle before asking the provider.
///
/// Two reasons, and the second one is why 120 ms was not enough:
/// - typing `nihao` would otherwise fire four requests (`ni`, `nih`, `niha`,
///   `nihao`), and with a two-character minimum that is the common case;
/// - a debounce shorter than a keystroke interval still fires mid-word, so the
///   answer arrives for a prefix the user has already finished typing.
constexpr auto kCloudDebounce = std::chrono::milliseconds(200);
/// Tick while a request or a debounce is outstanding. 40 ms keeps the answer
/// snappy without making an idle agent wake up at all.
constexpr std::uint64_t kCloudTickUs = 40000;

/// Monotonic milliseconds, for "how long has this been in this state".
std::uint64_t steadyMs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

/// A candidate that belongs to us rather than to the engine.
///
/// It carries its own commit path because the engine has no idea this word
/// exists: the pinyin is ours to drop, and the text is ours to insert.
class CloudCandidateWord : public fcitx::CandidateWord {
public:
    using CommitHandler = std::function<void(fcitx::InputContext *, std::string_view)>;

    CloudCandidateWord(fcitx::Text text, std::string commit, CommitHandler handler)
        : fcitx::CandidateWord(std::move(text)), commit_(std::move(commit)),
          handler_(std::move(handler)) {}

    void select(fcitx::InputContext *inputContext) const override {
        if (handler_) {
            handler_(inputContext, commit_);
        }
    }

private:
    std::string commit_;
    CommitHandler handler_;
};} // namespace

SuperiorAgent::SuperiorAgent(fcitx::AddonManager *manager)
    : manager_(manager), instance_(manager != nullptr ? manager->instance() : nullptr),
      overlay_(instance_),
      // The speech panel's decision layer. Its three effects reach back here,
      // and its capability factory is what keeps the process type out of the
      // controller (a test hands it a fake instead).
      speech_({// Where a transcript goes: the outbox is the only text exit (§6.6).
               [this]() -> core::TextOutbox & { return outbox_; },
               // A password field refuses dictation; the decision to refuse is
               // the controller's, the fact is the host's.
               [this] {
                   return activeContext_ != nullptr &&
                          activeContext_->capabilityFlags().testAny(
                              fcitx::CapabilityFlag::PasswordOrSensitive);
               },
               // Every speech transition ends with a repaint of the same scene.
               [this] { refresh(activeContext_); },
               // The recogniser child: a factory, so the controller never names
               // the process type and the tests can hand it a fake.
               [this] {
                   return std::unique_ptr<SpeechCapability>(
                       std::make_unique<SpeechProcess>(instance_));
               },
               // Monotonic milliseconds, for "how long has this been stuck".
               [] { return steadyMs(); },
               // Layout defaults (engine/model paths and presence) are the
               // catalogue's own helpers; nothing to override here.
               {}}),
      // `TextSession` asks the agent two questions and nothing else: "which
      // context is this key" and "where is the caret now". Both are the agent's
      // own state, so the port stays a pure sink (§6.7).
      textSession_(
          [this](const api::SessionKey &key) { return contextFor(key); },
          [this]() -> api::SessionKey {
              if (activeContext_ == nullptr) {
                  return {};
              }
              const core::ImeSession *session = findSession(activeContext_);
              return session != nullptr ? session->key() : api::SessionKey{};
          }),
      outbox_(textSession_) {
    // Fcitx5 starts us with stderr on /dev/null, so the default stderr sink
    // records nothing at all. Give the agent its own file first.
    static_cast<void>(diag::ensureLogFile("ime"));
    diag::configureFromEnvironment();

    if (instance_ == nullptr) {
        SUPERIOR_ERROR(diag::categories::Fcitx5)
            << "no Fcitx instance; agent disabled";
        return;
    }

    SUPERIOR_INFO(diag::categories::Fcitx5)
        .field("version", std::string_view("0.1.0"))
        .field("epoch", static_cast<std::uint64_t>(epoch_))
        << "superior agent starting (observer only)";

    // Before the first settings read: `reloadSettings` publishes the real value
    // of the toolbar switch through these actions.
    registerTrayActions();

    // Our own record of cloud picks. The engine's learning lives in Fcitx5's
    // data directory and is not ours to read; this file only covers the words we
    // committed ourselves.
    {
        const auto loadedLearning = config::loadLearning(config::defaultLearningPath());
        learned_ = std::move(loadedLearning.entries);
        for (const auto &warning : loadedLearning.warnings) {
            SUPERIOR_WARN(diag::categories::Cloud)
                .field("warning", warning)
                << "learning file rejected part of its content";
        }
        SUPERIOR_DEBUG(diag::categories::Cloud)
            .field("entries", static_cast<std::int64_t>(learned_.size()))
            .field("path", config::defaultLearningPath().string())
            << "learning loaded";
    }

    overlay_.setCommandHandler([this](std::string bytes) { handleCommand(std::move(bytes)); });
    overlay_.open(protocol::SceneServer::defaultSocketPath());

    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextFlushUI, fcitx::EventWatcherPhase::PostInputMethod,
        [this](fcitx::Event &event) {
            auto &flush = static_cast<fcitx::InputContextFlushUIEvent &>(event);
            // The status area is a scene input too. A switch flipped from
            // Fcitx5's own tray menu reaches the engine as a plain
            // `Action::activate`, so nothing else would repaint the toolbar and
            // its crescent would keep the old width until the next keystroke.
            if (flush.component() == fcitx::UserInterfaceComponent::InputPanel ||
                flush.component() == fcitx::UserInterfaceComponent::StatusArea) {
                refresh(flush.inputContext());
            }
        }));

    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextFocusIn, fcitx::EventWatcherPhase::PostInputMethod,
        [this](fcitx::Event &event) {
            auto *inputContext = static_cast<fcitx::InputContextEvent &>(event).inputContext();
            SUPERIOR_DEBUG(diag::categories::Fcitx5)
                .field("program", inputContext->program())
                << "input context focused";
            reloadSettings();
            overlay_.ensure();
            if (!isOurs(inputContext)) {
                if (activeContext_ != nullptr) {
                    activeContext_ = nullptr;
                    broadcastHidden();
                }
                return;
            }
            refresh(inputContext);
            speech_.flushHeld();
        }));

    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextFocusOut, fcitx::EventWatcherPhase::PostInputMethod,
        [this](fcitx::Event &event) {
            auto *inputContext = static_cast<fcitx::InputContextEvent &>(event).inputContext();
            SUPERIOR_DEBUG(diag::categories::Fcitx5)
                .field("program", inputContext->program())
                << "input context lost focus";
            if (!isOurs(inputContext)) {
                return;
            }
            // Losing focus mid-utterance is normal — clicking the microphone, then
            // the window you want the text in. Cancelling there threw away
            // everything the user had just said (and used to show a bogus
            // "the engine returned nothing" error on top). Instead the session
            // keeps recording and the text is held until a client is focused
            // again, so nothing is lost and nothing lands in the wrong place.
            speech_.holdCommits();
            if (auto *session = findSession(inputContext)) {
                session->hide();
                broadcast(inputContext);
            }
            // Drop physical key state: a Shift held while focus moves must not
            // make the next tap look like a modifier use.
            if (auto it = shiftTaps_.find(inputContext); it != shiftTaps_.end()) {
                it->second.reset();
            }
            if (auto it = ctrlTaps_.find(inputContext); it != ctrlTaps_.end()) {
                it->second.reset();
            }
        }));

    // Installed as early as possible: the first InputPanel draw must already be
    // routed to us, otherwise Classic UI paints one frame that it never hides
    // afterwards (its own update path is then bypassed).
    reloadSettings();
    overlay_.ensure();

    // Fcitx5 may already have a focused client when the addon loads (Fcitx5
    // restarted while an application kept focus). No FocusIn event will follow
    // in that case, so the toolbar would stay invisible until the user types.
    // Paint once from the most recent context after the UI had time to connect.
    startupTimer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 600000, 0,
        [this](fcitx::EventSourceTime *, std::uint64_t) {
            if (auto *inputContext = instance_->mostRecentInputContext();
                inputContext != nullptr && isOurs(inputContext)) {
                SUPERIOR_DEBUG(diag::categories::Fcitx5)
                    .field("program", inputContext->program())
                    << "initial paint from the most recent input context";
                refresh(inputContext);
            }
            return false;
        });

    // Poll the settings file so a switch in the settings window takes effect
    // without restarting Fcitx5 or refocusing a window.
    settingsTimer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 500000, 0,
        [this](fcitx::EventSourceTime *source, std::uint64_t) {
            reloadSettings();
            speech_.tick();
            // The socket can vanish while the user stays in one window: an
            // Fcitx5 handover, a stalled agent, a cleaned-up runtime directory.
            // Focus events are the only other thing that rebuilds it, and
            // waiting for one left the panels dead until the user happened to
            // switch windows. One lstat every half second is the whole cost.
            if (!overlay_.ownsSocket()) {
                overlay_.ensure();
            }
            source->setNextInterval(500000);
            source->setOneShot();
            return true;
        });

    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextKeyEvent, fcitx::EventWatcherPhase::PreInputMethod,
        [this](fcitx::Event &event) {
            static_cast<void>(handleKeyEvent(static_cast<fcitx::KeyEvent &>(event)));
        }));

    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextInputMethodActivated,
        fcitx::EventWatcherPhase::PostInputMethod, [this](fcitx::Event &event) {
            auto *inputContext = static_cast<fcitx::InputContextEvent &>(event).inputContext();
            syncPanelOwnership(inputContext);
        }));

    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextInputMethodDeactivated,
        fcitx::EventWatcherPhase::PostInputMethod, [this](fcitx::Event &event) {
            auto *inputContext = static_cast<fcitx::InputContextEvent &>(event).inputContext();
            inputContext->inputPanel().setCustomInputPanelCallback({});
            releasePreeditOwnership(inputContext);
        }));

    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextCursorRectChanged,
        fcitx::EventWatcherPhase::PostInputMethod, [this](fcitx::Event &event) {
            auto *inputContext = static_cast<fcitx::InputContextEvent &>(event).inputContext();
            const auto &rect = inputContext->cursorRect();
            SUPERIOR_TRACE(diag::categories::Fcitx5)
                .field("x", static_cast<std::int64_t>(rect.left()))
                .field("y", static_cast<std::int64_t>(rect.top()))
                .field("w", static_cast<std::int64_t>(rect.width()))
                .field("h", static_cast<std::int64_t>(rect.height()))
                << "cursor rect changed";
            refresh(inputContext);
        }));

    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextSwitchInputMethod,
        fcitx::EventWatcherPhase::PostInputMethod, [this](fcitx::Event &event) {
            auto *inputContext = static_cast<fcitx::InputContextEvent &>(event).inputContext();
            SUPERIOR_DEBUG(diag::categories::Fcitx5)
                .field("program", inputContext->program())
                << "input method switched";
            syncPanelOwnership(inputContext);
            refresh(inputContext);
        }));

    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextDestroyed, fcitx::EventWatcherPhase::PostInputMethod,
        [this](fcitx::Event &event) {
            auto *inputContext = static_cast<fcitx::InputContextEvent &>(event).inputContext();
            SUPERIOR_DEBUG(diag::categories::Fcitx5) << "input context destroyed";
            inputContext->inputPanel().setCustomInputPanelCallback({});
            // The context is going away; dropping the entry is enough, its
            // capability flags die with it.
            preeditOwnership_.erase(inputContext);
            shiftTaps_.erase(inputContext);
            ctrlTaps_.erase(inputContext);
            cloudContexts_.erase(inputContext);
            if (cloudPendingContext_ == inputContext) {
                cloudPendingContext_ = nullptr;
                cloudPendingPinyin_.clear();
            }
            if (activeContext_ == inputContext) {
                activeContext_ = nullptr;
            }
            forget(inputContext);
        }));
}

SuperiorAgent::~SuperiorAgent() {
    speech_.shutdown();
    settingsTimer_.reset();
    startupTimer_.reset();
    cloudTimer_.reset();
    // Drop sockets before libcurl's handles go away: a transfer outliving the
    // event loop would be polled by nobody and leak its socket.
    cloudFetcher_.cancelAll();
    SUPERIOR_INFO(diag::categories::Fcitx5)
        .field("sessions", static_cast<std::uint64_t>(sessions_.size()))
        << "superior agent stopping";
    if (trayActionsRegistered_) {
        instance_->userInterfaceManager().unregisterAction(&toolbarAction_);
        instance_->userInterfaceManager().unregisterAction(&speechAction_);
        trayActionsRegistered_ = false;
    }
    handlers_.clear();
    overlay_.shutdown();
}

core::ImeSession *SuperiorAgent::findSession(fcitx::InputContext *inputContext) {
    const auto it = sessions_.find(inputContext);
    return it == sessions_.end() ? nullptr : it->second.get();
}

core::ImeSession &SuperiorAgent::sessionFor(fcitx::InputContext *inputContext) {
    const auto it = sessions_.find(inputContext);
    if (it != sessions_.end()) {
        return *it->second;
    }
    auto session = std::make_unique<core::ImeSession>(api::SessionKey{epoch_, nextContext_++});
    core::ImeSession &reference = *session;
    sessions_.emplace(inputContext, std::move(session));
    return reference;
}

void SuperiorAgent::collect(fcitx::InputContext *inputContext, core::EngineFacts &engine,
                            core::HostFacts &host) {
    host.program = inputContext->program();
    host.scale = inputContext->scaleFactor();
    if (const auto *entry = instance_->inputMethodEntry(inputContext)) {
        host.inputMethod = entry->uniqueName();
        engine.engineId = entry->uniqueName();
    }

    const auto &cursor = inputContext->cursorRect();
    host.cursorRect = api::Rect{static_cast<double>(cursor.left()),
                                static_cast<double>(cursor.top()),
                                static_cast<double>(cursor.width()),
                                static_cast<double>(cursor.height())};
    host.cursorValid = inputContext->hasFocus();

    const auto flags = inputContext->capabilityFlags();
    host.clientPreedit = flags.test(fcitx::CapabilityFlag::Preedit);
    host.surroundingText = flags.test(fcitx::CapabilityFlag::SurroundingText);
    host.password = flags.test(fcitx::CapabilityFlag::Password) ||
                    flags.test(fcitx::CapabilityFlag::PasswordOrSensitive);

    const auto &panel = inputContext->inputPanel();
    // The composition row is drawn by us, but which field carries the text
    // depends on the engine and on the client's capabilities: pinyin writes
    // clientPreedit for "show actual pinyin" mode, which the application would
    // render inline in its own style. Taking the panel preedit first and
    // falling back to the client preedit keeps one look everywhere.
    const auto &panelPreedit = panel.preedit();
    const auto &clientPreedit = panel.clientPreedit();
    const PreeditView preedit = choosePreedit(panelPreedit.toString(), byteCursor(panelPreedit),
                                              clientPreedit.toString(),
                                              byteCursor(clientPreedit));
    engine.preedit = preedit.text;
    engine.preeditCursor = preedit.cursor;
    if (outbox_.draftShowing()) {
        // The speech draft is drawn by the application itself (client preedit).
        // Showing it here as well is the duplicate box the user asked us to remove.
        engine.preedit.clear();
        engine.preeditCursor = 0;
    }

    if (auto candidateList = panel.candidateList()) {
        const int size = candidateList->size();
        engine.candidates.reserve(static_cast<std::size_t>(std::max(0, size)));
        for (int index = 0; index < size; ++index) {
            const auto &word = candidateList->candidate(index);
            core::EngineCandidate entry;
            entry.label = candidateList->label(index).toString();
            entry.text = word.text().toString();
            entry.comment = word.comment().toString();
            engine.candidates.push_back(std::move(entry));
        }
        engine.highlighted = candidateList->cursorIndex();
        engine.vertical = candidateList->layoutHint() == fcitx::CandidateLayoutHint::Vertical;

        if (auto *pageable = candidateList->toPageable()) {
            engine.pageCount = pageable->totalPages();
            const int current = pageable->currentPage();
            engine.pageIndex = current >= 0 ? current + 1 : 1;
        }
    }

    for (auto *action : inputContext->statusArea().allActions()) {
        if (action == nullptr || action->name().empty()) {
            continue;
        }
        if (isStatusActionHidden(action->name())) {
            // Engine configuration knobs are not toolbar buttons.
            continue;
        }
        api::StatusItem item;
        item.id = action->name();
        item.label = action->shortText(inputContext);
        item.icon = action->icon(inputContext);
        // `isChecked` is authoritative when an action reports it; otherwise an
        // `-active`/`-inactive` icon pair carries the state (punctuation does
        // exactly this, which is why the moon would never turn into a circle).
        item.active = action->isChecked(inputContext) || statusIconIsOn(item.icon);
        item.role = statusRoleFor(item.id);
        SUPERIOR_TRACE(diag::categories::Fcitx5)
            .field("id", item.id)
            .field("active", item.active)
            .field("icon", item.icon)
            .redacted("label", item.label)
            << "status action collected";
        host.statusItems.push_back(std::move(item));
    }

    // The microphone is ours too. It is injected as a status item instead of a
    // Fcitx5 `SimpleAction` because the agent owns the decision and the UI
    // draws the bar: a real action would need a second addon to click into.
    if (speech_.enabled()) {
        api::StatusItem mic;
        mic.id = "speech";
        mic.label = "";
        mic.icon = "microphone";
        mic.active = speech_.recording();
        mic.role = api::StatusRole::Speech;
        host.statusItems.insert(host.statusItems.begin(), std::move(mic));
    }

    // The inline mode chip is ours, not the engine's: it mirrors `modes_`, the
    // only owner of the Chinese/English switch.
    api::StatusItem modeItem;
    modeItem.id = "inline-mode";
    modeItem.label = modes_.english() ? "英" : "中";
    modeItem.active = modes_.english();
    host.statusItems.insert(host.statusItems.begin(), std::move(modeItem));

    // L-4: a composition is visible even when the engine has no candidate yet.
    engine.composing = !engine.preedit.empty() || !engine.candidates.empty();

    // Never log the composition itself: log shape only.
    SUPERIOR_TRACE(diag::categories::Fcitx5)
        .field("program", host.program)
        .field("im", host.inputMethod)
        .redacted("preedit", engine.preedit)
        .field("preeditSource", preedit.fromClient ? "client" : "panel")
        .field("candidates", static_cast<std::uint64_t>(engine.candidates.size()))
        .field("cursorIndex", static_cast<std::int64_t>(engine.highlighted))
        .field("vertical", engine.vertical)
        .field("statusItems", static_cast<std::uint64_t>(host.statusItems.size()))
        .field("scale", host.scale, 2)
        .field("surroundingText", host.surroundingText)
        << "collected engine/host facts";
}

void SuperiorAgent::reloadSettings() {
    const std::filesystem::path path = config::defaultSettingsPath();
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(path, error);
    if (settingsLoaded_ && (error || stamp == settingsStamp_)) {
        return;
    }
    settingsStamp_ = stamp;
    settingsLoaded_ = true;

    const auto loaded = config::loadSettings(path);
    for (const auto &warning : loaded.warnings) {
        SUPERIOR_WARN(diag::categories::Fcitx5)
            .field("settings", path.string())
            .field("warning", warning)
            << "settings rejected part of the file";
    }

    const bool previousInlineEnglish = inlineEnglishEnabled_;
    inlineEnglishEnabled_ = loaded.settings.shiftToggle;
    shiftCommitPinyin_ = loaded.settings.shiftCommitPinyin;
    const bool previousSpeech = speech_.enabled();
    showStatusBar_ = loaded.settings.showStatusBar;
    speech_.setHotkeyEnabled(loaded.settings.speechRecognitionHotkey);
    // A stored id can be stale (a model that was dropped, or the *old* default
    // that the user never chose). Resolving it here keeps a dead id from turning
    // into "nothing is selected" in the panel and "nothing loads" in the engine.
    const auto choice = speech_recognition::resolveStoredModel(
        loaded.settings.speechRecognitionModelId, loaded.settings.settingsVersion);
    if (choice.unknown) {
        SUPERIOR_WARN(diag::categories::SpeechRecognition)
            .field("stored", loaded.settings.speechRecognitionModelId)
            .field("using", choice.id)
            << "stored speech model is not in the catalog";
    }
    if (choice.migrated) {
        SUPERIOR_INFO(diag::categories::SpeechRecognition)
            .field("from", loaded.settings.speechRecognitionModelId)
            .field("to", choice.id)
            << "migrated the speech model default";
    }
    speech_.setModelId(choice.id);
    // Turning the feature off must also close the panel, otherwise a hidden
    // switch keeps a microphone open.
    speech_.setEnabled(loaded.settings.speechRecognitionEnabled);
    if (previousSpeech != speech_.enabled()) {
        SUPERIOR_INFO(diag::categories::SpeechRecognition)
            .field("enabled", speech_.enabled())
            << "speech recognition toolbar entry changed";
    }
    if (!loaded.settings.skinId.empty()) {
        skinId_ = loaded.settings.skinId;
    }

    // Cloud pinyin. A provider switch invalidates every cached answer: a Baidu
    // reply must not be served while the user selected Google.
    const bool previousCloudEnabled = cloudEnabled_;
    const std::string previousBackend(cloudpinyin::backendName(cloudBackend_));
    cloudEnabled_ = loaded.settings.cloudPinyinEnabled;
    cloudMinLength_ = loaded.settings.cloudPinyinMinLength;
    cloudIndex_ = loaded.settings.cloudPinyinIndex;
    if (const auto backend = cloudpinyin::backendFromString(loaded.settings.cloudPinyinBackend)) {
        cloudBackend_ = *backend;
    }
    const std::string currentBackend(cloudpinyin::backendName(cloudBackend_));
    if (previousBackend != currentBackend) {
        cloudCache_.clear();
        cloudBudget_.recordSuccess();
        SUPERIOR_INFO(diag::categories::Cloud)
            .field("backend", currentBackend)
            << "cloud pinyin backend changed; cache cleared";
    }
    if (previousCloudEnabled != cloudEnabled_) {
        SUPERIOR_INFO(diag::categories::Cloud)
            .field("enabled", cloudEnabled_)
            .field("backend", currentBackend)
            .field("minLength", static_cast<std::int64_t>(cloudMinLength_))
            << "cloud pinyin setting changed";
    }
    if (!cloudEnabled_) {
        // Turning it off must also tear down anything in flight, otherwise a
        // late answer could still splice a candidate into the list.
        cloudFetcher_.cancelAll();
        cloudPendingPinyin_.clear();
        cloudPendingContext_ = nullptr;
        cloudContexts_.clear();
    }
    updateCloudTimer();

    SUPERIOR_DEBUG(diag::categories::Fcitx5)
        .field("inlineEnglish", inlineEnglishEnabled_)
        .field("settings", path.string())
        << "settings loaded";

    if (previousInlineEnglish != inlineEnglishEnabled_) {
        SUPERIOR_INFO(diag::categories::Fcitx5)
            .field("inlineEnglish", inlineEnglishEnabled_)
            << "inline English setting changed";
    }

    // Turning the feature off must also leave English mode; otherwise letters
    // would keep being committed directly even though the user disabled it.
    if (!inlineEnglishEnabled_ && modes_.english()) {
        setEnglishMode(activeContext_, false, "settings");
    }

    // The settings page edits the same switch the tray menu does.
    refreshTrayActions();
}

bool SuperiorAgent::commitCompositionLetters(fcitx::InputContext *inputContext) {
    if (inputContext == nullptr) {
        return false;
    }
    const auto &panel = inputContext->inputPanel();
    // Read the letters *before* anything touches the composition: the commit
    // path drops the composition first, so a later read yields nothing.
    const PreeditView view = choosePreedit(panel.preedit().toString(), byteCursor(panel.preedit()),
                                           panel.clientPreedit().toString(),
                                           byteCursor(panel.clientPreedit()));
    if (view.text.empty()) {
        // Nothing typed (an engine can hold a list with an empty preedit). The
        // caller still switches language: the user asked for English.
        return false;
    }

    // `Drop`: the letters belong to the window they were typed in. If focus
    // moved between press and release, typing them into the new window would be
    // wrong, and the old window lost them when it lost the caret.
    const core::ImeSession *session = findSession(inputContext);
    const api::SessionKey key = session != nullptr ? session->key() : api::SessionKey{};
    if (!outbox_.commit(key, view.text, core::WhenUnfocused::Drop)) {
        SUPERIOR_DEBUG(diag::categories::Fcitx5)
            .field("length", static_cast<std::int64_t>(view.text.size()))
            .field("program", inputContext->program())
            << "composition not committed: its session is no longer active";
        return false;
    }

    // The commit ends the composition, so the candidate window has to go with
    // it. `refresh` is also what re-folds the scene the overlay draws.
    refresh(inputContext);
    SUPERIOR_INFO(diag::categories::Fcitx5)
        .field("length", static_cast<std::int64_t>(view.text.size()))
        .field("fromClient", view.fromClient)
        .field("program", inputContext->program())
        << "committed the pinyin letters on a shift tap";
    return true;
}

void SuperiorAgent::setEnglishMode(fcitx::InputContext *inputContext, bool english,
                                   const char *reason) {
    // `Modes` is the only owner. A no-op must not reset the composition or
    // repaint: setLanguage reports whether anything actually changed.
    if (!modes_.setLanguage(english ? core::Language::English : core::Language::Chinese)) {
        return;
    }

    if (english && inputContext != nullptr && instance_ != nullptr) {
        // Drop the half-finished pinyin composition, otherwise its stale
        // candidate list would keep the panel visible while English letters
        // are committed directly.
        //
        // `InputContext::reset` dispatches `InputContextReset`, which is what
        // calls the engine's `reset()`. `Instance::resetCompose` only clears
        // XKB's compose state and leaves the engine composition alive. The
        // outbox expresses the same thing without naming Fcitx5.
        if (const core::ImeSession *session = findSession(inputContext); session != nullptr) {
            outbox_.setDestination(session->key());
            outbox_.abandonComposition();
        }
        inputContext->updatePreedit();
    }

    SUPERIOR_INFO(diag::categories::Fcitx5)
        .field("english", modes_.english())
        .field("reason", std::string(reason))
        .field("program", inputContext != nullptr ? inputContext->program() : std::string{})
        << "inline input mode changed";

    if (inputContext != nullptr) {
        refresh(inputContext);
    }
}

void SuperiorAgent::takePreeditOwnership(fcitx::InputContext *inputContext) {
    PreeditOwnership &state = preeditOwnership_[inputContext];
    if (state.owned) {
        return;
    }
    const auto flags = inputContext->capabilityFlags();
    // Remember only whether the client could display preedit at all, so
    // releasing restores exactly what the frontend announced instead of
    // clobbering flags someone else changed in the meantime (Password, Disable).
    state.clientShowedPreedit = flags.test(fcitx::CapabilityFlag::Preedit);
    if (!state.clientShowedPreedit) {
        // Nothing to take away: the client never drew inline preedit, so the
        // engine already fills the panel preedit.
        state.owned = true;
        return;
    }

    // Dropping the capability makes engines put the composition into
    // `InputPanel::preedit` (which we draw) instead of `clientPreedit` (which
    // the application draws inline in its own font and colour). This is how the
    // Sogou Linux build behaves, and it is why the panel looked different in
    // VSCode: VSCode renders client preedit itself.
    inputContext->setCapabilityFlags(flags.unset(fcitx::CapabilityFlag::Preedit));
    state.owned = true;
    SUPERIOR_INFO(diag::categories::Fcitx5)
        .field("program", inputContext->program())
        .field("im", instance_->inputMethod(inputContext))
        << "drawing the composition ourselves; client preedit disabled for this context";
}

void SuperiorAgent::releasePreeditOwnership(fcitx::InputContext *inputContext) {
    const auto it = preeditOwnership_.find(inputContext);
    if (it == preeditOwnership_.end()) {
        return;
    }
    if (it->second.owned && it->second.clientShowedPreedit) {
        // Add the capability back rather than restoring a snapshot: other flags
        // may have changed while we owned the panel.
        inputContext->setCapabilityFlags(inputContext->capabilityFlags() |
                                         fcitx::CapabilityFlag::Preedit);
        SUPERIOR_INFO(diag::categories::Fcitx5)
            .field("program", inputContext->program())
            << "releasing the composition back to the client";
    }
    preeditOwnership_.erase(it);
}

void SuperiorAgent::syncPanelOwnership(fcitx::InputContext *inputContext) {
    if (instance_ == nullptr || inputContext == nullptr) {
        return;
    }

    const std::string inputMethod = instance_->inputMethod(inputContext);
    const bool ours = inputMethod == "super-ime";
    const bool hasUi = overlay_.clientCount() > 0;
    auto &panel = inputContext->inputPanel();

    if (!ours) {
        // Another input method took over this context: hand the composition
        // back so Rime & friends keep their own client-side preedit.
        releasePreeditOwnership(inputContext);
        if (panel.customInputPanelCallback()) {
            panel.setCustomInputPanelCallback({});
            SUPERIOR_INFO(diag::categories::Fcitx5)
                .field("program", inputContext->program())
                .field("im", inputMethod)
                << "releasing the input panel back to Fcitx5's UI";
        }
        return;
    }

    if (!hasUi) {
        // No overlay to draw for us: fall back to Classic UI instead of leaving
        // the user with no candidates at all, and hand the composition back to
        // the client so nothing disappears.
        releasePreeditOwnership(inputContext);
        if (panel.customInputPanelCallback()) {
            panel.setCustomInputPanelCallback({});
            inputContext->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        }
        return;
    }

    takePreeditOwnership(inputContext);
    if (!panel.customInputPanelCallback()) {
        // Replaces Classic UI for this context only. Fcitx5 still routes
        // update batching through the same path, so the callback simply
        // forwards the latest state to our UI process.
        panel.setCustomInputPanelCallback(
            [this](fcitx::InputContext *context) { refresh(context); });
        SUPERIOR_INFO(diag::categories::Fcitx5)
            .field("program", inputContext->program())
            .field("im", inputMethod)
            << "taking over the input panel for this context";
    }
}

bool SuperiorAgent::handleKeyEvent(fcitx::KeyEvent &event) {
    if (instance_ == nullptr) {
        return false;
    }
    auto *inputContext = event.inputContext();
    if (inputContext == nullptr || instance_->inputMethod(inputContext) != "super-ime") {
        return false;
    }

    // Take over (or release) the panel before the engine produces anything for
    // this key, so the very first candidate frame already belongs to us.
    syncPanelOwnership(inputContext);

    ShiftTapDetector &shiftTap = shiftTaps_[inputContext];
    const fcitx::KeySym sym = event.key().sym();
    const bool release = event.isRelease();
    const auto now = std::chrono::steady_clock::now();

    if (sym == FcitxKey_Shift_L || sym == FcitxKey_Shift_R) {
        const auto shiftKey = sym == FcitxKey_Shift_L ? ShiftTapDetector::ShiftKey::Left
                                                      : ShiftTapDetector::ShiftKey::Right;
        const auto result =
            release ? shiftTap.shiftReleased(shiftKey, now) : shiftTap.shiftPressed(shiftKey, now);
        SUPERIOR_TRACE(diag::categories::Fcitx5)
            .field("sym", static_cast<std::int64_t>(sym))
            .field("release", release)
            .field("tap", result == ShiftTapDetector::Result::Toggle)
            .field("english", modes_.english())
            .field("program", inputContext->program())
            << "shift key";
        if (result == ShiftTapDetector::Result::Toggle) {
            // What a tap means is a product rule, so it is decided in
            // `core::decideGesture`; this adapter only reports that a tap
            // happened. The gate used to sit here with its own copy per gesture.
            const bool composing = panelComposing(inputContext->inputPanel());
            const auto intent = core::decideGesture(core::GestureInput{
                .gesture = core::Gesture::ShiftTap,
                .english = modes_.english(),
                .composing = composing,
                .inlineEnglishEnabled = inlineEnglishEnabled_,
                .shiftCommitPinyin = shiftCommitPinyin_,
            });
            if (intent == core::GestureIntent::CommitAndSwitchToEnglish) {
                // Order matters: switching language abandons the composition, so
                // the letters have to be read and committed while it still
                // exists. `setEnglishMode` then finds nothing left to drop.
                commitCompositionLetters(inputContext);
                setEnglishMode(inputContext, true, "shift tap: letters committed");
            } else if (intent == core::GestureIntent::ToggleLanguage) {
                setEnglishMode(inputContext, !modes_.english(), "shift tap");
            } else {
                SUPERIOR_DEBUG(diag::categories::Fcitx5)
                    .field("program", inputContext->program())
                    .field("composing", composing)
                    << "shift tap ignored: inline English is disabled in settings";
            }
        }
        // Never filter Shift: the application still needs modifier state for
        // selection and shortcuts. A tap only toggles our inline mode.
        return false;
    }

    // Right Ctrl is a second gesture key: a bare tap toggles dictation. The
    // press is never consumed, so every Ctrl shortcut keeps working.
    if (sym == FcitxKey_Control_R) {
        ModifierTap &ctrlTap = ctrlTaps_[inputContext];
        if (release) {
            if (ctrlTap.released(now) &&
                core::decideGesture(core::GestureInput{
                    .gesture = core::Gesture::RightCtrlTap,
                    .english = modes_.english(),
                    .speechEnabled = speech_.enabled(),
                    .speechHotkeyEnabled = speech_.hotkeyEnabled(),
                }) == core::GestureIntent::ToggleSpeech) {
                speech_.toggleByHotkey();
            }
        } else {
            ctrlTap.pressed(now);
        }
        return false;
    }

    if (!release) {
        // Any real key while Shift is held means "modifier", not "tap".
        shiftTap.otherKeyPressed();
        ctrlTaps_[inputContext].otherKeyPressed();
    }

    // While the microphone is live, Escape belongs to the utterance: cancel it
    // and keep the key away from the engine, which would clear a composition the
    // user cannot even see behind the panel. The panel stays open — cancelling
    // is not closing, and re-starting is one click.
    if (!release && sym == FcitxKey_Escape &&
        core::decideGesture(core::GestureInput{
            .gesture = core::Gesture::EscapeWhileSpeaking,
            .english = modes_.english(),
            .speaking = speech_.busy(),
        }) == core::GestureIntent::CancelSpeech) {
        speech_.cancel("escape");
        event.filterAndAccept();
        return true;
    }

    // Who owns this key is a product decision, so it lives in the decision
    // layer (`core::decideKey`) instead of here. This adapter only reports the
    // facts the policy cannot know: whether a composition is active and whether
    // the key produces text in the current layout.
    const auto &panel = inputContext->inputPanel();
    const bool composing = panelComposing(panel);

    const auto states = event.key().states();
    const bool shift = states.test(fcitx::KeyState::Shift);
    // The translation to a platform-free `KeyCode` happens here, once. The
    // native keysym keeps travelling to librime through `RimeInputMethod`, which
    // still holds the original event, so the decision layer never sees it.
    const std::optional<std::string> inlineText =
        inlineTextFor(static_cast<std::uint32_t>(sym), shift);
    // `inlineTextFor` returns exactly one printable ASCII character when it
    // produces anything, so the first byte is the whole character. "No text" and
    // "empty text" both mean the same thing here: this key types nothing.
    const char32_t typed =
        (inlineText && !inlineText->empty())
            ? static_cast<char32_t>(static_cast<unsigned char>((*inlineText)[0]))
            : char32_t{0};

    const core::KeyDecision decision = core::decideKey(core::KeyPolicyInput{
        .key = core::KeyPress{.code = keyCodeFor(static_cast<std::uint32_t>(sym)),
                              .text = typed,
                              .shift = shift,
                              .control = states.test(fcitx::KeyState::Ctrl),
                              .alt = states.test(fcitx::KeyState::Alt),
                              .super = states.test(fcitx::KeyState::Super),
                              .release = release},
        .composing = composing,
        .english = modes_.english(),
    });

    switch (decision.intent) {
    case core::KeyIntent::ToggleLanguage:
        setEnglishMode(inputContext, false, "escape");
        break;
    case core::KeyIntent::CommitInline:
        if (inlineText && !inlineText->empty()) {
            inputContext->commitString(*inlineText);
        }
        break;
    case core::KeyIntent::Ignore:
    case core::KeyIntent::Forward:
    case core::KeyIntent::Engine:
    case core::KeyIntent::Cancel:
        // Cancel and Engine are both "the composition is not ours to clear
        // here": the engine's own Escape/Backspace handling is correct, and
        // forwarding keeps undo, selection and IME-less widgets intact.
        return false;
    }

    if (decision.consumed) {
        event.filterAndAccept();
        return true;
    }
    return false;
}

void SuperiorAgent::refresh(fcitx::InputContext *inputContext) {
    if (instance_ == nullptr || inputContext == nullptr || refreshing_) {
        return;
    }

    // Never touch contexts that belong to another input method. Rime and the
    // rest keep their own candidates and their own Fcitx5 theme.
    if (!isOurs(inputContext)) {
        if (activeContext_ != nullptr) {
            activeContext_ = nullptr;
            outbox_.setDestination({});
            broadcastHidden();
        }
        return;
    }
    activeContext_ = inputContext;
    outbox_.setDestination(api::SessionKey{epoch_, sessionFor(inputContext).key().context});

    refreshing_ = true;
    syncPanelOwnership(inputContext);

    core::ImeSession &session = sessionFor(inputContext);
    // Cloud candidates are spliced into the engine's list *before* it is read,
    // so the frame we are about to build already contains them.
    syncCloudCandidate(inputContext);
    // The scene is folded from two independent projections: what the engine
    // produced, and what the host knows. Fcitx5 supplies both here, but a
    // Windows or Android adapter only has to supply the same two structs.
    core::EngineFacts engine;
    core::HostFacts host;
    collect(inputContext, engine, host);
    core::updateSession(engine, host, session, skinId_);
    session.setSpeech(speech_.scene(skinId_, 1));

    if (overlay_.clientCount() > 0) {
        broadcast(inputContext);
    } else {
        // Nothing to draw with: hand the panel back to Fcitx5 instead of
        // leaving the user without candidates.
        inputContext->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    }
    // Every transition that can open or close the speech panel ends up here, so
    // this is the one place that keeps its tray check mark honest.
    refreshTrayActions();
    refreshing_ = false;
}

bool SuperiorAgent::isOurs(fcitx::InputContext *inputContext) const {
    return instance_ != nullptr && inputContext != nullptr &&
           instance_->inputMethod(inputContext) == "super-ime";
}

void SuperiorAgent::broadcastHidden() {
    if (overlay_.clientCount() == 0) {
        return;
    }
    api::UiSceneFrame frame;
    frame.frameId = nextFrame_++;
    frame.session = api::SessionKey{epoch_, 0};
    frame.panel.visible = false;
    frame.status.visible = false;
    overlay_.publish(protocol::encodeScene(frame));
    SUPERIOR_DEBUG(diag::categories::Fcitx5) << "hiding our windows: another input method is active";
}

void SuperiorAgent::broadcast(fcitx::InputContext *inputContext) {
    const std::size_t clients = overlay_.clientCount();
    if (clients == 0) {
        SUPERIOR_TRACE(diag::categories::Fcitx5)
            .field("sessions", static_cast<std::uint64_t>(sessions_.size()))
            << "no UI connected, frame dropped";
        return;
    }
    auto *session = findSession(inputContext);
    if (session == nullptr) {
        return;
    }
    const api::UiSceneFrame frame = session->snapshot(nextFrame_++);
    const std::string payload = protocol::encodeScene(frame);
    SUPERIOR_TRACE(diag::categories::Fcitx5)
        .field("frame", static_cast<std::uint64_t>(frame.frameId))
        .field("clients", static_cast<std::uint64_t>(clients))
        .field("bytes", static_cast<std::uint64_t>(payload.size()))
        .field("visible", frame.panel.visible)
        << "broadcasting scene";
    overlay_.publish(payload);
}

void SuperiorAgent::forget(fcitx::InputContext *inputContext) {
    const auto it = sessions_.find(inputContext);
    if (it == sessions_.end()) {
        return;
    }
    it->second->hide();
    it->second->setStatus({});
    broadcast(inputContext);
    sessions_.erase(it);
    SUPERIOR_DEBUG(diag::categories::Fcitx5)
        .field("sessions", static_cast<std::uint64_t>(sessions_.size()))
        << "session forgotten";
}

void SuperiorAgent::handleCommand(std::string bytes) {
    std::size_t consumed = 0;
    protocol::WireMessage message;
    protocol::DecodeError error;
    if (protocol::tryDecodeFrame(bytes, consumed, message, error) !=
        protocol::FrameStatus::Complete) {
        SUPERIOR_WARN(diag::categories::Fcitx5)
            .field("code", error.code)
            .field("detail", error.detail)
            .field("bytes", static_cast<std::uint64_t>(bytes.size()))
            << "malformed frame from UI";
        return;
    }
    if (message.type != protocol::MessageType::Command) {
        SUPERIOR_DEBUG(diag::categories::Fcitx5)
            .field("type", std::string(protocol::messageTypeName(message.type)))
            << "ignoring non-command message";
        return;
    }
    const auto command = protocol::decodeCommand(message.payload, error);
    if (!command) {
        SUPERIOR_WARN(diag::categories::Fcitx5)
            .field("code", error.code)
            .field("detail", error.detail)
            << "malformed command from UI";
        return;
    }
    applyCommand(*command);
}

void SuperiorAgent::applyCommand(const api::UiCommand &command) {
    // Commands are semantic: the UI never sends geometry or engine internals.
    // Anything not listed here is ignored on purpose, so an older UI cannot
    // corrupt a newer agent.
    const auto handle = [this](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, api::ToggleStatusItem>) {
            if (value.itemId == "speech") {
                speech_.togglePanel();
                return;
            }
            if (value.itemId != "inline-mode") {
                activateStatusItem(value.itemId);
                return;
            }
            if (!inlineEnglishEnabled_) {
                SUPERIOR_DEBUG(diag::categories::Fcitx5)
                    << "status bar toggle ignored: inline English is disabled in settings";
                return;
            }
            setEnglishMode(activeContext_, !modes_.english(), "status bar");
        } else if constexpr (std::is_same_v<T, api::SetInputMode>) {
            if (!inlineEnglishEnabled_) {
                SUPERIOR_DEBUG(diag::categories::Fcitx5)
                    << "set input mode ignored: inline English is disabled in settings";
                return;
            }
            setEnglishMode(activeContext_, value.mode == api::InputMode::English, "ui command");
        } else if constexpr (std::is_same_v<T, api::ActivateCandidate>) {
            activateCandidate(value);
        } else if constexpr (std::is_same_v<T, api::PageCandidates>) {
            pageCandidates(value);
        } else if constexpr (std::is_same_v<T, api::SpeechStart>) {
            if (contextFor(value.session) == nullptr) {
                return;
            }
            if (!speech_.canStart()) {
                SUPERIOR_DEBUG(diag::categories::SpeechRecognition)
                    .field("phase", api::speechPhaseName(speech_.phase()))
                    << "speech start ignored: the capability is not ready";
                return;
            }
            speech_.start();
        } else if constexpr (std::is_same_v<T, api::SpeechStop>) {
            if (contextFor(value.session) == nullptr) {
                return;
            }
            speech_.stop();
        } else if constexpr (std::is_same_v<T, api::SpeechCancel>) {
            speech_.cancel("discard button");
        } else if constexpr (std::is_same_v<T, api::SpeechReload>) {
            // The panel sends this right after it writes `settings.json` (model
            // choice) or finishes a download. Re-reading the file first is not
            // optional: the 500 ms poll has not run yet, so without it this very
            // refresh answers with the *previous* model and the panel's combo
            // snaps back — which looked like "the first click does nothing".
            reloadSettings();
            speech_.refreshSetup();
            // A child that is already running loaded the old model at start.
            speech_.reconfigure();
            refresh(activeContext_);
        } else if constexpr (std::is_same_v<T, api::SetUiScale>) {
            // Scaling is applied by the UI process, which also owns the
            // settings file. Logged so a wrong value is visible in a trace.
            SUPERIOR_DEBUG(diag::categories::Fcitx5)
                .field("scale", value.scale, 2)
                << "ui scale reported by the UI";
        } else {
            SUPERIOR_DEBUG(diag::categories::Fcitx5)
                << "command not handled by the agent";
        }
    };
    std::visit(handle, command);
}

void SuperiorAgent::activateStatusItem(const std::string &itemId) {
    auto *inputContext = activeContext_;
    if (inputContext == nullptr) {
        SUPERIOR_DEBUG(diag::categories::Fcitx5)
            .field("item", itemId)
            << "status item click ignored: no active input context";
        return;
    }

    // The click is a request, not a state change: find the engine's own action
    // and let it decide. That keeps punctuation/fullwidth semantics in the
    // engine instead of reimplementing them here.
    for (auto *action : inputContext->statusArea().allActions()) {
        if (action == nullptr || action->name() != itemId) {
            continue;
        }
        SUPERIOR_INFO(diag::categories::Fcitx5)
            .field("item", itemId)
            .field("program", inputContext->program())
            << "activating status action";
        action->activate(inputContext);
        // The action may have changed engine state that shows up in the panel
        // or in the item's own label/checked state.
        inputContext->updateUserInterface(fcitx::UserInterfaceComponent::StatusArea);
        refresh(inputContext);
        return;
    }

    SUPERIOR_DEBUG(diag::categories::Fcitx5)
        .field("item", itemId)
        << "status item click ignored: no such action in this context";
}

void SuperiorAgent::registerTrayActions() {
    if (trayActionsRegistered_ || instance_ == nullptr) {
        return;
    }

    // Both entries are checkable: the tray menu then draws a mark instead of an
    // icon, and `dbusmenu` turns `isChecked` into `toggle-state`. The labels are
    // what the menu shows, so they name the window, not the verb.
    toolbarAction_.setCheckable(true);
    toolbarAction_.setChecked(showStatusBar_);
    toolbarAction_.setShortText("显示工具栏");
    toolbarAction_.setLongText("显示或隐藏输入法工具栏");
    toolbarAction_.connect<fcitx::SimpleAction::Activated>(
        [this](fcitx::InputContext *) { toggleStatusBar(); });

    speechAction_.setCheckable(true);
    speechAction_.setChecked(speech_.visible());
    speechAction_.setShortText("语音输入");
    speechAction_.setLongText("显示或隐藏语音输入窗口");
    speechAction_.connect<fcitx::SimpleAction::Activated>([this](fcitx::InputContext *) {
        speech_.togglePanel();
        // The panel just opened or closed. The mark has to move in the same
        // activation, otherwise the menu the user is still looking at is wrong.
        refreshTrayActions();
    });

    const bool toolbarRegistered =
        instance_->userInterfaceManager().registerAction(std::string(kToolbarToggleAction),
                                                         &toolbarAction_);
    const bool speechRegistered =
        instance_->userInterfaceManager().registerAction(std::string(kSpeechToggleAction),
                                                         &speechAction_);
    if (!toolbarRegistered || !speechRegistered) {
        // Half a pair is worse than none: one menu entry that works and one that
        // does nothing. Leave the status area untouched.
        if (toolbarRegistered) {
            instance_->userInterfaceManager().unregisterAction(&toolbarAction_);
        }
        if (speechRegistered) {
            instance_->userInterfaceManager().unregisterAction(&speechAction_);
        }
        SUPERIOR_WARN(diag::categories::Fcitx5)
            .field("toolbar", std::string(kToolbarToggleAction))
            .field("speech", std::string(kSpeechToggleAction))
            << "cannot publish the overlay switches to the tray menu";
        return;
    }

    trayActionsRegistered_ = true;
    SUPERIOR_INFO(diag::categories::Fcitx5)
        .field("toolbar", std::string(kToolbarToggleAction))
        .field("speech", std::string(kSpeechToggleAction))
        << "overlay switches published as status actions";
}

void SuperiorAgent::attachTrayActions(fcitx::InputContext *inputContext) {
    if (!trayActionsRegistered_ || inputContext == nullptr) {
        return;
    }
    auto &statusArea = inputContext->statusArea();
    const auto present = [&statusArea](const fcitx::Action *action) {
        for (auto *candidate : statusArea.allActions()) {
            if (candidate == action) {
                return true;
            }
        }
        return false;
    };
    // Only our own contexts get these: the menu lists the *focused* context's
    // actions, and another input method's tray menu is not our business.
    bool changed = false;
    if (!present(&toolbarAction_)) {
        statusArea.addAction(fcitx::StatusGroup::InputMethod, &toolbarAction_);
        changed = true;
    }
    // The microphone is a product switch (`settings.json: speechRecognitionEnabled`).
    // An entry that opens a panel the user switched off would promise a feature
    // that is not there, so it follows the same gate the toolbar uses.
    const bool speechWanted = speech_.enabled();
    if (speechWanted && !present(&speechAction_)) {
        statusArea.addAction(fcitx::StatusGroup::InputMethod, &speechAction_);
        changed = true;
    } else if (!speechWanted && present(&speechAction_)) {
        statusArea.removeAction(&speechAction_);
        changed = true;
    }
    if (changed) {
        SUPERIOR_DEBUG(diag::categories::Fcitx5)
            .field("program", inputContext->program())
            .field("toolbar", std::string(kToolbarToggleAction))
            .field("speech", std::string(kSpeechToggleAction))
            .field("speechEnabled", speechWanted)
            << "overlay switches attached to the status area";
    }
}

void SuperiorAgent::refreshTrayActions() {
    if (!trayActionsRegistered_) {
        return;
    }
    // Membership first (a product switch may have turned the microphone off),
    // then the check marks.
    if (activeContext_ != nullptr) {
        attachTrayActions(activeContext_);
    }
    const bool toolbar = showStatusBar_;
    const bool speech = speech_.visible();
    const bool changed = toolbar != trayToolbarChecked_ || speech != traySpeechChecked_;
    trayToolbarChecked_ = toolbar;
    traySpeechChecked_ = speech;
    toolbarAction_.setChecked(toolbar);
    speechAction_.setChecked(speech);
    if (!changed) {
        return;
    }
    // `notificationitem` rebuilds the tray menu on a StatusArea flush and reads
    // `isChecked` at that moment. Without this the mark keeps the old value
    // until something else happens to flush.
    if (activeContext_ != nullptr) {
        activeContext_->updateUserInterface(fcitx::UserInterfaceComponent::StatusArea);
    }
}

void SuperiorAgent::toggleStatusBar() {
    const bool visible = !showStatusBar_;
    // The settings file is the owner of this switch, so the overlay's existing
    // poll picks the change up without a second IPC surface. A failed write is
    // still applied: a menu entry that does nothing is worse than one that
    // forgets across a restart.
    const std::filesystem::path path = config::defaultSettingsPath();
    config::Settings settings = config::loadSettings(path).settings;
    settings.showStatusBar = visible;
    std::string error;
    if (!config::saveSettings(path, settings, error)) {
        SUPERIOR_WARN(diag::categories::Fcitx5)
            .field("path", path.string())
            .field("error", error)
            << "cannot persist the status bar visibility";
    }
    showStatusBar_ = visible;
    SUPERIOR_INFO(diag::categories::Fcitx5)
        .field("visible", visible)
        << "status bar visibility toggled from the tray menu";
    refreshTrayActions();
}

void SuperiorAgent::syncCloudCandidate(fcitx::InputContext *inputContext) {
    auto &state = cloudContexts_[inputContext];

    // English mode commits letters directly; there is no pinyin to complete.
    // Password fields are the one gate that also disables the *local* history:
    // remembering that someone typed a password would be worse than losing a
    // preference.
    if (modes_.english() ||
        inputContext->capabilityFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive)) {
        state = CloudContext{};
        clearPendingCloud(inputContext);
        return;
    }

    const auto candidateList = inputContext->inputPanel().candidateList();
    if (!candidateList) {
        state = CloudContext{};
        return;
    }
    auto *modifiable = candidateList->toModifiable();
    if (modifiable == nullptr) {
        // An engine with a read-only list simply does not get cloud candidates.
        SUPERIOR_TRACE(diag::categories::Cloud)
            .field("reason", "list-not-modifiable")
            .field("im", instance_ != nullptr ? instance_->inputMethod(inputContext)
                                              : std::string{})
            << "cloud candidate skipped";
        state = CloudContext{};
        return;
    }

    const auto normalized =
        cloudpinyin::normalizePinyin(inputContext->inputPanel().preedit().toString());
    if (!normalized) {
        SUPERIOR_TRACE(diag::categories::Cloud)
            .field("reason", "not-pinyin")
            .redacted("preedit", inputContext->inputPanel().preedit().toString())
            << "cloud candidate skipped";
        state = CloudContext{};
        clearPendingCloud(inputContext);
        return;
    }

    // An engine may rebuild the list contents in place or hand us a new list
    // object when the candidates change, so the pinyin — not the pointer — is
    // what identifies a composition.
    if (state.pinyin != *normalized || state.list != candidateList.get()) {
        state = CloudContext{};
        state.list = candidateList.get();
        state.pinyin = *normalized;
    }

    const auto containsText = [modifiable](const std::string &text) {
        if (text.empty()) {
            return false;
        }
        const int total = modifiable->totalSize();
        for (int index = 0; index < total; ++index) {
            if (modifiable->candidateFromAll(index).text().toString() == text) {
                return true;
            }
        }
        return false;
    };
    const auto slotHolds = [modifiable](int index, const std::string &text) {
        return index >= 0 && index < modifiable->totalSize() &&
               modifiable->candidateFromAll(index).text().toString() == text;
    };

    // ---------------------------------------------------------------- local
    //
    // A word this user already picked for this pinyin goes to the very front:
    // that is what "the second time 原神 comes first" means. This part runs even
    // with cloud pinyin switched off — it is local history, not a network
    // feature.
    if (state.learnedIndex >= 0 && !slotHolds(state.learnedIndex, state.learnedText)) {
        // The engine rebuilt the list and our row is gone.
        state.learnedIndex = -1;
        state.learnedText.clear();
    }
    if (state.learnedIndex < 0) {
        if (const auto preferred = config::preferredWord(learned_, state.pinyin);
            preferred && !containsText(*preferred)) {
            modifiable->insert(
                0, std::make_unique<CloudCandidateWord>(
                       fcitx::Text(*preferred), *preferred,
                       [this](fcitx::InputContext *context, std::string_view text) {
                           commitCloudText(context, text);
                       }));
            state.learnedIndex = 0;
            state.learnedText = *preferred;
            SUPERIOR_TRACE(diag::categories::Cloud)
                .field("index", static_cast<std::int64_t>(0))
                .field("length", static_cast<std::int64_t>(preferred->size()))
                << "learned word promoted to the front";
        }
    }

    // ---------------------------------------------------------------- cloud
    if (!cloudEnabled_) {
        return;
    }
    if (state.insertIndex >= 0 && !slotHolds(state.insertIndex, state.text)) {
        // The engine rebuilt the list and our row is gone; a later frame for the
        // same pinyin would otherwise return early and never re-insert.
        state.insertIndex = -1;
        state.text.clear();
        state.queried = false;
    }
    if (state.insertIndex >= 0) {
        return; // already spliced in and still where we put it
    }
    if (!cloudpinyin::worthQuerying(state.pinyin, cloudMinLength_)) {
        SUPERIOR_TRACE(diag::categories::Cloud)
            .field("reason", "too-short")
            .field("length", static_cast<std::int64_t>(state.pinyin.size()))
            .field("minimum", static_cast<std::int64_t>(cloudMinLength_))
            << "cloud candidate skipped";
        clearPendingCloud(inputContext);
        return;
    }

    // If the upstream cloudpinyin addon is installed and already showing its
    // placeholder, it owns cloud candidates for this context. Two providers
    // would mean two cloud rows.
    const int total = modifiable->totalSize();
    for (int index = 0; index < total; ++index) {
        if (modifiable->candidateFromAll(index).text().toString() == kCloudPlaceholder) {
            SUPERIOR_TRACE(diag::categories::Cloud)
                .field("reason", "upstream-placeholder")
                << "cloud candidate skipped";
            return;
        }
    }

    if (const auto cached = cloudCache_.find(state.pinyin)) {
        if (!containsText(*cached)) {
            // A cached answer is instant: insert it now, at the configured slot.
            const int offset = state.learnedIndex >= 0 ? 1 : 0;
            const int insertAt = std::clamp(cloudIndex_ - 1 + offset, 0, modifiable->totalSize());
            modifiable->insert(
                insertAt, std::make_unique<CloudCandidateWord>(
                              fcitx::Text(*cached), *cached,
                              [this](fcitx::InputContext *context, std::string_view text) {
                                  commitCloudText(context, text);
                              }));
            state.insertIndex = insertAt;
            state.text = *cached;
            state.queried = true;
            SUPERIOR_TRACE(diag::categories::Cloud)
                .field("index", static_cast<std::int64_t>(insertAt))
                .field("length", static_cast<std::int64_t>(state.pinyin.size()))
                << "cloud candidate served from cache";
        }
        return;
    }

    if (state.queried) {
        return; // already asking; the answer inserts itself when it arrives
    }
    state.queried = true;

    // **No placeholder.** Reserving a slot while the request is in flight made
    // the list jump: the row you were about to press (2) was a cloud that said
    // nothing yet, and everything behind it shifted by one. The candidate now
    // appears only when there is a word to show — until then the list is exactly
    // what the engine produced.
    cloudPendingContext_ = inputContext;
    cloudPendingPinyin_ = state.pinyin;
    cloudPendingSince_ = std::chrono::steady_clock::now();
    updateCloudTimer();
}

void SuperiorAgent::clearPendingCloud(fcitx::InputContext *inputContext) {
    if (cloudPendingContext_ == inputContext) {
        cloudPendingContext_ = nullptr;
        cloudPendingPinyin_.clear();
    }
}

void SuperiorAgent::updateCloudTimer() {
    if (instance_ == nullptr) {
        return;
    }
    const bool needed = !cloudPendingPinyin_.empty() || cloudFetcher_.inFlight();
    if (!needed) {
        if (cloudTimer_) {
            cloudTimer_->setEnabled(false);
        }
        return;
    }
    if (cloudTimer_) {
        // The callback re-arms the one-shot source itself. Do not touch it from
        // inside that callback: `isEnabled()` can report false while the source
        // is being dispatched, and re-enabling it there used to leave the
        // deadline in the past. sd-event then called `pumpCloud()` in a tight
        // loop which starved XCB/XIM input (Telegram keys arrived in ~200 ms
        // batches while a cloud request was running).
        if (!cloudPumping_ && !cloudTimer_->isEnabled()) {
            cloudTimer_->setNextInterval(kCloudTickUs);
            cloudTimer_->setEnabled(true);
        }
        return;
    }
    cloudTimer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + kCloudTickUs, 0,
        [this](fcitx::EventSourceTime *source, std::uint64_t) {
            // A time source is one-shot: without moving the deadline forward,
            // sd-event keeps it enabled in the past and calls this callback in
            // a tight loop. That starved XCB/XIM input, so Telegram keys were
            // only delivered in ~200 ms batches while a request was pending.
            source->setNextInterval(kCloudTickUs);
            cloudPumping_ = true;
            pumpCloud();
            cloudPumping_ = false;
            return true;
        });
    cloudTimer_->setEnabled(false);
    if (needed) {
        cloudTimer_->setEnabled(true);
    }
}

void SuperiorAgent::pumpCloud() {
    const auto now = std::chrono::steady_clock::now();
    const auto nowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    for (const auto &result : cloudFetcher_.poll()) {
        if (result.ok && !result.text.empty()) {
            cloudBudget_.recordSuccess();
            cloudCache_.insert(result.pinyin, result.text);
        } else if (result.ok) {
            // Reached the provider, it simply had nothing: not a failure, so do
            // not count it against the budget.
            cloudBudget_.recordSuccess();
        } else {
            // A dead network will not recover on the next keystroke, so it counts
            // three times and parks the provider with one try.
            cloudBudget_.recordFailure(nowMs, result.networkError ? 3 : 1);
        }

        for (auto &[context, state] : cloudContexts_) {
            if (!state.queried || state.pinyin != result.pinyin) {
                continue;
            }
            if (result.ok && !result.text.empty()) {
                applyCloudResult(context, result.pinyin, result.text);
            } else {
                dropCloudCandidate(context);
            }
        }
    }

    if (!cloudPendingPinyin_.empty() && now - cloudPendingSince_ >= kCloudDebounce) {
        if (!cloudBudget_.allowed(nowMs)) {
            // Parked: drop the request instead of retrying every tick. The
            // budget re-opens after its cooldown.
            cloudPendingPinyin_.clear();
            cloudPendingContext_ = nullptr;
            SUPERIOR_DEBUG(diag::categories::Cloud)
                .field("errors", static_cast<std::int64_t>(cloudBudget_.consecutiveErrors()))
                << "cloud pinyin parked after repeated failures";
        } else if (!cloudFetcher_.inFlight()) {
            const std::string pinyin = cloudPendingPinyin_;
            cloudPendingPinyin_.clear();
            cloudPendingContext_ = nullptr;
            cloudFetcher_.submit(pinyin, cloudBackend_);
        }
        // Otherwise a transfer is still running. Keep the request: clearing it
        // here lost the query entirely, so the newest composition never reached
        // the provider and its placeholder stayed empty forever.
    }

    updateCloudTimer();
}

void SuperiorAgent::applyCloudResult(fcitx::InputContext *inputContext,
                                     const std::string &pinyin, const std::string &text) {
    const auto it = cloudContexts_.find(inputContext);
    if (it == cloudContexts_.end()) {
        return;
    }
    CloudContext &state = it->second;
    if (state.pinyin != pinyin) {
        return;
    }

    const auto candidateList = inputContext->inputPanel().candidateList();
    if (!candidateList || state.list != candidateList.get()) {
        // The user typed on. The answer is cached already, so the next frame for
        // this pinyin picks it up without another request.
        state = CloudContext{};
        return;
    }
    auto *modifiable = candidateList->toModifiable();
    if (modifiable == nullptr) {
        state = CloudContext{};
        return;
    }

    const int total = modifiable->totalSize();
    // Nothing was reserved while loading, so a word the engine produced in the
    // meantime must not be duplicated.
    for (int index = 0; index < total; ++index) {
        if (modifiable->candidateFromAll(index).text().toString() == text) {
            SUPERIOR_DEBUG(diag::categories::Cloud)
                << "cloud candidate dropped: engine already offers this word";
            state = CloudContext{};
            return;
        }
    }

    // Insert at the configured slot. The learned word (if any) already sits at
    // index 0, so the engine's own order has to be offset by one.
    const int offset = state.learnedIndex >= 0 ? 1 : 0;
    const int insertAt = std::clamp(cloudIndex_ - 1 + offset, 0, total);
    modifiable->insert(
        insertAt, std::make_unique<CloudCandidateWord>(
                      fcitx::Text(text), text,
                      [this](fcitx::InputContext *context, std::string_view value) {
                          commitCloudText(context, value);
                      }));
    state.insertIndex = insertAt;
    state.text = text;
    SUPERIOR_DEBUG(diag::categories::Cloud)
        .field("index", static_cast<std::int64_t>(insertAt))
        .field("length", static_cast<std::int64_t>(text.size()))
        << "cloud candidate inserted";
    // A cloud answer arrives outside any key event. Calling the host's
    // `updateUserInterface()` here makes the platform (XIM clients in
    // particular) process an asynchronous panel update interleaved with key
    // dispatch. Our overlay draws from the scene, so refresh that directly and
    // leave the platform UI untouched. A result for a background context stays
    // in its panel and is shown when it becomes active again; refreshing it here
    // would switch the overlay to the wrong window.
    if (inputContext == activeContext_) {
        refresh(inputContext);
    }
}

void SuperiorAgent::dropCloudCandidate(fcitx::InputContext *inputContext) {
    const auto it = cloudContexts_.find(inputContext);
    if (it == cloudContexts_.end()) {
        return;
    }
    CloudContext &state = it->second;

    // Nothing is reserved while a request is in flight, so normally there is
    // nothing to remove here — this only fires when an answer that had already
    // been inserted has to go, which happens if the engine later produces the
    // same word itself.
    if (state.insertIndex >= 0) {
        const auto candidateList = inputContext->inputPanel().candidateList();
        if (candidateList && state.list == candidateList.get()) {
            if (auto *modifiable = candidateList->toModifiable();
                modifiable != nullptr && state.insertIndex < modifiable->totalSize() &&
                modifiable->candidateFromAll(state.insertIndex).text().toString() ==
                    state.text) {
                modifiable->remove(state.insertIndex);
                if (inputContext == activeContext_) {
                    refresh(inputContext);
                }
            }
        }
    }
    state = CloudContext{};
}

void SuperiorAgent::commitCloudText(fcitx::InputContext *inputContext, std::string_view text) {
    if (instance_ == nullptr || inputContext == nullptr || text.empty()) {
        return;
    }
    // Read the pinyin *before* touching the composition. `updatePreedit()` and
    // `commitString()` flush the UI, which re-enters `syncCloudCandidate()` with
    // an empty preedit and clears our state — reading afterwards returned an
    // empty string, so nothing was ever learned.
    const std::string pinyin = [&]() -> std::string {
        const auto it = cloudContexts_.find(inputContext);
        return it != cloudContexts_.end() ? it->second.pinyin : std::string{};
    }();

    // `Drop`: a candidate belongs to one composition. If focus moved since it
    // was drawn, typing it into the new window would be wrong.
    const core::ImeSession *session = findSession(inputContext);
    const api::SessionKey key = session != nullptr ? session->key() : api::SessionKey{};
    if (!outbox_.commit(key, std::string(text), core::WhenUnfocused::Drop)) {
        SUPERIOR_DEBUG(diag::categories::Cloud)
            .field("length", static_cast<std::int64_t>(text.size()))
            << "cloud candidate dropped: its session is no longer active";
        return;
    }

    // Remember the pick, so the next `yuanshen` offers 原神 first without a
    // request. The engine cannot learn this one: we committed it, not rime.
    if (!pinyin.empty()) {
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        config::recordLearnedWord(learned_, pinyin, std::string(text), nowMs);
        std::string error;
        if (!config::saveLearning(config::defaultLearningPath(), learned_, error)) {
            SUPERIOR_WARN(diag::categories::Cloud)
                .field("error", error)
                << "cannot persist the learning file";
        } else {
            SUPERIOR_DEBUG(diag::categories::Cloud)
                .field("entries", static_cast<std::int64_t>(learned_.size()))
                .field("length", static_cast<std::int64_t>(text.size()))
                << "learning updated";
        }
    }

    if (const auto it = cloudContexts_.find(inputContext); it != cloudContexts_.end()) {
        it->second = CloudContext{};
    }
    SUPERIOR_INFO(diag::categories::Cloud)
        .field("length", static_cast<std::int64_t>(text.size()))
        << "cloud candidate committed";
    refresh(inputContext);
}

void SuperiorAgent::activateCandidate(const api::ActivateCandidate &command) {
    auto *inputContext = contextFor(command.session);
    if (inputContext == nullptr) {
        SUPERIOR_DEBUG(diag::categories::Fcitx5)
            .field("candidate", static_cast<std::int64_t>(command.candidate))
            << "candidate activation ignored: session is no longer active";
        return;
    }

    const auto candidateList = inputContext->inputPanel().candidateList();
    if (!candidateList) {
        // The list changed between painting and clicking. Dropping the click is
        // correct: guessing would commit a word the user never saw.
        SUPERIOR_DEBUG(diag::categories::Fcitx5)
            .field("candidate", static_cast<std::int64_t>(command.candidate))
            << "candidate activation ignored: no candidate list";
        return;
    }

    // The id encodes the list generation it was painted from. If the list has
    // been rebuilt since, the click refers to a word the user never saw and is
    // dropped instead of selecting whatever now sits at that position.
    auto *session = findSession(inputContext);
    const auto position = session != nullptr
                              ? session->candidates().positionOf(command.candidate)
                              : std::nullopt;
    if (!position || *position >= static_cast<std::size_t>(candidateList->size())) {
        SUPERIOR_DEBUG(diag::categories::Fcitx5)
            .field("candidate", static_cast<std::int64_t>(command.candidate))
            .field("size", static_cast<std::int64_t>(candidateList->size()))
            << "candidate activation ignored: stale or out-of-range id";
        return;
    }

    SUPERIOR_DEBUG(diag::categories::Fcitx5)
        .field("candidate", static_cast<std::int64_t>(command.candidate))
        .field("index", static_cast<std::int64_t>(*position))
        .field("program", inputContext->program())
        << "candidate activated from the UI";
    candidateList->candidate(static_cast<int>(*position)).select(inputContext);
    refresh(inputContext);
}

void SuperiorAgent::pageCandidates(const api::PageCandidates &command) {
    auto *inputContext = contextFor(command.session);
    if (inputContext == nullptr) {
        return;
    }
    const auto candidateList = inputContext->inputPanel().candidateList();
    if (!candidateList) {
        return;
    }
    auto *pageable = candidateList->toPageable();
    if (pageable == nullptr) {
        // A single-page list: scrolling must do nothing, not wrap silently.
        return;
    }

    // One wheel notch is one page, matching every other paging key
    // (Tab/Shift+Tab, +/-) rather than scrolling a single candidate.
    bool moved = false;
    if (command.delta > 0) {
        if (pageable->hasNext()) {
            pageable->next();
            moved = true;
        }
    } else if (command.delta < 0) {
        if (pageable->hasPrev()) {
            pageable->prev();
            moved = true;
        }
    }
    SUPERIOR_DEBUG(diag::categories::Fcitx5)
        .field("delta", static_cast<std::int64_t>(command.delta))
        .field("moved", moved)
        .field("program", inputContext->program())
        << "candidate page requested";
    if (moved) {
        refresh(inputContext);
    }
}

fcitx::InputContext *SuperiorAgent::contextFor(const api::SessionKey &session) {
    // Only the session the UI is currently being fed for may act: a click on a
    // window that already lost focus must not type into the new one.
    if (activeContext_ == nullptr || session.epoch != epoch_) {
        return nullptr;
    }
    const auto it = sessions_.find(activeContext_);
    if (it == sessions_.end() || session.context != it->second->key().context) {
        return nullptr;
    }
    return activeContext_;
}

} // namespace superior::fcitx5

namespace {

class SuperiorAgentFactory : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new superior::fcitx5::SuperiorAgent(manager);
    }
};

} // namespace

FCITX_ADDON_FACTORY_V2(superioragent, SuperiorAgentFactory);
