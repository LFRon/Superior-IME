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
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "superior/diagnostics/Log.hpp"
#include "superior/config/Settings.hpp"
#include "superior/core/KeyPolicy.hpp"
#include "superior/speech_recognition/Layout.hpp"
#include "superior/speech_recognition/Models.hpp"
#include "superior/speech_recognition/Protocol.hpp"
#include "superior/fcitx5/InlineText.hpp"
#include "superior/fcitx5/PreeditSource.hpp"
#include "superior/fcitx5/ShiftTap.hpp"
#include "superior/fcitx5/StatusMapping.hpp"
#include "superior/protocol/Wire.hpp"

#include <nlohmann/json.hpp>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace superior::fcitx5 {

namespace diag = ::superior::diagnostics;

namespace {

/// Commands without a payload are one-liners; going through `encodeCommand`
/// keeps their spelling in the protocol header rather than in this file.
std::string encodeSpeechCommand(superior::speech_recognition::Command::Kind kind) {
    superior::speech_recognition::Command command;
    command.kind = kind;
    return superior::speech_recognition::encodeCommand(command);
}

std::size_t byteCursor(const fcitx::Text &text) {
    const int cursor = text.cursor();
    if (cursor < 0) {
        return text.toString().size();
    }
    return static_cast<std::size_t>(cursor);
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
    : manager_(manager), instance_(manager != nullptr ? manager->instance() : nullptr) {
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

    std::string error;
    const std::string socketPath = protocol::SceneServer::defaultSocketPath();
    if (!startSceneServer(socketPath, error)) {
        // A failed bind used to be permanent, which is how a Fcitx5 handover
        // could leave the overlay dead for the rest of the session. Keep trying:
        // the other instance is usually on its way out.
        SUPERIOR_WARN(diag::categories::Fcitx5)
            .field("path", socketPath)
            .field("error", error)
            << "cannot listen for the UI yet; retrying";
        socketRetryTimer_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 1000000, 0,
            [this, socketPath](fcitx::EventSourceTime *, std::uint64_t) {
                std::string retryError;
                if (startSceneServer(socketPath, retryError)) {
                    ensureOverlay();
                } else {
                    SUPERIOR_DEBUG(diag::categories::Fcitx5)
                        .field("error", retryError)
                        << "UI socket still busy";
                }
                return !socketRetryStop_; // keep polling once a second
            });
    } else {
        SUPERIOR_INFO(diag::categories::Fcitx5)
            .field("path", socketPath)
            << "UI socket ready";
    }
    server_.setCommandHandler([this](std::string bytes) { handleCommand(std::move(bytes)); });

    handlers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextFlushUI, fcitx::EventWatcherPhase::PostInputMethod,
        [this](fcitx::Event &event) {
            auto &flush = static_cast<fcitx::InputContextFlushUIEvent &>(event);
            if (flush.component() == fcitx::UserInterfaceComponent::InputPanel) {
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
            ensureOverlay();
            if (!isOurs(inputContext)) {
                if (activeContext_ != nullptr) {
                    activeContext_ = nullptr;
                    broadcastHidden();
                }
                return;
            }
            refresh(inputContext);
            flushHeldSpeech();
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
            holdSpeechCommits();
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
    ensureOverlay();

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
            pollSpeechProcess();
            // The socket can vanish while the user stays in one window: an
            // Fcitx5 handover, a stalled agent, a cleaned-up runtime directory.
            // Focus events are the only other thing that rebuilds it, and
            // waiting for one left the panels dead until the user happened to
            // switch windows. One lstat every half second is the whole cost.
            if (!server_.ownsSocketFile()) {
                ensureOverlay();
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
    if (speechProcess_) {
        speechProcess_->stop();
    }
    settingsTimer_.reset();
    startupTimer_.reset();
    cloudTimer_.reset();
    // Drop sockets before libcurl's handles go away: a transfer outliving the
    // event loop would be polled by nobody and leak its socket.
    cloudFetcher_.cancelAll();
    reaperStop_.store(true, std::memory_order_relaxed);
    for (auto &reaper : reapers_) {
        if (reaper.joinable()) {
            reaper.join();
        }
    }
    SUPERIOR_INFO(diag::categories::Fcitx5)
        .field("sessions", static_cast<std::uint64_t>(sessions_.size()))
        << "superior agent stopping";
    handlers_.clear();
    server_.close();
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
    if (speechPreeditActive_) {
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
    if (speechRecognitionEnabled_) {
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
    const bool previousSpeech = speechRecognitionEnabled_;
    speechRecognitionEnabled_ = loaded.settings.speechRecognitionEnabled;
    speechRecognitionHotkey_ = loaded.settings.speechRecognitionHotkey;
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
    speechModelId_ = choice.id;
    if (previousSpeech != speechRecognitionEnabled_) {
        SUPERIOR_INFO(diag::categories::SpeechRecognition)
            .field("enabled", speechRecognitionEnabled_)
            << "speech recognition toolbar entry changed";
        if (!speechRecognitionEnabled_) {
            speech_.hide();
        }
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
        // XKB's compose state and leaves the engine composition alive.
        inputContext->reset();
        inputContext->inputPanel().reset();
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

bool SuperiorAgent::reclaimStaleOverlay() {
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime == nullptr || runtime[0] == '\0') {
        return false;
    }
    const std::string lockPath = std::string(runtime) + "/superior-ime-ui.lock";
    std::ifstream file(lockPath);
    if (!file) {
        return false;
    }
    // QLockFile's format: pid on the first line, then host and application name.
    long pid = 0;
    file >> pid;
    if (pid <= 0 || pid == static_cast<long>(::getpid())) {
        return false;
    }
    // Only signal a process that is really our overlay: a recycled PID must not
    // take a stray SIGTERM, and this file is user-writable.
    std::ifstream cmdline("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    std::string command;
    if (cmdline) {
        std::getline(cmdline, command, '\0');
    }
    if (command.find("superior_ui") == std::string::npos) {
        return false;
    }
    if (::kill(static_cast<pid_t>(pid), SIGTERM) != 0) {
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return true;
}

void SuperiorAgent::ensureOverlay() {
    // The overlay can only be reached through our socket. Rebuild it first when
    // it is gone: another Fcitx5 replacing this one (`fcitx5 -r`) can remove the
    // file, and respawning the UI against a dead socket just produced the
    // "started the Superior overlay" / "stopped a stale overlay" loop with no
    // panel in between.
    if (!server_.ownsSocketFile()) {
        std::string error;
        if (!startSceneServer(protocol::SceneServer::defaultSocketPath(), error)) {
            // Someone else owns the UI right now. Their agent spawns the
            // overlay; taking it over here would fight it for the single
            // instance lock on every focus change.
            SUPERIOR_DEBUG(diag::categories::Fcitx5)
                .field("error", error)
                << "not touching the overlay while another agent owns the UI socket";
            return;
        }
    }

    if (server_.clientCount() > 0) {
        overlaySpawned_ = true;
        return;
    }

    if (const char *disabled = std::getenv("SUPERIOR_UI_AUTOSTART");
        disabled != nullptr && std::string_view(disabled) == "0") {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (overlaySpawned_ && now - lastOverlayAttempt_ < std::chrono::seconds(3)) {
        return;
    }
    // We already have a socket but nobody is connected: either the overlay just
    // started, or a *previous* one is wedged and still holds the single-instance
    // lock — then every respawn prints "already running" and we never get a
    // window again. Reclaim by stopping the holder, which is safe: the lock file
    // lives in our own runtime directory and records the process that took it.
    if (overlaySpawned_ && reclaimStaleOverlay()) {
        SUPERIOR_WARN(diag::categories::Fcitx5)
            << "stopped a stale overlay that still held the single-instance lock";
    }
    lastOverlayAttempt_ = now;

    // Order matters: the per-user install keeps the executable under
    // ~/.local/share/superior-ime/bin (only a tiny wrapper sits in
    // ~/.local/bin), a system install puts it in /usr/bin, and the PATH lookup
    // is the last resort for hand-built trees.
    std::vector<std::string> candidates;
    if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        candidates.push_back(std::string(home) + "/.local/share/superior-ime/bin/superior_ui");
        candidates.push_back(std::string(home) + "/.local/bin/superior_ui");
    }
    candidates.emplace_back("/usr/bin/superior_ui");
    candidates.emplace_back("superior_ui");

    for (const auto &binary : candidates) {
        pid_t pid = -1;
        std::string program = binary;
        std::vector<char *> argv{program.data(), nullptr};
        if (::posix_spawnp(&pid, program.c_str(), nullptr, nullptr, argv.data(), environ) == 0) {
            overlaySpawned_ = true;
            // Reap the child so a crashed overlay does not stay a zombie for the
            // lifetime of Fcitx5.
            //
            // The reaper is joinable, not detached: Fcitx5 may unload this addon,
            // and a detached thread executing code inside a dlclose'd library
            // would crash. The destructor stops and joins it first.
            reapers_.emplace_back([this, pid] {
                while (!reaperStop_.load(std::memory_order_relaxed)) {
                    int status = 0;
                    const pid_t result = ::waitpid(pid, &status, WNOHANG);
                    if (result != 0) {
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            });
            SUPERIOR_INFO(diag::categories::Fcitx5)
                .field("binary", binary)
                << "started the Superior overlay";
            return;
        }
    }
    SUPERIOR_WARN(diag::categories::Fcitx5)
        << "cannot start the overlay; run superior_ui manually or set SUPERIOR_UI_AUTOSTART=0";
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
    const bool hasUi = server_.clientCount() > 0;
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
            if (inlineEnglishEnabled_) {
                setEnglishMode(inputContext, !modes_.english(), "shift tap");
            } else {
                SUPERIOR_DEBUG(diag::categories::Fcitx5)
                    .field("program", inputContext->program())
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
            if (ctrlTap.released(now)) {
                toggleSpeechByHotkey();
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
    if (!release && sym == FcitxKey_Escape && speech_.busy()) {
        cancelSpeech("escape");
        event.filterAndAccept();
        return true;
    }

    // Who owns this key is a product decision, so it lives in the decision
    // layer (`core::decideKey`) instead of here. This adapter only reports the
    // facts the policy cannot know: whether a composition is active and whether
    // the key produces text in the current layout.
    const auto &panel = inputContext->inputPanel();
    const bool composing = !panel.preedit().toString().empty() ||
                           !panel.clientPreedit().toString().empty() ||
                           panel.candidateList() != nullptr;

    const auto states = event.key().states();
    const auto inlineText = inlineTextFor(static_cast<std::uint32_t>(sym),
                                          states.test(fcitx::KeyState::Shift));

    const core::KeyDecision decision = core::decideKey(core::KeyPolicyInput{
        .key = core::KeyPress{.sym = static_cast<std::uint32_t>(sym),
                              .shift = states.test(fcitx::KeyState::Shift),
                              .control = states.test(fcitx::KeyState::Ctrl),
                              .alt = states.test(fcitx::KeyState::Alt),
                              .super = states.test(fcitx::KeyState::Super),
                              .release = release},
        .composing = composing,
        .english = modes_.english(),
        .printable = inlineText.has_value() && !inlineText->empty(),
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
            broadcastHidden();
        }
        return;
    }
    activeContext_ = inputContext;

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

    if (server_.clientCount() > 0) {
        broadcast(inputContext);
    } else {
        // Nothing to draw with: hand the panel back to Fcitx5 instead of
        // leaving the user without candidates.
        inputContext->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    }
    refreshing_ = false;
}

bool SuperiorAgent::isOurs(fcitx::InputContext *inputContext) const {
    return instance_ != nullptr && inputContext != nullptr &&
           instance_->inputMethod(inputContext) == "super-ime";
}

void SuperiorAgent::broadcastHidden() {
    if (server_.clientCount() == 0) {
        return;
    }
    api::UiSceneFrame frame;
    frame.frameId = nextFrame_++;
    frame.session = api::SessionKey{epoch_, 0};
    frame.panel.visible = false;
    frame.status.visible = false;
    server_.broadcast(protocol::encodeFrame(protocol::MessageType::Scene,
                                            protocol::encodeScene(frame)));
    SUPERIOR_DEBUG(diag::categories::Fcitx5) << "hiding our windows: another input method is active";
}

void SuperiorAgent::broadcast(fcitx::InputContext *inputContext) {
    const std::size_t clients = server_.clientCount();
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
    server_.broadcast(protocol::encodeFrame(protocol::MessageType::Scene, payload));
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
                toggleSpeechPanel();
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
            startSpeech();
        } else if constexpr (std::is_same_v<T, api::SpeechStop>) {
            if (contextFor(value.session) == nullptr) {
                return;
            }
            stopSpeech();
        } else if constexpr (std::is_same_v<T, api::SpeechCancel>) {
            cancelSpeech("discard button");
        } else if constexpr (std::is_same_v<T, api::SpeechReload>) {
            // The panel sends this right after it writes `settings.json` (model
            // choice) or finishes a download. Re-reading the file first is not
            // optional: the 500 ms poll has not run yet, so without it this very
            // refresh answers with the *previous* model and the panel's combo
            // snaps back — which looked like "the first click does nothing".
            reloadSettings();
            refreshSpeechSetup();
            // A child that is already running loaded the old model at start.
            configureSpeechProcess();
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
        cloudTimer_->setEnabled(true);
        return;
    }
    cloudTimer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + kCloudTickUs, kCloudTickUs,
        [this](fcitx::EventSourceTime *, std::uint64_t) {
            pumpCloud();
            return true;
        });
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
    inputContext->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
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
                inputContext->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
            }
        }
    }
    state = CloudContext{};
}

namespace {

/// The catalogue entry the user selected, falling back to the recommended one so
/// a hand-edited settings file cannot leave voice permanently unusable.
const superior::speech_recognition::ModelEntry &selectedModel(const std::string &id) {
    if (const auto *entry = superior::speech_recognition::findModel(id)) {
        return *entry;
    }
    return superior::speech_recognition::defaultModel();
}

} // namespace

bool SuperiorAgent::startSceneServer(const std::string &socketPath, std::string &error) {
    // Ask the server itself, never a cached flag: a flag stays true after the
    // socket file has been removed or taken over, and then this returned success
    // for the rest of the session while every panel waited forever for a frame
    // that could not arrive.
    if (server_.ownsSocketFile() && server_.socketPath() == socketPath) {
        return true;
    }
    if (!server_.listen(socketPath, error)) {
        return false;
    }
    // The timer keeps rescheduling itself while it runs; dropping the handle on
    // the next tick is enough, and `reset()` here would delete a live source.
    socketRetryStop_ = true;
    SUPERIOR_INFO(diag::categories::Fcitx5).field("path", socketPath) << "UI socket ready";
    return true;
}

bool SuperiorAgent::ensureSpeechProcess() {
    if (speechProcess_ && speechProcess_->running()) {
        return true;
    }
    if (instance_ == nullptr) {
        return false;
    }
    if (!speechProcess_) {
        speechProcess_ = std::make_unique<SpeechProcess>(instance_);
    }
    std::string error;
    const bool started = speechProcess_->start(
        [this](std::string_view line) { handleSpeechEvent(line); }, error);
    if (!started) {
        SUPERIOR_INFO(diag::categories::SpeechRecognition)
            .field("error", error)
            << "speech recognition process is unavailable";
        return false;
    }

    configureSpeechProcess();
    return true;
}

void SuperiorAgent::configureSpeechProcess() {
    if (!speechProcess_ || !speechProcess_->running()) {
        return;
    }
    // Tell the child which model to load. Sending it after every (re)start keeps
    // the two in sync without the child reading our settings file.
    using namespace superior::speech_recognition;
    const ModelEntry &model = selectedModel(speechModelId_);
    const std::string resolvedModel = speech_recognition::modelPath(model.id).string();
    if (resolvedModel.empty()) {
        return;
    }
    Command configure;
    configure.kind = Command::Kind::Configure;
    configure.config.enginePath = resolveEngine();
    configure.config.modelPath = resolvedModel;
    configure.config.backend = model.backend;
    // Optional punctuation model: FireRedASR is a CTC model and emits no
    // punctuation at all, so a downloaded FireRedPunc is used when present.
    configure.config.puncPath = punctuationModelPath();
    configure.config.language = "zh";
    configure.config.threads = 0; // 0 = let the capability size it to the machine
    speechProcess_->send(encodeCommand(configure));
}

void SuperiorAgent::pollSpeechProcess() {
    // Whatever the cause — a dead child, a phase event lost on the way — a session
    // must not stay "busy" forever. This is the difference between "one utterance
    // failed" and "the input method is dead until Fcitx5 restarts".
    //
    // The limits are per phase on purpose: flushing a finished utterance takes a
    // second or two, so waiting a minute there would be a minute of nothing for
    // no reason, while recording legitimately lasts until the 60 s cap.
    const std::uint64_t stuckMs = [this] {
        switch (speech_.phase()) {
        case api::SpeechPhase::Recording:
            return std::uint64_t{65000};
        case api::SpeechPhase::Transcribing:
            return std::uint64_t{20000};
        case api::SpeechPhase::Cancelling:
            return std::uint64_t{5000};
        default:
            return std::uint64_t{0};
        }
    }();
    if (speech_.busy() && stuckMs != 0 && speechPhaseSinceMs_ != 0 &&
        steadyMs() - speechPhaseSinceMs_ > stuckMs) {
        SUPERIOR_WARN(diag::categories::SpeechRecognition)
            .field("phase", api::speechPhaseName(speech_.phase()))
            .field("idleMs", static_cast<std::int64_t>(steadyMs() - speechPhaseSinceMs_))
            << "speech session appears stuck; resetting the panel";
        speechPhaseSinceMs_ = steadyMs();
        speech_.fail("语音识别超时，已停止。");
        refresh(activeContext_);
        return;
    }
    if (!speechProcess_ || speechProcess_->running()) {
        return;
    }
    // A dead child is harmless while nothing is in flight: the next start simply
    // spawns a new one. It is fatal exactly when the user is waiting for it.
    if (!speech_.busy()) {
        return;
    }
    SUPERIOR_WARN(diag::categories::SpeechRecognition)
        << "speech capability exited mid-session; ending the session";
    speech_.fail("语音识别组件意外退出，请重试。");
    refresh(activeContext_);
}

void SuperiorAgent::refreshSpeechSetup() {
    using namespace superior::speech_recognition;
    const ModelEntry &entry = selectedModel(speechModelId_);
    const bool engine = enginePresent();
    const bool model = modelPresent(entry.id);
    speech_.setModel(entry.id, entry.displayName, entry.bytes);
    speech_.refreshSetup(engine, model);
}

bool SuperiorAgent::startSpeech() {
    if (!speech_.canStart()) {
        return false;
    }
    if (activeContext_ != nullptr &&
        activeContext_->capabilityFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive)) {
        speech_.fail("密码输入框不允许语音输入。");
        refresh(activeContext_);
        return false;
    }
    if (!ensureSpeechProcess()) {
        speech_.fail("语音识别组件未安装。");
        refresh(activeContext_);
        return false;
    }
    // Starting speech abandons a half-typed pinyin composition: two input modes
    // must never own the same commit.
    if (activeContext_ != nullptr) {
        activeContext_->reset();
        activeContext_->inputPanel().reset();
    }
    heldSpeech_.clear();
    speechHolding_ = false;
    if (speech_.beginRecording()) {
        if (!speechProcess_->send(encodeSpeechCommand(speech_recognition::Command::Kind::Start))) {
            // The pipe is gone: the child died between the readiness check and the
            // command. Saying so beats a panel that animates a microphone nobody is
            // listening to.
            speech_.fail("语音识别组件未响应，请重试。");
        }
    }
    refresh(activeContext_);
    return speech_.recording();
}

void SuperiorAgent::stopSpeech() {
    speech_.beginTranscribing();
    if (speechProcess_) {
        speechProcess_->send(encodeSpeechCommand(speech_recognition::Command::Kind::Stop));
    }
    refresh(activeContext_);
}

void SuperiorAgent::toggleSpeechByHotkey() {
    if (!speechRecognitionEnabled_ || !speechRecognitionHotkey_) {
        return;
    }
    if (speech_.recording()) {
        stopSpeech();
        return;
    }
    if (!speech_.visible()) {
        // Open *before* asking whether it can start: `canStart()` is false while
        // the panel is hidden, and a recording with no visible state would be a
        // hidden microphone. Opening also computes the setup state, so a missing
        // engine or model turns into an explanation instead of a dead hotkey.
        toggleSpeechPanel();
    }
    if (speech_.canStart()) {
        startSpeech();
    }
}

void SuperiorAgent::cancelSpeech(std::string_view reason) {
    if (!speech_.busy() || speech_.phase() == api::SpeechPhase::Cancelling) {
        return;
    }
    setSpeechPreedit({});
    heldSpeech_.clear();
    speechHolding_ = false;
    // The decision layer revokes the commit *first*: a `final` that is already
    // on the wire must not be typed after this point.
    speech_.cancel();
    if (speechProcess_) {
        speechProcess_->send(encodeSpeechCommand(speech_recognition::Command::Kind::Cancel));
    }
    SUPERIOR_INFO(diag::categories::SpeechRecognition)
        .field("reason", std::string(reason))
        .field("phase", api::speechPhaseName(speech_.phase()))
        << "speech cancelled";
    if (activeContext_ != nullptr) {
        refresh(activeContext_);
    }
}

void SuperiorAgent::toggleSpeechPanel() {
    if (speech_.visible()) {
        // Closing is a cancel: a transcript still in flight must not be typed
        // into the document after the user closed the panel.
        cancelSpeech("panel closed");
        speech_.hide();
        if (activeContext_ != nullptr) {
            refresh(activeContext_);
        }
        return;
    }

    using namespace superior::speech_recognition;
    const ModelEntry &entry = selectedModel(speechModelId_);
    const bool engine = enginePresent();
    const bool model = modelPresent(entry.id);
    speech_.setModel(entry.id, entry.displayName, entry.bytes);
    speech_.show(engine, model);
    if (engine && model) {
        ensureSpeechProcess();
    }
    if (activeContext_ != nullptr) {
        refresh(activeContext_);
    }
}

void SuperiorAgent::handleSpeechEvent(std::string_view line) {
    using namespace superior::speech_recognition;
    // The child's protocol is small; decoding it here keeps the decision layer
    // (`core::SpeechState`) free of JSON.
    const auto parsed = nlohmann::json::parse(std::string(line), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return;
    }
    const std::string kind = parsed.value("ev", std::string{});
    speechPhaseSinceMs_ = steadyMs();

    if (kind == "ready") {
        speech_.refreshSetup(true, true);
    } else if (kind == "phase") {
        // The child drives the session as well as the button does: it ends an
        // utterance on its own after a pause. Following its phase events is what
        // keeps `acceptFinal` from dropping a transcript whose stop nobody
        // clicked. Every transition is idempotent, so the UI-driven ones win.
        const std::string name = parsed.value("name", std::string{});
        if (name == "recording") {
            speech_.recordingStarted();
        } else if (name == "transcribing") {
            speech_.beginTranscribing();
        } else if (name == "idle") {
            speech_.idle();
        }
    } else if (kind == "level") {
        speech_.setLevel(parsed.value("rms", 0.0));
        speech_.setRemaining(parsed.value("remainingMs", std::int64_t{-1}));
    } else if (kind == "partial") {
        speech_.setPartial(parsed.value("text", std::string{}));
        // The draft lives in the composition line, not in the document: the user
        // sees words appear, and any rewrite happens before anything is real.
        setSpeechPreedit(speech_.partial());
    } else if (kind == "chunk") {
        // A finalized segment. Commit it now and keep listening — this is what
        // "writes while I speak" means.
        const std::string text = parsed.value("text", std::string{});
        if (speech_.acceptChunk(text)) {
            setSpeechPreedit({});
            commitSpeechText(speech_.lastText());
        }
    } else if (kind == "final") {
        const std::string text = parsed.value("text", std::string{});
        if (speech_.acceptFinal(text)) {
            setSpeechPreedit({});
            commitSpeechText(speech_.lastText());
        }
    } else if (kind == "error") {
        speech_.fail(parsed.value("message", std::string{"语音识别失败"}));
    }

    if (activeContext_ != nullptr) {
        refresh(activeContext_);
    }
}

void SuperiorAgent::setSpeechPreedit(std::string_view text) {
    if (instance_ == nullptr || activeContext_ == nullptr) {
        return;
    }
    fcitx::InputContext *inputContext = activeContext_;
    if (inputContext->capabilityFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive)) {
        return;
    }
    if (text.empty() && !speechPreeditActive_) {
        return;
    }
    // The draft belongs inline at the caret, in the application, exactly where the
    // committed text will land. Setting the *panel* preedit put it in our own
    // candidate strip instead — a second box next to the text field, which reads as
    // "the input method is writing somewhere else".
    //
    // Client preedit is only used when the application says it supports it; the
    // fallback is to show nothing and let the committed chunks appear, which is
    // still direct typing. The panel preedit stays empty either way, so our window
    // never duplicates the line (see `collect`).
    fcitx::Text draft;
    const bool inlinePreedit =
        !text.empty() && inputContext->capabilityFlags().test(fcitx::CapabilityFlag::Preedit) &&
        !inputContext->capabilityFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive);
    if (inlinePreedit) {
        draft = fcitx::Text(std::string(text));
        draft.setCursor(static_cast<int>(text.size()));
    }
    inputContext->inputPanel().setPreedit(fcitx::Text{});
    inputContext->inputPanel().setClientPreedit(draft);
    speechPreeditActive_ = inlinePreedit;
    inputContext->updatePreedit();
}

void SuperiorAgent::holdSpeechCommits() {
    if (!speech_.busy()) {
        return;
    }
    // The draft cannot be drawn anywhere either; drop it and let the next chunk
    // carry the text once a client is focused again.
    setSpeechPreedit({});
    speechHolding_ = true;
    SUPERIOR_DEBUG(diag::categories::SpeechRecognition)
        .field("held", static_cast<std::int64_t>(heldSpeech_.size()))
        << "no focused client; holding the transcript";
}

void SuperiorAgent::flushHeldSpeech() {
    if (heldSpeech_.empty()) {
        speechHolding_ = false;
        return;
    }
    const std::string text = std::move(heldSpeech_);
    heldSpeech_.clear();
    speechHolding_ = false;
    SUPERIOR_INFO(diag::categories::SpeechRecognition)
        .field("chars", static_cast<std::int64_t>(text.size()))
        << "flushing the transcript held while nothing was focused";
    commitSpeechText(text);
}

void SuperiorAgent::commitSpeechText(std::string_view text) {
    if (instance_ == nullptr || text.empty()) {
        return;
    }
    if (activeContext_ == nullptr || speechHolding_) {
        // Nowhere to type right now. Keep the words instead of losing them; the
        // next focus-in flushes this buffer.
        heldSpeech_ += text;
        return;
    }
    fcitx::InputContext *inputContext = activeContext_;
    if (inputContext->capabilityFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive)) {
        SUPERIOR_WARN(diag::categories::SpeechRecognition)
            << "refusing to commit a transcript into a password field";
        return;
    }
    // The engine may still hold a composition from before the panel opened.
    // Dropping it first keeps the transcript from being followed by stale pinyin.
    inputContext->reset();
    inputContext->inputPanel().reset();
    inputContext->commitString(std::string(text));
    inputContext->updatePreedit();
    SUPERIOR_INFO(diag::categories::SpeechRecognition)
        .field("chars", static_cast<std::int64_t>(text.size()))
        << "transcript committed";
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

    // The engine still owns a composition for the pinyin we completed. Drop it
    // first, otherwise the committed word would be followed by the remaining
    // pinyin as soon as the user types again. `InputContext::reset` reaches the
    // engine's `reset()`; `Instance::resetCompose` only resets XKB.
    inputContext->reset();
    inputContext->inputPanel().reset();
    inputContext->commitString(std::string(text));
    inputContext->updatePreedit();

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
